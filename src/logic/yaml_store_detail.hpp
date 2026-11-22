// Internal seam for the versioned logic-workspace store (LOGIC-6).
//
// This header is private to the wengine_logic_yaml TUs under src/logic/
// (never installed, never included by engine/CLI/test code). It holds the
// pieces the store TUs share: parsed row types, the SectionSpec table
// vocabulary driving the generic collect/replay loop, and the engine-seam
// replay/save entry points. The public surface stays yaml-free in
// include/wengine/logic/yaml_store.hpp; yaml-cpp is known only inside
// this library (same seam as src/signal/lua_solver.cpp).

#pragma once

#include "wengine/logic/netlist.hpp"
#include "wengine/logic/pins.hpp"
#include "wengine/logic/types.hpp"
#include "wengine/logic/yaml_store.hpp"

#include <yaml-cpp/yaml.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wengine::logic::yaml::detail {

// --- Small shared utilities (used by check + orchestration + replay) ---

// Strict unsigned decimal: no signs/spaces, digits only, saturates on
// overflow. Mirrors the former hand parser (quoted "2" and plain 2 both
// surface as Scalar "2", so one path covers both spellings).
[[nodiscard]] inline bool parse_u64(std::string_view text, std::uint64_t &out) {
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

[[nodiscard]] inline std::size_t line_of(const YAML::Node &n) {
  const int line = n.Mark().line;
  if (line < 0) {
    return 1;
  }
  return static_cast<std::size_t>(line) + 1u;
}

[[nodiscard]] inline std::string at_line(const YAML::Node &n, const std::string &msg) {
  return "line " + std::to_string(line_of(n)) + ": " + msg;
}

// --- Parsed row types (one per sequence section) ---

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

// 012: one `{net: PIN, pull: up|down}` row per pulled input. The level
// spellings are lowercase, matching the lowercase kind convention;
// `none` is absence, never written or accepted.
struct PullRow {
  std::string net;
  std::uint8_t level = 0; // 1 = up, 2 = down
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

// All collected rows, in canonical section order.
struct Collected {
  std::vector<GateRow> gates;
  std::vector<WireRow> wires;
  std::vector<SharedRow> shared;
  std::vector<PullRow> pulls;
  std::vector<TieRow> ties;
  std::vector<ClockRow> clocks;
};

// --- Bulk-load gate index (LOGIC-V2-SCALE4 follow-up, no format change) ---
//
// Pin text (`AND12.1`) resolves through resolve_gate's linear slot scan:
// O(N) per lookup, so replaying W wires costs O(N*W) — the measured
// 100k-gate load wall (replay ~16s, superlinear). The index maps
// (kind-ordinal, counter) -> slot once per load (O(N)) and every pin
// resolves O(1) after. Interactive verbs keep resolve_gate (single-pin
// cost is trivial); only the bulk replay path below uses this.
//
// Keys use the same vocabulary as check_gate_map (kind-matched prefix +
// 1-based counter, case-insensitive): duplicate (ordinal, counter)
// pairs are refused by replay_gates before any index is built, so the
// index never observes them.

struct GateIndex {
  std::unordered_map<std::uint64_t, Slot> slot_by_key;
};

[[nodiscard]] inline std::uint64_t gate_index_key(std::uint8_t ordinal, std::uint32_t counter) noexcept {
  return (static_cast<std::uint64_t>(ordinal) << 32) | counter;
}

inline void build_gate_index(const Netlist &nl, GateIndex &idx) {
  idx.slot_by_key.clear();
  std::size_t live = 0;
  for (Slot s = 0; s < slot_count(nl); ++s) {
    if (nl.slots[s].live) {
      ++live;
    }
  }
  idx.slot_by_key.reserve(live * 2 + 1);
  for (Slot s = 0; s < slot_count(nl); ++s) {
    const Gate &g = nl.slots[s];
    if (!g.live) {
      continue;
    }
    idx.slot_by_key.emplace(gate_index_key(kind_ordinal(g.kind), g.counter), s);
  }
}

// Indexed gate resolution: the exact resolve_gate (pins.hpp) spelling
// and error vocabulary ("bad gate name: empty", "bad gate name",
// "unknown gate") over the bulk index instead of the slot scan, so
// parse_pin_with below reports byte-identical diagnostics.
[[nodiscard]] inline core::Result<Slot> resolve_gate_indexed(const GateIndex &idx, std::string_view name) noexcept {
  if (name.empty()) {
    return core::Result<Slot>::failure("bad gate name: empty");
  }
  for (const KindInfo &e : kKinds) {
    const std::uint8_t o = kind_ordinal(e.kind);
    const std::string_view kind_name = e.name;
    if (!logic::detail::starts_with_ci(name, kind_name)) {
      continue;
    }
    const std::string_view num = name.substr(kind_name.size());
    std::uint32_t counter = 0;
    if (!logic::detail::parse_decimal(num, 0xFFFFFFFFu, counter) || counter == 0) {
      continue; // not this kind (or bad number): keep trying other kinds
    }
    const auto it = idx.slot_by_key.find(gate_index_key(o, counter));
    if (it != idx.slot_by_key.end()) {
      return core::Result<Slot>::success(it->second);
    }
    return core::Result<Slot>::failure("unknown gate");
  }
  return core::Result<Slot>::failure("bad gate name");
}

// Indexed pin parse: the single table-owned suffix parser (pins.hpp)
// over the bulk index — byte-identical diagnostics, O(1) gate lookup.
[[nodiscard]] inline core::Result<PinRef> parse_pin_indexed(const Netlist &nl, const GateIndex &idx, std::string_view text) {
  return parse_pin_with(nl, text, [&](std::string_view n) noexcept { return resolve_gate_indexed(idx, n); });
}

// --- Per-row checkers (yaml_store_check.cpp) ---

[[nodiscard]] bool check_gate_map(const YAML::Node &g, GateRow &row, std::string &err);
[[nodiscard]] bool check_wire_map(const YAML::Node &w, WireRow &row, std::string &err);
[[nodiscard]] bool check_tie_map(const YAML::Node &t, TieRow &row, std::string &err);
[[nodiscard]] bool check_clocks_map(const YAML::Node &c, ClockRow &row, std::string &err);
[[nodiscard]] bool check_shared_map(const YAML::Node &n, SharedRow &row, std::string &err);
[[nodiscard]] bool check_pull_map(const YAML::Node &n, PullRow &row, std::string &err);

// --- SectionSpec table (yaml_store_check.cpp) ---
//
// One row per sequence section. The generic loop in load_impl derives
// everything per-section from this table: required-section presence,
// sequence-shape guards, and the row-collection loop (via collect). The
// root-key allowlist and its "(want ...)" diagnostic derive from the
// same table, so section 13 is one row + one checker + one replay
// function instead of four cloned blocks. Replay order is engine-seam
// order (slots, marks, wires, ties, pulls), not file order — see
// kReplayOrder in yaml_store.cpp.

struct SectionSpec {
  const char *name;       // e.g. "gates"
  bool optional;          // shared/pulls only: absent means empty
  const char *missing_err; // presence refusal when required (nullptr if optional)
  const char *seq_err;    // e.g. "'gates:' must read a sequence"
  const char *row_err;    // non-map row shape diagnostic
  // Shape-guard + row loop for one section; row_err comes from the table.
  bool (*collect)(const YAML::Node &sec, Collected &out, const char *row_err, std::string &err);
  // Whole-section engine-seam replay into tmp (clocks: nullptr — the
  // tail validates clocks against derived topology instead). The index
  // covers tmp's slots (built once after replay_gates; replay_gates
  // itself ignores it) so pin-heavy sections skip the O(N) slot scan.
  bool (*replay)(const Collected &in, const YAML::Node &sec, Netlist &tmp, const GateIndex &idx, std::string &err);
};

extern const SectionSpec kSections[6];

[[nodiscard]] const SectionSpec *section_by_name(std::string_view name) noexcept;

// "(want version|gates|wires|shared|pulls|ties|clocks|meta)" derived from
// the table (version first, meta last — the canonical save order).
[[nodiscard]] std::string root_want_list();

// --- Engine-seam replay (yaml_store_replay.cpp) ---

[[nodiscard]] bool replay_gates(const Collected &in, const YAML::Node &sec, Netlist &tmp, const GateIndex &idx, std::string &err);
[[nodiscard]] bool replay_shared(const Collected &in, const YAML::Node &sec, Netlist &tmp, const GateIndex &idx, std::string &err);
[[nodiscard]] bool replay_wires(const Collected &in, const YAML::Node &sec, Netlist &tmp, const GateIndex &idx, std::string &err);
[[nodiscard]] bool replay_ties(const Collected &in, const YAML::Node &sec, Netlist &tmp, const GateIndex &idx, std::string &err);
[[nodiscard]] bool replay_pulls(const Collected &in, const YAML::Node &sec, Netlist &tmp, const GateIndex &idx, std::string &err);
// Flop-aware whole-graph cycle check (mirrors the wire verb).
[[nodiscard]] bool validate_topology(Netlist &tmp, std::string &err);
// Per-slot power-on values + pinned-clock set-equality against the
// derived domains.
[[nodiscard]] bool apply_inits_and_validate_clocks(const Collected &in, const Netlist &tmp, std::vector<LogicValue> &inits, std::string &err);

} // namespace wengine::logic::yaml::detail
