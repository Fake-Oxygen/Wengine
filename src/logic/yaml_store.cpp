// Versioned logic-workspace store (LOGIC-6; 006-versioned-workspace-v1).
//
// Load orchestration only: parse real YAML, validate the strict v1 schema
// via the SectionSpec table (see yaml_store_detail.hpp), then replay the
// collected rows through the engine seam (yaml_store_replay.cpp) with
// whole-graph topology validation. Save lives in yaml_store_save.cpp,
// row checkers + the section table in yaml_store_check.cpp.
//
// The wengine_logic_yaml TUs are the only ones allowed to know yaml-cpp
// exists (same seam as src/signal/lua_solver.cpp). No other engine, CLI,
// or test TU includes a yaml header; the public surface is the yaml-free
// include/wengine/logic/yaml_store.hpp.
//
// Save emits one canonical v1 byte layout. Load parses real YAML via
// yaml-cpp (comments, quoting, block vs flow style, multiline all
// accepted) then validates the strict v1 schema: exact version gate
// (new generation only — retired v1/v2 files are refused loudly, never
// loaded), no unknown keys anywhere, single-driver wires, known
// gates/pins, per-sequential-row inits, clocks/topology agreement, no
// cycles. Every diagnostic carries a 1-based `line N:` prefix from the
// offending node's mark.

#include "wengine/logic/yaml_store.hpp"

#include "wengine/logic/netlist.hpp"
#include "wengine/logic/types.hpp"

#include "yaml_store_detail.hpp"

#include <yaml-cpp/yaml.h>

#include <array>
#include <cstdint>
#include <string>

namespace wengine::logic::yaml {

namespace {

[[nodiscard]] bool load_impl(const std::string &text, Workspace &out, std::string &err) {
  using namespace detail;

  if (text.find_first_not_of(" \t\r\n#") == std::string::npos) {
    // Cheap empty/whitespace fast path; comment-only files fall through
    // to the parser and hit the same diagnostic below.
    bool only_ws = true;
    for (char c : text) {
      if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
        only_ws = false;
        break;
      }
    }
    if (only_ws) {
      err = "empty workspace (want 'version: 1' first)";
      return false;
    }
  }
  YAML::Node root;
  try {
    root = YAML::Load(text);
  } catch (const YAML::Exception &e) {
    const int line = e.mark.line;
    if (line < 0) {
      err = std::string("bad YAML: ") + e.msg;
    } else {
      err = "line " + std::to_string(static_cast<std::size_t>(line) + 1u) + ": " + e.msg;
    }
    return false;
  }
  if (!root || !root.IsDefined() || root.IsNull()) {
    err = "empty workspace (want 'version: 1' first)";
    return false;
  }
  if (!root.IsMap()) {
    err = at_line(root, "workspace root must read 'version: 1' first");
    return false;
  }
  // Root allowlist derives from the section table; the "(want ...)"
  // diagnostic derives from the same table (canonical save order).
  for (auto it = root.begin(); it != root.end(); ++it) {
    const std::string key = it->first.IsScalar() ? it->first.Scalar() : std::string{};
    if (key != "version" && key != "meta" && section_by_name(key) == nullptr) {
      err = at_line(it->first, "unknown workspace key '" + key + "' (want " + root_want_list() + ")");
      return false;
    }
  }
  const YAML::Node version = root["version"];
  const YAML::Node meta = root["meta"];
  if (!version || !version.IsDefined()) {
    err = "first line must read 'version: 1'";
    return false;
  }
  if (!version.IsScalar()) {
    err = at_line(version, "bad workspace version");
    return false;
  }
  {
    std::uint64_t v = 0;
    if (!parse_u64(version.Scalar(), v)) {
      err = at_line(version, "bad workspace version");
      return false;
    }
    if (v == 2) {
      err = at_line(version, "workspace version 2 is retired (want 1: re-save from a live session)");
      return false;
    }
    if (v != static_cast<std::uint64_t>(kWorkspaceVersion)) {
      err = at_line(version, "unsupported workspace version " + std::to_string(v) + " (want 1)");
      return false;
    }
  }
  // Required-section presence (table order; optional shared/pulls skip).
  // Ties/clocks absence means retired first-generation content, refused
  // as legacy — indistinguishable from it by the marker alone.
  for (const SectionSpec &s : kSections) {
    if (s.optional) {
      continue;
    }
    const YAML::Node sec = root[s.name];
    if (!sec || !sec.IsDefined()) {
      err = s.missing_err;
      return false;
    }
  }
  if (!meta || !meta.IsDefined()) {
    err = "want 'meta:' section";
    return false;
  }
  // Sequence-shape guards (table order; present-but-null reads empty).
  for (const SectionSpec &s : kSections) {
    const YAML::Node sec = root[s.name];
    if ((!sec || !sec.IsDefined()) || sec.IsNull()) {
      continue;
    }
    if (!sec.IsSequence()) {
      err = at_line(sec, s.seq_err);
      return false;
    }
  }
  if (!meta.IsMap()) {
    err = at_line(meta, "'meta:' must read a map");
    return false;
  }
  for (auto it = meta.begin(); it != meta.end(); ++it) {
    const std::string key = it->first.IsScalar() ? it->first.Scalar() : std::string{};
    if (key != "tool") {
      err = at_line(it->first, "unknown meta key (want 'tool: ...')");
      return false;
    }
  }
  const YAML::Node tool = meta["tool"];
  if (!tool || !tool.IsDefined() || !tool.IsScalar()) {
    err = "meta: missing 'tool: ...'";
    return false;
  }
  // Row collection: one generic loop over the table.
  Collected rows;
  for (const SectionSpec &s : kSections) {
    const YAML::Node sec = root[s.name];
    if ((!sec || !sec.IsDefined()) || !sec.IsSequence()) {
      continue;
    }
    if (!s.collect(sec, rows, s.row_err, err)) {
      return false;
    }
  }
  if (rows.gates.size() > kMaxGates) {
    err = at_line(root["gates"], "too many gates (max 1024)");
    return false;
  }

  // Replay in engine-seam dependency order (not file order): slots,
  // shared marks (wires read them), wires, ties (disjoint from marks),
  // pulls (after ties and wires).
  Netlist tmp{};
  constexpr const char *kReplayOrder[5] = {"gates", "shared", "wires", "ties", "pulls"};
  for (const char *name : kReplayOrder) {
    const SectionSpec *s = section_by_name(name);
    if (!s->replay(rows, root[s->name], tmp, err)) {
      return false;
    }
  }
  if (!validate_topology(tmp, err)) {
    return false;
  }
  std::array<LogicValue, kMaxGates> inits{};
  if (!apply_inits_and_validate_clocks(rows, tmp, inits, err)) {
    return false;
  }
  out.netlist = tmp;
  out.init = inits;
  return true;
}

} // namespace

bool load_text(const std::string &text, Workspace &out, std::string &err) {
  try {
    return load_impl(text, out, err);
  } catch (const YAML::Exception &e) {
    const int line = e.mark.line;
    if (line < 0) {
      err = std::string("bad YAML: ") + e.msg;
    } else {
      err = "line " + std::to_string(static_cast<std::size_t>(line) + 1u) + ": " + e.msg;
    }
    return false;
  } catch (const std::exception &e) {
    err = std::string("bad YAML: ") + e.what();
    return false;
  }
}

} // namespace wengine::logic::yaml
