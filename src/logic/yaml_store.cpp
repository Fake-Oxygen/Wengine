// Versioned logic-workspace store (LOGIC-6; 006-versioned-workspace-v1).
//
// THE single TU allowed to know yaml-cpp exists (same seam as
// src/signal/lua_solver.cpp). No other engine, CLI, or test TU includes a
// yaml header; the public surface is the yaml-free
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

#include "wengine/logic/clock_domains.hpp"
#include "wengine/logic/eval.hpp"
#include "wengine/logic/netlist.hpp"
#include "wengine/logic/seq_state.hpp"
#include "wengine/logic/topo.hpp"
#include "wengine/logic/types.hpp"

#include <yaml-cpp/yaml.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wengine::logic::yaml {

namespace {

[[nodiscard]] std::string lower_kind(GateKind k) {
  std::string s(gate_kind_name(k));
  for (char &c : s) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - ('A' - 'a'));
    }
  }
  return s;
}

[[nodiscard]] std::string gate_name(const Netlist &nl, std::uint16_t slot) {
  std::string s(gate_kind_name(nl.gates[slot].kind));
  s += std::to_string(nl.gates[slot].counter);
  return s;
}

[[nodiscard]] std::string pin_text(const Netlist &nl, std::uint16_t slot, bool is_output, std::uint8_t input, bool negated = false) {
  std::string s = gate_name(nl, slot);
  s += '.';
  const Gate &g = nl.gates[slot];
  if (is_output && is_sequential_kind(g.kind)) {
    s += negated ? "NQ" : "Q";
  } else if (is_output) {
    s += 'Y';
  } else if (is_sequential_kind(g.kind)) {
    // Canonical named inputs: DFF D/CLK (+RST/SET/EN on full-control
    // flops), DLATCH D/EN, JK J/K/CLK, T T/CLK, SR S/R.
    if (g.kind == GateKind::kJk) {
      s += (input == 0) ? "J" : (input == 1) ? "K" : "CLK";
    } else if (g.kind == GateKind::kT) {
      s += (input == 0) ? "T" : "CLK";
    } else if (g.kind == GateKind::kSrlatch) {
      s += (input == 0) ? "S" : "R";
    } else if (input == 0) {
      s += 'D';
    } else if (g.kind == GateKind::kDff && input >= 2) {
      s += (input == 2) ? "RST" : (input == 3) ? "SET" : "EN";
    } else {
      s += (g.kind == GateKind::kDff) ? "CLK" : "EN";
    }
  } else {
    s += std::to_string(static_cast<unsigned>(input) + 1u);
  }
  return s;
}

// Strict unsigned decimal: no signs/spaces, digits only, saturates on
// overflow. Mirrors the former hand parser (quoted "2" and plain 2 both
// surface as Scalar "2", so one path covers both spellings).
[[nodiscard]] bool parse_u64(std::string_view text, std::uint64_t &out) {
  if (text.empty()) {
    return false;
  }
  std::uint64_t v = 0;
  for (char c : text) {
    if (c < '0' || c > '9') {
      return false;
    }
    const auto d = static_cast<std::uint64_t>(c - '0');
    if (v > (0xFFFFFFFFFFFFFFFFULL - d) / 10ULL) {
      return false;
    }
    v = v * 10ULL + d;
  }
  out = v;
  return true;
}

[[nodiscard]] std::size_t line_of(const YAML::Node &n) {
  const int line = n.Mark().line;
  if (line < 0) {
    return 1;
  }
  return static_cast<std::size_t>(line) + 1u;
}

[[nodiscard]] std::string at_line(const YAML::Node &n, const std::string &msg) {
  return "line " + std::to_string(line_of(n)) + ": " + msg;
}

struct GateRow {
  std::string id;
  GateKind kind = GateKind::kAnd;
  std::uint8_t inputs = 0;
  std::uint8_t ignored_mask = 0;
  bool ignore_output = false;
  bool ignore_nq = false; // optional key, sequential gates only
  bool has_init = false;  // optional key, sequential gates only
  LogicValue init = LogicValue::kLow;
  std::uint64_t delay_ps = 0;
  std::uint32_t counter = 0;
};

struct WireRow {
  std::string from;
  std::string to;
  std::size_t line = 1;
};

struct SharedRow {
  std::string net;
  std::size_t line = 1;
};

struct TieRow {
  std::string a;
  std::string b;
  std::size_t line = 1;
};

struct ClockRow {
  std::string name;
  std::size_t line = 1;
};

[[nodiscard]] bool check_gate_map(const YAML::Node &g, GateRow &row, std::string &err) {
  constexpr const char *kWant[6] = {"id", "kind", "inputs", "ignore", "ignore_output", "delay_ps"};
  for (auto it = g.begin(); it != g.end(); ++it) {
    const std::string key = it->first.IsScalar() ? it->first.Scalar() : std::string{};
    bool known = (key == "ignore_nq") || (key == "init"); // optional 7th/8th keys (sequential gates)
    for (const char *w : kWant) {
      if (key == w) {
        known = true;
        break;
      }
    }
    if (!known) {
      err = at_line(it->first, "unknown gate key '" + key + "' (want id|kind|inputs|ignore|ignore_output|delay_ps|ignore_nq|init)");
      return false;
    }
  }
  for (const char *w : kWant) {
    const YAML::Node v = g[w];
    if (!v || !v.IsDefined()) {
      err = at_line(g, std::string("gate entry needs 6 fields (id, kind, inputs, ignore, ignore_output, delay_ps), missing '") + w + "'");
      return false;
    }
  }
  const YAML::Node id = g["id"];
  const YAML::Node kind = g["kind"];
  const YAML::Node inputs = g["inputs"];
  const YAML::Node ignore = g["ignore"];
  const YAML::Node ignore_output = g["ignore_output"];
  const YAML::Node delay = g["delay_ps"];
  if (!id.IsScalar() || id.Scalar().empty()) {
    err = at_line(id.IsDefined() ? id : g, "empty gate id");
    return false;
  }
  row.id = id.Scalar();
  if (!kind.IsScalar()) {
    err = at_line(kind, "bad gate kind (want AND|OR|NAND|NOR|XOR|XNOR|NOT|BUF|DFF|DLATCH|JK|T|SRLATCH)");
    return false;
  }
  const auto parsed_kind = parse_gate_kind(kind.Scalar());
  if (!parsed_kind.ok()) {
    err = at_line(kind, "bad gate kind (want AND|OR|NAND|NOR|XOR|XNOR|NOT|BUF|DFF|DLATCH|JK|T|SRLATCH)");
    return false;
  }
  row.kind = parsed_kind.value;
  // NQ defaults to ignored on sequential gates (explicit `ignore_nq: false`
  // re-exposes, same as the `use` verb); combinational gates never read it.
  row.ignore_nq = is_sequential_kind(row.kind);
  if (!inputs.IsScalar()) {
    err = at_line(inputs, "bad input count (want 1..8)");
    return false;
  }
  std::uint64_t n = 0;
  if (!parse_u64(inputs.Scalar(), n) || n == 0 || n > kMaxInputs) {
    err = at_line(inputs, "bad input count (want 1..8)");
    return false;
  }
  row.inputs = static_cast<std::uint8_t>(n);
  if (!valid_fanin(row.kind, row.inputs)) {
    err = at_line(inputs, "bad fan-in for kind");
    return false;
  }
  // Id must read `<KIND><counter>` with the same kind (case-insensitive).
  const std::string kname(gate_kind_name(row.kind));
  if (row.id.size() <= kname.size()) {
    err = at_line(id, "gate id must read KIND+counter (e.g. AND1)");
    return false;
  }
  for (std::size_t i = 0; i < kname.size(); ++i) {
    char a = row.id[i];
    if (a >= 'a' && a <= 'z') {
      a = static_cast<char>(a - ('a' - 'A'));
    }
    if (a != kname[i]) {
      err = at_line(id, "gate id/kind mismatch");
      return false;
    }
  }
  std::uint64_t counter = 0;
  if (!parse_u64(std::string_view(row.id.data() + kname.size(), row.id.size() - kname.size()), counter) ||
      counter == 0 || counter > 0xFFFFFFFFULL) {
    err = at_line(id, "gate id needs a counter >= 1 (e.g. AND1)");
    return false;
  }
  row.counter = static_cast<std::uint32_t>(counter);
  // ignore: sequence of 1-based inputs, ascending, unique, in range.
  // Accepts flow `[2, 3]` and block lists alike (the multiline win over
  // the old canonical-subset parser).
  if (!ignore.IsSequence()) {
    err = at_line(ignore, "ignore must read [i, ...]");
    return false;
  }
  row.ignored_mask = 0;
  unsigned prev = 0;
  for (std::size_t i = 0; i < ignore.size(); ++i) {
    const YAML::Node item = ignore[i];
    if (!item.IsScalar()) {
      err = at_line(item.IsDefined() ? item : ignore, "ignore index out of range");
      return false;
    }
    std::uint64_t idx = 0;
    if (!parse_u64(item.Scalar(), idx) || idx == 0 || idx > row.inputs) {
      err = at_line(item, "ignore index out of range");
      return false;
    }
    if (idx <= prev) {
      err = at_line(item, "ignore indices must ascend");
      return false;
    }
    prev = static_cast<unsigned>(idx);
    row.ignored_mask = static_cast<std::uint8_t>(row.ignored_mask | (1u << (idx - 1u)));
  }
  if (!ignore_output.IsScalar()) {
    err = at_line(ignore_output, "ignore_output must be true|false");
    return false;
  }
  if (ignore_output.Scalar() == "true") {
    row.ignore_output = true;
  } else if (ignore_output.Scalar() == "false") {
    row.ignore_output = false;
  } else {
    err = at_line(ignore_output, "ignore_output must be true|false");
    return false;
  }
  // Optional 7th key: missing means false (all pre-NQ files).
  const YAML::Node ignore_nq = g["ignore_nq"];
  if (ignore_nq && ignore_nq.IsDefined()) {
    if (!ignore_nq.IsScalar()) {
      err = at_line(ignore_nq, "ignore_nq must be true|false");
      return false;
    }
    if (ignore_nq.Scalar() == "true") {
      row.ignore_nq = true;
    } else if (ignore_nq.Scalar() == "false") {
      row.ignore_nq = false;
    } else {
      err = at_line(ignore_nq, "ignore_nq must be true|false");
      return false;
    }
  }
  // Optional 8th key: per-sequential-row power-on value. Missing means
  // Low (hand-written files stay small); combinational rows must not
  // carry it.
  const YAML::Node init = g["init"];
  if (init && init.IsDefined()) {
    if (!is_sequential_kind(row.kind)) {
      err = at_line(init, "init needs a sequential gate (got '" + row.id + "')");
      return false;
    }
    if (!init.IsScalar()) {
      err = at_line(init, "bad init value (want 0|1)");
      return false;
    }
    if (init.Scalar() == "1") {
      row.has_init = true;
      row.init = LogicValue::kHigh;
    } else if (init.Scalar() == "0") {
      row.has_init = true;
      row.init = LogicValue::kLow;
    } else {
      err = at_line(init, "bad init value (want 0|1)");
      return false;
    }
  }
  if (!delay.IsScalar() || !parse_u64(delay.Scalar(), row.delay_ps)) {
    err = at_line(delay.IsDefined() ? delay : g, "bad delay_ps");
    return false;
  }
  return true;
}

[[nodiscard]] bool check_wire_map(const YAML::Node &w, WireRow &row, std::string &err) {
  for (auto it = w.begin(); it != w.end(); ++it) {
    const std::string key = it->first.IsScalar() ? it->first.Scalar() : std::string{};
    if (key != "from" && key != "to") {
      err = at_line(it->first, "wire fields must read 'from' then 'to'");
      return false;
    }
  }
  const YAML::Node from = w["from"];
  const YAML::Node to = w["to"];
  if (!from || !from.IsDefined() || !to || !to.IsDefined()) {
    err = at_line(w, "wire entry needs 2 fields (from, to)");
    return false;
  }
  if (!from.IsScalar() || from.Scalar().empty() || !to.IsScalar() || to.Scalar().empty()) {
    err = at_line(w, "empty wire pin");
    return false;
  }
  row.from = from.Scalar();
  row.to = to.Scalar();
  row.line = line_of(w);
  return true;
}

[[nodiscard]] bool check_tie_map(const YAML::Node &t, TieRow &row, std::string &err) {
  for (auto it = t.begin(); it != t.end(); ++it) {
    const std::string key = it->first.IsScalar() ? it->first.Scalar() : std::string{};
    if (key != "a" && key != "b") {
      err = at_line(it->first, "tie fields must read 'a' then 'b'");
      return false;
    }
  }
  const YAML::Node a = t["a"];
  const YAML::Node b = t["b"];
  if (!a || !a.IsDefined() || !b || !b.IsDefined()) {
    err = at_line(t, "tie entry needs 2 fields (a, b)");
    return false;
  }
  if (!a.IsScalar() || a.Scalar().empty() || !b.IsScalar() || b.Scalar().empty()) {
    err = at_line(t, "empty tie pin");
    return false;
  }
  row.a = a.Scalar();
  row.b = b.Scalar();
  row.line = line_of(t);
  return true;
}

[[nodiscard]] bool check_clocks_map(const YAML::Node &c, ClockRow &row, std::string &err) {
  for (auto it = c.begin(); it != c.end(); ++it) {
    const std::string key = it->first.IsScalar() ? it->first.Scalar() : std::string{};
    if (key != "name") {
      err = at_line(it->first, "clock fields must read 'name'");
      return false;
    }
  }
  const YAML::Node name = c["name"];
  if (!name || !name.IsDefined()) {
    err = at_line(c, "clock entry needs 1 field (name)");
    return false;
  }
  if (!name.IsScalar() || name.Scalar().empty()) {
    err = at_line(c, "empty clock name");
    return false;
  }
  row.name = name.Scalar();
  row.line = line_of(c);
  return true;
}

// 011: one `{net: PIN}` row per marked input. The mark is the opt-in;
// the attachment count derives from the repeated `wires:` rows.
[[nodiscard]] bool check_shared_map(const YAML::Node &n, SharedRow &row, std::string &err) {
  for (auto it = n.begin(); it != n.end(); ++it) {
    const std::string key = it->first.IsScalar() ? it->first.Scalar() : std::string{};
    if (key != "net") {
      err = at_line(it->first, "unknown shared key '" + key + "' (want 'net: ...')");
      return false;
    }
  }
  const YAML::Node net = n["net"];
  if (!net || !net.IsDefined()) {
    err = at_line(n, "shared entry needs 1 field (net)");
    return false;
  }
  if (!net.IsScalar() || net.Scalar().empty()) {
    err = at_line(n, "empty shared net");
    return false;
  }
  row.net = net.Scalar();
  row.line = line_of(n);
  return true;
}

[[nodiscard]] bool load_impl(const std::string &text, Workspace &out, std::string &err) {
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
  for (auto it = root.begin(); it != root.end(); ++it) {
    const std::string key = it->first.IsScalar() ? it->first.Scalar() : std::string{};
    if (key != "version" && key != "gates" && key != "wires" && key != "shared" && key != "ties" && key != "clocks" &&
        key != "meta") {
      err = at_line(it->first, "unknown workspace key '" + key + "' (want version|gates|wires|shared|ties|clocks|meta)");
      return false;
    }
  }
  const YAML::Node version = root["version"];
  const YAML::Node gates = root["gates"];
  const YAML::Node wires = root["wires"];
  const YAML::Node shared = root["shared"];
  const YAML::Node ties = root["ties"];
  const YAML::Node clocks = root["clocks"];
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
  if (!gates || !gates.IsDefined()) {
    err = "want 'gates:' section";
    return false;
  }
  if (!wires || !wires.IsDefined()) {
    err = "want 'wires:' section";
    return false;
  }
  if (!ties || !ties.IsDefined()) {
    // A `version: 1` file without `ties:` is a retired first-generation
    // file (indistinguishable from it by the marker alone): refuse as
    // legacy, never misload.
    err = "legacy workspace version 1 is retired (missing 'ties:' section, want 1)";
    return false;
  }
  if (!clocks || !clocks.IsDefined()) {
    // Same generation-marker collision as `ties:` above: a `version: 1`
    // file without `clocks:` is retired first-generation content.
    err = "legacy workspace version 1 is retired (missing 'clocks:' section, want 1)";
    return false;
  }
  if (!meta || !meta.IsDefined()) {
    err = "want 'meta:' section";
    return false;
  }
  if (!gates.IsSequence() && !gates.IsNull()) {
    err = at_line(gates, "'gates:' must read a sequence");
    return false;
  }
  if (!wires.IsSequence() && !wires.IsNull()) {
    err = at_line(wires, "'wires:' must read a sequence");
    return false;
  }
  // 011: `shared:` is optional (absent = no marks, pre-011 files load
  // byte-identically); present-but-malformed is loud, never tolerated.
  if (shared && shared.IsDefined() && !shared.IsSequence() && !shared.IsNull()) {
    err = at_line(shared, "'shared:' must read a sequence");
    return false;
  }
  if (ties && ties.IsDefined() && !ties.IsSequence() && !ties.IsNull()) {
    err = at_line(ties, "'ties:' must read a sequence");
    return false;
  }
  if (!clocks.IsSequence() && !clocks.IsNull()) {
    err = at_line(clocks, "'clocks:' must read a sequence");
    return false;
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

  std::vector<GateRow> gate_rows;
  std::vector<WireRow> wire_rows;
  std::vector<SharedRow> shared_rows;
  std::vector<TieRow> tie_rows;
  std::vector<ClockRow> clock_rows;
  if (gates.IsSequence()) {
    gate_rows.reserve(gates.size());
    for (std::size_t i = 0; i < gates.size(); ++i) {
      const YAML::Node g = gates[i];
      if (!g.IsMap()) {
        err = at_line(g, "gate entry must read '{id: ..., kind: ..., inputs: ..., ignore: [...], ignore_output: ..., [ignore_nq: ...,] delay_ps: ...[, init: ...]}'");
        return false;
      }
      GateRow row;
      if (!check_gate_map(g, row, err)) {
        return false;
      }
      gate_rows.push_back(std::move(row));
    }
  }
  if (wires.IsSequence()) {
    wire_rows.reserve(wires.size());
    for (std::size_t i = 0; i < wires.size(); ++i) {
      const YAML::Node w = wires[i];
      if (!w.IsMap()) {
        err = at_line(w, "wire entry must read '{from: ..., to: ...}'");
        return false;
      }
      WireRow row;
      if (!check_wire_map(w, row, err)) {
        return false;
      }
      wire_rows.push_back(std::move(row));
    }
  }
  if (shared && shared.IsDefined() && shared.IsSequence()) {
    shared_rows.reserve(shared.size());
    for (std::size_t i = 0; i < shared.size(); ++i) {
      const YAML::Node n = shared[i];
      if (!n.IsMap()) {
        err = at_line(n, "shared entry must read '{net: ...}'");
        return false;
      }
      SharedRow row;
      if (!check_shared_map(n, row, err)) {
        return false;
      }
      shared_rows.push_back(std::move(row));
    }
  }
  if (ties && ties.IsDefined() && ties.IsSequence()) {
    tie_rows.reserve(ties.size());
    for (std::size_t i = 0; i < ties.size(); ++i) {
      const YAML::Node t = ties[i];
      if (!t.IsMap()) {
        err = at_line(t, "tie entry must read '{a: ..., b: ...}'");
        return false;
      }
      TieRow row;
      if (!check_tie_map(t, row, err)) {
        return false;
      }
      tie_rows.push_back(std::move(row));
    }
  }
  if (clocks.IsSequence()) {
    clock_rows.reserve(clocks.size());
    for (std::size_t i = 0; i < clocks.size(); ++i) {
      const YAML::Node c = clocks[i];
      if (!c.IsMap()) {
        err = at_line(c, "clock entry must read '{name: ...}'");
        return false;
      }
      ClockRow row;
      if (!check_clocks_map(c, row, err)) {
        return false;
      }
      clock_rows.push_back(std::move(row));
    }
  }
  if (gate_rows.size() > kMaxGates) {
    err = at_line(gates, "too many gates (max 1024)");
    return false;
  }

  Netlist tmp{};
  for (std::size_t i = 0; i < gate_rows.size(); ++i) {
    const GateRow &g = gate_rows[i];
    for (std::size_t j = 0; j < i; ++j) {
      if (gate_rows[j].kind == g.kind && gate_rows[j].counter == g.counter) {
        err = "line " + std::to_string(wire_rows.empty() ? line_of(gates[i]) : line_of(gates[i])) + ": duplicate gate id '" +
              g.id + "'";
        return false;
      }
    }
    Gate &slot = tmp.gates[i];
    slot.live = true;
    slot.kind = g.kind;
    slot.counter = g.counter;
    slot.input_count = g.inputs;
    slot.ignored_mask = g.ignored_mask;
    slot.ignore_output = g.ignore_output;
    slot.ignore_nq_output = g.ignore_nq;
    slot.output_fanout = 0;
    slot.nq_fanout = 0;
    slot.propagation_delay_ps = g.delay_ps;
    for (std::size_t k = 0; k < kMaxInputs; ++k) {
      slot.drivers[k] = kNoDriver;
    }
    const std::uint8_t ord = kind_ordinal(g.kind);
    if (tmp.next_counter[ord] <= g.counter) {
      tmp.next_counter[ord] = g.counter + 1u;
      if (tmp.next_counter[ord] == 0) {
        tmp.next_counter[ord] = 1;
      }
    }
  }
  tmp.live_count = static_cast<std::uint16_t>(gate_rows.size());
  tmp.dirty = true;
  tmp.topo_size = 0;

  // 011: marks land before wire replay (in slot order of first
  // appearance — replay order stays file order per the contract, and
  // the array state converges deterministically regardless).
  // Mark validation mirrors the engine seam exactly: live inputs,
  // untied destinations (ties do not exist yet at this point, so the
  // loader's tie-time check below guards the other direction).
  for (const SharedRow &sr : shared_rows) {
    const std::string at = "line " + std::to_string(sr.line) + ": ";
    const auto pin = parse_pin(tmp, sr.net);
    if (!pin.ok() || pin.value.is_output) {
      err = at + "bad shared net '" + sr.net + "'";
      return false;
    }
    if (is_shared_input(tmp, pin.value.slot, pin.value.input)) {
      err = at + "duplicate shared net '" + sr.net + "'";
      return false;
    }
    if (!mark_shared(tmp, pin.value.slot, pin.value.input).ok()) {
      err = at + "bad shared net '" + sr.net + "'";
      return false;
    }
  }

  for (const WireRow &w : wire_rows) {
    const std::string at = "line " + std::to_string(w.line) + ": ";
    const auto from = parse_pin(tmp, w.from);
    if (!from.ok()) {
      if (from.error == std::string_view("unknown gate")) {
        err = at + "unknown gate in wire source '" + w.from + "'";
      } else {
        err = at + "bad wire source '" + w.from + "'";
      }
      return false;
    }
    const auto to = parse_pin(tmp, w.to);
    if (!to.ok()) {
      if (to.error == std::string_view("unknown gate")) {
        err = at + "unknown gate in wire destination '" + w.to + "'";
      } else {
        err = at + "bad wire destination '" + w.to + "'";
      }
      return false;
    }
    if (!from.value.is_output || to.value.is_output) {
      err = at + "wire needs FROM.out TO.in (got '" + w.from + "' -> '" + w.to + "')";
      return false;
    }
    // NQ sources exist on sequential gates only (same rule as
    // logic::wire_pins — slot-built refs stay loud, never silent).
    if (from.value.negated && !is_sequential_kind(tmp.gates[from.value.slot].kind)) {
      err = at + "bad wire source '" + w.from + "'";
      return false;
    }
    Gate &dst = tmp.gates[to.value.slot];
    const bool shared_marked = is_shared_input(tmp, to.value.slot, to.value.input);
    if (dst.drivers[to.value.input] != kNoDriver && !shared_marked) {
      err = at + "input already wired '" + w.to + "'";
      return false;
    }
    // Q-boundary (same rule as logic::wire): an edge into a
    // sequential-gate input can never close a combinational cycle, so
    // file order never affects acceptance. The whole-graph validator at
    // the end stays authoritative.
    if (!is_sequential_kind(dst.kind) &&
        topo::reaches(tmp, to.value.slot, from.value.slot, EdgeFilter::kCombOnly)) {
      err = at + "wire closes a combinational cycle '" + w.from + "' -> '" + w.to + "'";
      return false;
    }
    if (dst.drivers[to.value.input] != kNoDriver) {
      // 011: repeated `to:` on a marked input appends an extra — the
      // same cap the engine enforces, loudly, never truncated.
      const std::uint8_t extra_n = shared_extra_count(tmp, to.value.slot, to.value.input);
      if (extra_n >= kMaxSharedExtra) {
        err = at + "shared net full (cap 4) '" + w.to + "'";
        return false;
      }
      const std::size_t flat = tie_flat(to.value.slot, to.value.input);
      tmp.extra_drivers[flat * kMaxSharedExtra + extra_n] = from.value.slot;
      const std::uint8_t inv = from.value.negated ? 1 : 0;
      tmp.extra_inv[flat * kMaxSharedExtra + extra_n] = inv;
      tmp.shared_meta[flat] = static_cast<std::uint8_t>((tmp.shared_meta[flat] & 0x01u) | ((extra_n + 1u) << 1));
      if (inv != 0) {
        ++tmp.gates[from.value.slot].nq_fanout;
      } else {
        ++tmp.gates[from.value.slot].output_fanout;
      }
      continue;
    }
    dst.drivers[to.value.input] = from.value.slot;
    if (from.value.negated) {
      ++tmp.gates[from.value.slot].nq_fanout;
      tmp.driver_inv[tie_flat(to.value.slot, to.value.input)] = 1;
    } else {
      ++tmp.gates[from.value.slot].output_fanout;
    }
  }
  for (const TieRow &t : tie_rows) {
    const std::string at = "line " + std::to_string(t.line) + ": ";
    const auto a = parse_pin(tmp, t.a);
    if (!a.ok()) {
      if (a.error == std::string_view("unknown gate")) {
        err = at + "unknown gate in tie pin '" + t.a + "'";
      } else {
        err = at + "bad tie pin '" + t.a + "'";
      }
      return false;
    }
    const auto b = parse_pin(tmp, t.b);
    if (!b.ok()) {
      if (b.error == std::string_view("unknown gate")) {
        err = at + "unknown gate in tie pin '" + t.b + "'";
      } else {
        err = at + "bad tie pin '" + t.b + "'";
      }
      return false;
    }
    if (a.value.is_output || b.value.is_output) {
      err = at + "tie needs two input pins (got '" + t.a + "' + '" + t.b + "')";
      return false;
    }
    if (a.value.slot == b.value.slot && a.value.input == b.value.input) {
      err = at + "tie needs two distinct pins (got '" + t.a + "' twice)";
      return false;
    }
    const Gate &ga = tmp.gates[a.value.slot];
    const Gate &gb = tmp.gates[b.value.slot];
    if (ga.drivers[a.value.input] != kNoDriver || gb.drivers[b.value.input] != kNoDriver) {
      err = at + "tie needs dangling pins (unwire first)";
      return false;
    }
    if (((ga.ignored_mask & static_cast<std::uint8_t>(1u << a.value.input)) != 0) ||
        ((gb.ignored_mask & static_cast<std::uint8_t>(1u << b.value.input)) != 0)) {
      err = at + "tie needs non-ignored pins (use first)";
      return false;
    }
    // 011: marks and ties are disjoint per flat — a marked input is
    // never tied (the other direction, mark-on-tied, is refused by the
    // engine seam at authoring time).
    if (is_shared_input(tmp, a.value.slot, a.value.input) || is_shared_input(tmp, b.value.slot, b.value.input)) {
      err = at + "shared net cannot be tied (unshare it first)";
      return false;
    }
    if (!tie_pins(tmp, a.value, b.value).ok()) {
      err = at + "bad tie '" + t.a + "' + '" + t.b + "'";
      return false;
    }
  }
  tmp.dirty = true;
  tmp.topo_size = 0;
  // Flop-aware cycle check (mirrors the wire verb): combinational loops
  // always fail; feedback through sequential gates validates sequentially.
  if (has_sequential_gates(tmp)) {
    if (!topo::validate(tmp, EdgeFilter::kCombOnly).ok()) {
      err = "workspace topology error";
      return false;
    }
  } else if (!topo::rebuild(tmp, EdgeFilter::kAll).ok()) {
    err = "workspace topology error";
    return false;
  }
  // Per-slot power-on values: explicit `init:` keys land on their gate's
  // slot; every other slot reads Low. `check_gate_map` already rejected
  // `init` on combinational rows.
  std::array<LogicValue, kMaxGates> inits{};
  inits.fill(LogicValue::kLow);
  for (std::size_t i = 0; i < gate_rows.size(); ++i) {
    if (gate_rows[i].has_init) {
      inits[i] = gate_rows[i].init;
    }
  }
  // Pinned clock domains: the stored names must equal the derived set
  // exactly (order-insensitive) — membership itself stays derived from
  // the topology above, so any drift fails loudly instead of silently
  // re-deriving.
  {
    const std::vector<ClockDomain> derived = derive_clock_domains(tmp);
    for (const ClockRow &c : clock_rows) {
      std::size_t hits = 0;
      for (const ClockRow &d : clock_rows) {
        if (d.name == c.name) {
          ++hits;
        }
      }
      if (hits > 1) {
        err = "line " + std::to_string(c.line) + ": duplicate clock domain '" + c.name + "'";
        return false;
      }
      bool found = false;
      for (const ClockDomain &d : derived) {
        if (d.name == c.name) {
          found = true;
          break;
        }
      }
      if (!found) {
        err = "line " + std::to_string(c.line) + ": unknown clock domain '" + c.name +
              "' (not derived from this workspace)";
        return false;
      }
    }
    if (clock_rows.size() != derived.size()) {
      for (const ClockDomain &d : derived) {
        bool found = false;
        for (const ClockRow &c : clock_rows) {
          if (c.name == d.name) {
            found = true;
            break;
          }
        }
        if (!found) {
          err = "workspace topology derives clock domain '" + d.name + "' with no 'clocks:' entry";
          return false;
        }
      }
      err = "workspace topology derives no clock domain for a stored 'clocks:' entry";
      return false;
    }
  }
  out.netlist = tmp;
  out.init = inits;
  return true;
}

} // namespace

void save_text(const Netlist &nl, const std::array<LogicValue, kMaxGates> &inits, std::string &out) {
  out += "version: 1\n";
  out += "gates:\n";
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    const Gate &g = nl.gates[s];
    if (!g.live) {
      continue;
    }
    out += "  - {id: ";
    out += gate_name(nl, s);
    out += ", kind: ";
    out += lower_kind(g.kind);
    out += ", inputs: ";
    out += std::to_string(static_cast<unsigned>(g.input_count));
    out += ", ignore: [";
    bool first = true;
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      if ((g.ignored_mask & static_cast<std::uint8_t>(1u << i)) == 0) {
        continue;
      }
      if (!first) {
        out += ", ";
      }
      first = false;
      out += std::to_string(static_cast<unsigned>(i) + 1u);
    }
    out += "], ignore_output: ";
    out += g.ignore_output ? "true" : "false";
    if (is_sequential_kind(g.kind)) {
      // Sequential-only 7th key (comb rows keep the 6-field canonical
      // shape); the power-on value trails last (see below).
      out += ", ignore_nq: ";
      out += g.ignore_nq_output ? "true" : "false";
    }
    out += ", delay_ps: ";
    out += std::to_string(g.propagation_delay_ps);
    if (is_sequential_kind(g.kind)) {
      // Trailing power-on value from the caller's per-slot inits.
      out += ", init: ";
      out += (inits[s] == LogicValue::kHigh) ? "1" : "0";
    }
    out += "}\n";
  }
  out += "wires:\n";
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    const Gate &g = nl.gates[s];
    if (!g.live) {
      continue;
    }
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      const std::uint16_t drv = g.drivers[i];
      if (drv == kNoDriver) {
        continue;
      }
      out += "  - {from: ";
      const bool neg = nl.driver_inv[tie_flat(s, i)] != 0;
      out += pin_text(nl, drv, true, 0, neg);
      out += ", to: ";
      out += pin_text(nl, s, false, i);
      out += "}\n";
      // 011: every extra attachment is a repeated `to:` row, in compact
      // index order (legal on load iff the input is marked, which the
      // `shared:` section below guarantees).
      const std::size_t flat = tie_flat(s, i);
      const std::uint8_t extra_n = shared_extra_flat(nl, flat);
      for (std::uint8_t j = 0; j < extra_n; ++j) {
        const std::uint16_t edrv = nl.extra_drivers[flat * kMaxSharedExtra + j];
        const bool eneg = nl.extra_inv[flat * kMaxSharedExtra + j] != 0;
        out += "  - {from: ";
        out += pin_text(nl, edrv, true, 0, eneg);
        out += ", to: ";
        out += pin_text(nl, s, false, i);
        out += "}\n";
      }
    }
  }
  // 011: optional `shared:` section, emitted iff >= 1 mark exists, one
  // `{net: PIN}` row per marked input in slot order. Absent on read
  // means no marks (pre-011 files load byte-identically).
  {
    bool any_mark = false;
    for (std::uint16_t s = 0; s < kMaxGates && !any_mark; ++s) {
      if (!nl.gates[s].live) {
        continue;
      }
      for (std::uint8_t i = 0; i < nl.gates[s].input_count; ++i) {
        if (is_shared_flat(nl, tie_flat(s, i))) {
          any_mark = true;
          break;
        }
      }
    }
    if (any_mark) {
      out += "shared:\n";
      for (std::uint16_t s = 0; s < kMaxGates; ++s) {
        if (!nl.gates[s].live) {
          continue;
        }
        for (std::uint8_t i = 0; i < nl.gates[s].input_count; ++i) {
          if (!is_shared_flat(nl, tie_flat(s, i))) {
            continue;
          }
          out += "  - {net: ";
          out += pin_text(nl, s, false, i);
          out += "}\n";
        }
      }
    }
  }
  out += "ties:\n";
  // Canonical: one entry per non-leader tied member (a = leader, b = member),
  // ordered by b in slot order so save/load round-trips byte-identically.
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    const Gate &g = nl.gates[s];
    if (!g.live) {
      continue;
    }
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      if (g.drivers[i] != kNoDriver) {
        continue;
      }
      if ((g.ignored_mask & static_cast<std::uint8_t>(1u << i)) != 0) {
        continue;
      }
      const std::size_t flat = tie_flat(s, i);
      if (!is_tied(nl, s, i)) {
        continue;
      }
      const std::size_t leader = tie_leader_flat(nl, flat);
      if (leader == flat) {
        continue;
      }
      const auto lslot = static_cast<std::uint16_t>(leader / kMaxInputs);
      const auto linput = static_cast<std::uint8_t>(leader % kMaxInputs);
      out += "  - {a: ";
      out += pin_text(nl, lslot, false, linput);
      out += ", b: ";
      out += pin_text(nl, s, false, i);
      out += "}\n";
    }
  }
  out += "clocks:\n";
  // Canonical: one entry per derived clock domain (`derive_clock_domains`
  // vocabulary), first-seen slot order, so save/load round-trips
  // byte-identically. Empty for combinational nets.
  for (const ClockDomain &d : derive_clock_domains(nl)) {
    out += "  - {name: ";
    out += d.name;
    out += "}\n";
  }
  out += "meta:\n";
  out += "  tool: ";
  out += kToolName;
  out += "\n";
}

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
