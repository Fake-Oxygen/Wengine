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

[[nodiscard]] bool replay_gates(const Collected &in, const YAML::Node &sec, Netlist &tmp, std::string &err) {
  const std::vector<GateRow> &gate_rows = in.gates;
  for (std::size_t i = 0; i < gate_rows.size(); ++i) {
    const GateRow &g = gate_rows[i];
    for (std::size_t j = 0; j < i; ++j) {
      if (gate_rows[j].kind == g.kind && gate_rows[j].counter == g.counter) {
        err = "line " + std::to_string(line_of(sec[i])) + ": duplicate gate id '" + g.id + "'";
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
  return true;
}

// 011: marks land before wire replay (in slot order of first
// appearance — replay order stays file order per the contract, and
// the array state converges deterministically regardless).
// Mark validation mirrors the engine seam exactly: live inputs,
// untied destinations (ties do not exist yet at this point, so the
// loader's tie-time check below guards the other direction).
[[nodiscard]] bool replay_shared(const Collected &in, const YAML::Node & /*sec*/, Netlist &tmp, std::string &err) {
  for (const SharedRow &sr : in.shared) {
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
  return true;
}

[[nodiscard]] bool replay_wires(const Collected &in, const YAML::Node & /*sec*/, Netlist &tmp, std::string &err) {
  for (const WireRow &w : in.wires) {
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
      // 011: repeated `to:` on a marked input appends an extra via the
      // SharedNet/engine seam (same cap the engine enforces, loudly,
      // never truncated).
      const std::uint8_t inv = from.value.negated ? 1 : 0;
      if (!logic::detail::append_shared_extra(tmp, from.value.slot, to.value.slot, to.value.input, inv).ok()) {
        err = at + "shared net full (cap 4) '" + w.to + "'";
        return false;
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
  return true;
}

[[nodiscard]] bool replay_ties(const Collected &in, const YAML::Node & /*sec*/, Netlist &tmp, std::string &err) {
  for (const TieRow &t : in.ties) {
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
  return true;
}

// 012: pulls land after ties (so a pull on a tied input fails with
// the pull diagnostic, mirroring the interactive verbs) and after
// wires (pulled destinations wire normally). Replay order is file
// order; row validation replays the engine seams exactly.
[[nodiscard]] bool replay_pulls(const Collected &in, const YAML::Node & /*sec*/, Netlist &tmp, std::string &err) {
  for (const PullRow &pr : in.pulls) {
    const std::string at = "line " + std::to_string(pr.line) + ": ";
    const auto pin = parse_pin(tmp, pr.net);
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
    if ((tmp.gates[pin.value.slot].ignored_mask & static_cast<std::uint8_t>(1u << pin.value.input)) != 0) {
      err = at + "bad pull net '" + pr.net + "' (ignored, use it first)";
      return false;
    }
    if (!set_pull(tmp, pin.value.slot, pin.value.input, pr.level).ok()) {
      err = at + "bad pull net '" + pr.net + "'";
      return false;
    }
  }
  tmp.dirty = true;
  tmp.topo_size = 0;
  return true;
}

[[nodiscard]] bool validate_topology(Netlist &tmp, std::string &err) {
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
  return true;
}

[[nodiscard]] bool apply_inits_and_validate_clocks(const Collected &in, const Netlist &tmp, std::array<LogicValue, kMaxGates> &inits, std::string &err) {
  // Per-slot power-on values: explicit `init:` keys land on their gate's
  // slot; every other slot reads Low. `check_gate_map` already rejected
  // `init` on combinational rows.
  inits.fill(LogicValue::kLow);
  for (std::size_t i = 0; i < in.gates.size(); ++i) {
    if (in.gates[i].has_init) {
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
