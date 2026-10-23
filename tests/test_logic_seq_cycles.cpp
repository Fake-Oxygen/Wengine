#include "wengine/logic/netlist.hpp"
#include "wengine/logic/seq_state.hpp"
#include "wengine/logic/types.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>

using namespace wengine;
using wengine::logic::GateKind;
using wengine::logic::Netlist;

namespace {

// add_gate still refuses sequential kinds (SEQ1 staging), so tests place
// them by hand — same white-box pokes as test_logic_seq_state.
void place_seq(Netlist &nl, std::uint16_t slot, GateKind kind, std::uint32_t counter) {
  assert(slot < logic::kMaxGates);
  assert(logic::is_sequential_kind(kind));
  logic::Gate &g = nl.gates[slot];
  assert(!g.live);
  g.live = true;
  g.kind = kind;
  g.counter = counter;
  g.input_count = 2;
  g.ignored_mask = 0;
  g.ignore_output = false;
  g.output_fanout = 0;
  g.propagation_delay_ps = 0;
  for (std::size_t i = 0; i < logic::kMaxInputs; ++i) {
    g.drivers[i] = logic::kNoDriver;
  }
  ++nl.live_count;
  nl.dirty = true;
  nl.topo_size = 0;
}

void test_validate_empty_and_comb_acyclic() {
  Netlist nl;
  assert(logic::validate_sequential(nl).ok());
  assert(!logic::has_sequential_gates(nl));
  const std::uint16_t a = logic::add_gate(nl, GateKind::kAnd, 2).value;
  const std::uint16_t n = logic::add_gate(nl, GateKind::kNot, 1).value;
  assert(logic::wire(nl, a, n, 0).ok());
  assert(logic::validate_sequential(nl).ok());
  assert(!logic::has_sequential_gates(nl));
}

void test_validate_rejects_comb_cycles() {
  // Hand-poked cycle (wire() rightly refuses to build it): AND <-> NOT.
  Netlist nl;
  const std::uint16_t a = logic::add_gate(nl, GateKind::kAnd, 2).value;
  const std::uint16_t n = logic::add_gate(nl, GateKind::kNot, 1).value;
  nl.gates[a].drivers[0] = n;
  nl.gates[n].drivers[0] = a;
  nl.dirty = true;
  assert(!logic::validate_sequential(nl).ok());

  // Self-loop likewise fails.
  Netlist self;
  const std::uint16_t s = logic::add_gate(self, GateKind::kAnd, 2).value;
  self.gates[s].drivers[0] = s;
  self.dirty = true;
  assert(!logic::validate_sequential(self).ok());
}

void test_flop_feedback_loop() {
  // DFF Q -> AND -> DFF D: the canonical legal sequential cycle, built
  // entirely through the public wire() API.
  Netlist nl;
  place_seq(nl, 0, GateKind::kDff, 1); // DFF1
  const std::uint16_t a = logic::add_gate(nl, GateKind::kAnd, 2).value;
  assert(logic::wire(nl, a, 0, 0).ok()); // AND.Y -> DFF.D
  assert(logic::wire(nl, 0, a, 0).ok()); // DFF.Y -> AND.0 (closes the loop)
  assert(logic::has_sequential_gates(nl));
  assert(logic::validate_sequential(nl).ok());
  // The old predicate still flags it — the Q-boundary is what allows it.
  assert(logic::detail::reaches(nl, a, 0));
  assert(!logic::detail::reaches_seq(nl, a, 0));
}

void test_clk_and_dlatch_variants() {
  // Clock input driven by comb + Q feedback: same boundary, other pin.
  Netlist clk;
  place_seq(clk, 0, GateKind::kDff, 1);
  const std::uint16_t c = logic::add_gate(clk, GateKind::kBuf, 1).value;
  assert(logic::wire(clk, c, 0, 1).ok()); // BUF.Y -> DFF.CLK
  assert(logic::wire(clk, 0, c, 0).ok()); // DFF.Y -> BUF.0
  assert(logic::validate_sequential(clk).ok());

  // DLATCH enable + D feedback through one comb gate.
  Netlist latch;
  place_seq(latch, 2, GateKind::kDlatch, 1);
  const std::uint16_t o = logic::add_gate(latch, GateKind::kOr, 2).value;
  assert(logic::wire(latch, o, 2, 0).ok()); // OR.Y -> DLATCH.D
  assert(logic::wire(latch, 2, o, 0).ok()); // DLATCH.Y -> OR.0
  assert(logic::validate_sequential(latch).ok());
}

void test_comb_cycle_still_rejected_with_flop_present() {
  // A live-but-uninvolved flop must not shield a real combinational cycle.
  Netlist nl;
  place_seq(nl, 0, GateKind::kDff, 1);
  const std::uint16_t a = logic::add_gate(nl, GateKind::kAnd, 2).value;
  const std::uint16_t n = logic::add_gate(nl, GateKind::kNot, 1).value;
  assert(logic::wire(nl, a, n, 0).ok());
  assert(!logic::wire(nl, n, a, 0).ok()); // NOT.Y -> AND.0: comb cycle, loud
  // Hand-built comb cycle beside a flop fails whole-graph validation too.
  nl.gates[a].drivers[1] = n;
  nl.dirty = true;
  assert(!logic::validate_sequential(nl).ok());
}

void test_wire_unchanged_for_comb() {
  // Pure-combinational discipline is byte-identical behavior: self-loops
  // and two-gate loops rejected, DAG wires accepted (old reaches path).
  Netlist nl;
  const std::uint16_t a = logic::add_gate(nl, GateKind::kAnd, 2).value;
  const std::uint16_t n = logic::add_gate(nl, GateKind::kNot, 1).value;
  assert(!logic::wire(nl, a, a, 0).ok()); // self-loop
  assert(logic::wire(nl, a, n, 0).ok());
  assert(!logic::wire(nl, n, a, 0).ok()); // would close AND->NOT->AND
  assert(!logic::has_sequential_gates(nl));
  assert(logic::validate_sequential(nl).ok());
}

void test_has_sequential_gates_tracks_lifetime() {
  Netlist nl;
  assert(!logic::has_sequential_gates(nl));
  place_seq(nl, 3, GateKind::kDff, 1);
  assert(logic::has_sequential_gates(nl));
  assert(logic::remove_gate(nl, 3).ok());
  assert(!logic::has_sequential_gates(nl));
}

} // namespace

int main() {
  test_validate_empty_and_comb_acyclic();
  test_validate_rejects_comb_cycles();
  test_flop_feedback_loop();
  test_clk_and_dlatch_variants();
  test_comb_cycle_still_rejected_with_flop_present();
  test_wire_unchanged_for_comb();
  test_has_sequential_gates_tracks_lifetime();
  std::cout << "test_logic_seq_cycles: OK\n";
  return 0;
}
