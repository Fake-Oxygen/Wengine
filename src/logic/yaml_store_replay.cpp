// Engine-seam replay for the versioned workspace store.
//
// Each function replays one collected section through the same engine
// seam the interactive verbs use, so file load and live authoring cannot
// drift apart. Replay order is a dependency order, not file order:
// slots first, then shared marks (wires read them), then wires, then
// ties (marks and ties are disjoint), then pulls (after ties and
// wires). The tail validates whole-graph topology and the pinned
// clocks/inits.

#include "yaml_store_detail.hpp"

#include "wengine/logic/adjacency.hpp"
#include "wengine/logic/clock_domains.hpp"
#include "wengine/logic/netlist.hpp"
#include "wengine/logic/seq_state.hpp"
#include "wengine/logic/topo.hpp"
#include "wengine/logic/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace wengine::logic::yaml::detail {

namespace {

// Slot fill shared by replay_gates and the wire-cycle diagnostic below
// (rows already validated by the checkers; the caller ensures capacity
// and, for the live path, duplicate rejection first).
void fill_gate_slots(const Collected &in, Netlist &tmp) {
  for (std::size_t i = 0; i < in.gates.size(); ++i) {
    const GateRow &g = in.gates[i];
    Gate &slot = tmp.slots[i];
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
  tmp.live_count = static_cast<std::uint32_t>(in.gates.size());
  tmp.dirty = true;
  tmp.topo_order.clear();
}

} // namespace

[[nodiscard]] bool replay_gates(const Collected &in, const YAML::Node &sec, Netlist &tmp, std::string &err) {
  const std::vector<GateRow> &gate_rows = in.gates;
  // Dynamic pool: grow to cover every file row before slot-indexed
  // replay. The file-size gate in load_impl already capped the row
  // count at kMaxGates, so growth cannot fail — the refusal below is
  // unreachable, kept loud by construction.
  if (!ensure_slots(tmp, gate_rows.size())) {
    err = at_line(sec, "too many gates (max " + std::to_string(kMaxGates) + ")");
    return false;
  }
  // Duplicate gate ids: dense per-kind counter scan, O(N). Counters are
  // per-kind monotonic and dense in bulk loads, so a vector indexed by
  // counter replaces the old (ordinal, counter) hash set — same refusal,
  // no hashing. Sparse file counters still work (gaps stay unseen).
  std::array<std::uint32_t, 21> max_seen{};
  for (const GateRow &g : gate_rows) {
    const std::size_t o = static_cast<std::size_t>(kind_ordinal(g.kind));
    if (o < max_seen.size() && max_seen[o] < g.counter) {
      max_seen[o] = g.counter;
    }
  }
  std::array<std::vector<char>, 21> seen;
  for (std::size_t o = 0; o < seen.size(); ++o) {
    if (max_seen[o] != 0) {
      seen[o].assign(static_cast<std::size_t>(max_seen[o]) + 1u, 0);
    }
  }
  for (std::size_t i = 0; i < gate_rows.size(); ++i) {
    const GateRow &g = gate_rows[i];
    const std::size_t o = static_cast<std::size_t>(kind_ordinal(g.kind));
    if (o >= seen.size() || static_cast<std::size_t>(g.counter) >= seen[o].size()) {
      err = "line " + std::to_string(line_of(sec[i])) + ": duplicate gate id '" + g.id + "'";
      return false;
    }
    if (seen[o][g.counter] != 0) {
      err = "line " + std::to_string(line_of(sec[i])) + ": duplicate gate id '" + g.id + "'";
      return false;
    }
    seen[o][g.counter] = 1;
  }
  fill_gate_slots(in, tmp);
  return true;
}

// 011: marks land before wire replay (in slot order of first
// appearance — replay order stays file order per the contract, and
// the array state converges deterministically regardless).
// Mark validation mirrors the engine seam exactly: live inputs,
// untied destinations (ties do not exist yet at this point, so the
// loader's tie-time check below guards the other direction).
[[nodiscard]] bool replay_shared(const Collected &in, const YAML::Node & /*sec*/, Netlist &tmp, std::string &err) {
  IndexedBulkResolver idx;
  idx.build(tmp);
  for (const SharedRow &sr : in.shared) {
    const std::string at = "line " + std::to_string(sr.line) + ": ";
    const auto pin = parse_pin_with(tmp, sr.net, idx);
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
  return true;
}

namespace {

// One resolved wire row: pin validation + occupancy + NQ rules, edge
// append excluded. Single shared predicate for the fast bulk loop and
// the slow cycle diagnostic, so their accept sets agree by construction
// (same function, same diagnostics, same order).
struct WirePins {
  PinRef from{};
  PinRef to{};
};

[[nodiscard]] bool validate_wire_row(const Netlist &tmp, const IndexedBulkResolver &idx, const WireRow &w, WirePins &pins,
                                     std::string &err) {
  const std::string at = "line " + std::to_string(w.line) + ": ";
  const auto from = parse_pin_with(tmp, w.from, idx);
  if (!from.ok()) {
    if (from.error == std::string_view("unknown gate")) {
      err = at + "unknown gate in wire source '" + w.from + "'";
    } else {
      err = at + "bad wire source '" + w.from + "'";
    }
    return false;
  }
  const auto to = parse_pin_with(tmp, w.to, idx);
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
  if (from.value.negated && !is_sequential_kind(tmp.slots[from.value.slot].kind)) {
    err = at + "bad wire source '" + w.from + "'";
    return false;
  }
  const Gate &dst = tmp.slots[to.value.slot];
  const bool shared_marked = is_shared_input(tmp, to.value.slot, to.value.input);
  // Range proof for -O3: the parser guarantees input < input_count <=
  // kMaxInputs, but GCC loses that range through the indexed-lookup
  // seam — keep the subscript provably in-bounds (unreachable, loud).
  if (to.value.input >= kMaxInputs) {
    err = at + "bad wire destination '" + w.to + "'";
    return false;
  }
  if (dst.drivers[to.value.input] != kNoDriver && !shared_marked) {
    err = at + "input already wired '" + w.to + "'";
    return false;
  }
  pins.from = from.value;
  pins.to = to.value;
  return true;
}

// Edge append for one validated row (primary driver or 011 shared
// extra). Returns false with the cap diagnostic on shared overflow.
[[nodiscard]] bool append_wire_edge(Netlist &tmp, const WireRow &w, const WirePins &pins, std::string &err) {
  Gate &dst = tmp.slots[pins.to.slot];
  if (dst.drivers[pins.to.input] != kNoDriver) {
    // 011: repeated `to:` on a marked input appends an extra via the
    // SharedNet/engine seam (same cap the engine enforces, loudly,
    // never truncated).
    const std::uint8_t inv = pins.from.negated ? 1 : 0;
    if (!logic::detail::append_shared_extra(tmp, pins.from.slot, pins.to.slot, pins.to.input, inv).ok()) {
      err = "line " + std::to_string(w.line) + ": shared net full (cap 4) '" + w.to + "'";
      return false;
    }
    return true;
  }
  dst.drivers[pins.to.input] = pins.from.slot;
  // Adjacency (016): direct table write; the end-of-load rebuild in
  // validate_topology stays as the bulk-path backstop. Pool-sized edge
  // capacity makes this infallible by construction — false is loud.
  if (!adjacency::adj_insert_edge(tmp, pins.from.slot, pins.to.slot, pins.to.input, 0)) {
    err = "line " + std::to_string(w.line) + ": adjacency edge pool exhausted '" + w.to + "'";
    return false;
  }
  if (pins.from.negated) {
    ++tmp.slots[pins.from.slot].nq_fanout;
    tmp.driver_inv[tie_flat(pins.to.slot, pins.to.input)] = 1;
  } else {
    ++tmp.slots[pins.from.slot].output_fanout;
  }
  return true;
}

// Q-boundary probe shared by the slow diagnostic: an edge into a
// sequential-gate input can never close a combinational cycle (same rule
// as logic::wire), so file order never affects acceptance. The
// whole-graph validator stays authoritative; this names the first closing
// wire on the failure path only.
[[nodiscard]] bool closes_comb_cycle(Netlist &nl, const WirePins &pins) {
  return !is_sequential_kind(nl.slots[pins.to.slot].kind) &&
         topo::reaches(nl, pins.to.slot, pins.from.slot, EdgeFilter::kCombOnly);
}

// Single merged wire loop: validate + append every row, optionally
// probing each edge for the first combinational-cycle closer. The fast
// bulk path runs with probe=false (whole-graph check stays
// authoritative); the slow diagnostic re-runs the same rows on a scratch
// net with probe=true. One loop, one predicate — no fast/slow
// duplication.
[[nodiscard]] bool replay_wire_rows(Netlist &nl, const IndexedBulkResolver &idx, const Collected &in, bool probe_cycle,
                                   std::string &err, bool generic_row_errors) {
  for (const WireRow &w : in.wires) {
    WirePins pins{};
    if (!validate_wire_row(nl, idx, w, pins, err)) {
      if (generic_row_errors) {
        err = "workspace topology error"; // unreachable: rows resolved above
      }
      return false;
    }
    if (!append_wire_edge(nl, w, pins, err)) {
      if (generic_row_errors) {
        err = "workspace topology error"; // unreachable: caps passed above
      }
      return false;
    }
    if (probe_cycle && closes_comb_cycle(nl, pins)) {
      err = "line " + std::to_string(w.line) + ": wire closes a combinational cycle '" + w.from + "' -> '" + w.to + "'";
      return false;
    }
  }
  return true;
}

// Slow cycle diagnostic (failure path only): replays the rows onto a
// scratch net with the per-wire cycle probe to name the first closing
// wire. The fast bulk path skips that probe — its O(N) seen-zeroing per
// wire dominates large loads — while the single whole-graph check stays
// authoritative.
[[nodiscard]] bool locate_cycle_wire(const Collected &in, std::string &err) {
  Netlist scratch{};
  if (!ensure_slots(scratch, in.gates.size())) {
    err = "too many gates (max " + std::to_string(kMaxGates) + ")";
    return false;
  }
  fill_gate_slots(in, scratch);
  std::string dummy;
  if (!replay_shared(in, YAML::Node{}, scratch, dummy)) {
    err = std::move(dummy); // unreachable: shared replay passed above
    return false;
  }
  IndexedBulkResolver sidx;
  sidx.build(scratch);
  if (!replay_wire_rows(scratch, sidx, in, true, dummy, true)) {
    err = std::move(dummy);
    if (err.rfind("line ", 0) != 0) {
      err = "workspace topology error";
    }
    return false;
  }
  err = "workspace topology error";
  return false;
}

} // namespace

[[nodiscard]] bool replay_wires(const Collected &in, const YAML::Node & /*sec*/, Netlist &tmp, std::string &err) {
  // Fast bulk loop: pin/occupancy/NQ validation + edge append, no
  // per-wire cycle probe. The probe's whole-pool seen-zeroing per wire
  // is O(N*W) — the second half of the 100k-gate load wall. Cycles are
  // caught once below by the whole-graph check (same authority as
  // validate_topology); only that failure pays for the slow
  // line-numbered diagnostic above.
  IndexedBulkResolver idx;
  idx.build(tmp);
  if (!replay_wire_rows(tmp, idx, in, false, err, false)) {
    return false;
  }
  adjacency::adjacency_rebuild(tmp);
  const bool acyclic = has_sequential_gates(tmp) ? topo::query(tmp, EdgeFilter::kCombOnly).ok()
                                                 : topo::compile(tmp, EdgeFilter::kAll).ok();
  if (acyclic) {
    return true;
  }
  return locate_cycle_wire(in, err);
}

[[nodiscard]] bool replay_ties(const Collected &in, const YAML::Node & /*sec*/, Netlist &tmp, std::string &err) {
  IndexedBulkResolver idx;
  idx.build(tmp);
  for (const TieRow &t : in.ties) {
    const std::string at = "line " + std::to_string(t.line) + ": ";
    const auto a = parse_pin_with(tmp, t.a, idx);
    if (!a.ok()) {
      if (a.error == std::string_view("unknown gate")) {
        err = at + "unknown gate in tie pin '" + t.a + "'";
      } else {
        err = at + "bad tie pin '" + t.a + "'";
      }
      return false;
    }
    const auto b = parse_pin_with(tmp, t.b, idx);
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
    const Gate &ga = tmp.slots[a.value.slot];
    const Gate &gb = tmp.slots[b.value.slot];
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
  return true;
}

// 012: pulls land after ties (so a pull on a tied input fails with
// the pull diagnostic, mirroring the interactive verbs) and after
// wires (pulled destinations wire normally). Replay order is file
// order; row validation replays the engine seams exactly.
[[nodiscard]] bool replay_pulls(const Collected &in, const YAML::Node & /*sec*/, Netlist &tmp, std::string &err) {
  IndexedBulkResolver idx;
  idx.build(tmp);
  for (const PullRow &pr : in.pulls) {
    const std::string at = "line " + std::to_string(pr.line) + ": ";
    const auto pin = parse_pin_with(tmp, pr.net, idx);
    if (!pin.ok()) {
      err = at + "bad pull net '" + pr.net + "'";
      return false;
    }
    if (pin.value.is_output) {
      err = at + "bad pull net '" + pr.net + "' (outputs carry no pull)";
      return false;
    }
    if (is_pulled_flat(tmp, tie_flat(pin.value.slot, pin.value.input))) {
      err = at + "duplicate pull net '" + pr.net + "'";
      return false;
    }
    if (is_tied(tmp, pin.value.slot, pin.value.input)) {
      err = at + "bad pull net '" + pr.net + "' (tied, untie it first)";
      return false;
    }
    if ((tmp.slots[pin.value.slot].ignored_mask & static_cast<std::uint8_t>(1u << pin.value.input)) != 0) {
      err = at + "bad pull net '" + pr.net + "' (ignored, use it first)";
      return false;
    }
    if (!set_pull(tmp, pin.value.slot, pin.value.input, pr.level).ok()) {
      err = at + "bad pull net '" + pr.net + "'";
      return false;
    }
  }
  tmp.dirty = true;
  tmp.topo_order.clear();
  return true;
}

[[nodiscard]] bool validate_topology(Netlist &tmp, std::string &err) {
  // Bulk replay wrote tables directly: one PoolStore seam rebuilds every
  // derived store (chains, tie index, heap, feeders) before the
  // whole-graph validation below (which reads the chains).
  pool_store::rebuild_all(tmp);
  // Flop-aware cycle check (mirrors the wire verb): combinational loops
  // always fail; feedback through sequential gates validates sequentially.
  if (has_sequential_gates(tmp)) {
    if (!topo::query(tmp, EdgeFilter::kCombOnly).ok()) {
      err = "workspace topology error";
      return false;
    }
  } else if (!topo::compile(tmp, EdgeFilter::kAll).ok()) {
    err = "workspace topology error";
    return false;
  }
  return true;
}

[[nodiscard]] bool apply_inits_and_validate_clocks(const Collected &in, const Netlist &tmp, std::vector<LogicValue> &inits, std::string &err) {
  // Per-slot power-on values: explicit `init:` keys land on their gate's
  // slot; every other slot reads Low. `check_gate_map` already rejected
  // `init` on combinational rows. The caller sizes `inits` to the pool;
  // an init index past the end fails loudly instead of indexing out
  // of bounds (unreachable after the file-size gate, kept loud by
  // construction).
  inits.assign(inits.size(), LogicValue::kLow);
  for (std::size_t i = 0; i < in.gates.size(); ++i) {
    if (in.gates[i].has_init) {
      if (i >= inits.size()) {
        err = "too many gates (max " + std::to_string(kMaxGates) + ")";
        return false;
      }
      inits[i] = in.gates[i].init;
    }
  }
  // Pinned clock domains: the stored names must equal the derived set
  // exactly (order-insensitive) — membership itself stays derived from
  // the topology above, so any drift fails loudly instead of silently
  // re-deriving.
  const std::vector<ClockDomain> derived = derive_clock_domains(tmp);
  for (const ClockRow &c : in.clocks) {
    std::size_t hits = 0;
    for (const ClockRow &d : in.clocks) {
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
      err = "line " + std::to_string(c.line) + ": unknown clock domain '" + c.name + "' (not derived from this workspace)";
      return false;
    }
  }
  if (in.clocks.size() != derived.size()) {
    for (const ClockDomain &d : derived) {
      bool found = false;
      for (const ClockRow &c : in.clocks) {
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
  return true;
}

} // namespace wengine::logic::yaml::detail
