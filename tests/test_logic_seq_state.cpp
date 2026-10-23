#include "wengine/logic/eval.hpp"
#include "wengine/logic/netlist.hpp"
#include "wengine/logic/seq_state.hpp"
#include "wengine/logic/types.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <span>

using namespace wengine;
using wengine::logic::GateKind;
using wengine::logic::LogicValue;
using wengine::logic::Netlist;
using wengine::logic::SeqState;

namespace {

// add_gate still refuses sequential kinds (SEQ1 staging), so tests place
// them by hand — white-box struct pokes, same lifetime fields add_gate sets.
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

void kill_gate(Netlist &nl, std::uint16_t slot) {
  assert(logic::remove_gate(nl, slot).ok());
}

void test_fresh_and_empty_init() {
  SeqState st;
  assert(st.live_count == 0);
  assert(st.next_serial == 0);
  for (std::uint16_t s = 0; s < logic::kMaxGates; ++s) {
    assert(st.valid[s] == 0);
    assert(st.q[s] == LogicValue::kLow);
    assert(st.serial[s] == 0);
    assert(!logic::seq_has(st, s));
  }
  assert(!logic::find_seq_serial(st, 0).has_value());
  Netlist nl;
  logic::init_seq_state(st, nl);
  assert(st.live_count == 0 && st.next_serial == 0);
}

void test_attach_and_combinational_ignored() {
  Netlist nl;
  place_seq(nl, 0, GateKind::kDff, 1);   // DFF1
  place_seq(nl, 5, GateKind::kDlatch, 1); // DLATCH1
  assert(logic::add_gate(nl, GateKind::kAnd, 2).ok()); // combinational: no state
  SeqState st;
  logic::init_seq_state(st, nl);
  assert(st.live_count == 2);
  assert(st.next_serial == 2);
  assert(logic::seq_has(st, 0));
  assert(logic::seq_has(st, 5));
  assert(!logic::seq_has(st, 1));
  assert(st.serial[0] == 0 && st.serial[5] == 1);
  assert(st.q[0] == LogicValue::kLow && st.q[5] == LogicValue::kLow);
  const std::optional<std::uint16_t> s0 = logic::find_seq_serial(st, 0);
  assert(s0.has_value() && s0.value() == 0);
  const std::optional<std::uint16_t> s1 = logic::find_seq_serial(st, 1);
  assert(s1.has_value() && s1.value() == 5);
  assert(!logic::find_seq_serial(st, 2).has_value());
  assert(!logic::seq_has(st, logic::kMaxGates)); // out of range, never UB
}

void test_reinit_retains_q_and_serial() {
  Netlist nl;
  place_seq(nl, 0, GateKind::kDff, 1);
  SeqState st;
  logic::init_seq_state(st, nl);
  st.q[0] = LogicValue::kHigh; // a step latched Q
  logic::init_seq_state(st, nl); // stepper re-syncs every step
  assert(st.live_count == 1 && st.next_serial == 1);
  assert(st.serial[0] == 0); // same flop, same serial
  assert(st.q[0] == LogicValue::kHigh); // state persisted, not re-powered
  logic::init_seq_state(st, nl); // idempotent
  assert(st.live_count == 1 && st.serial[0] == 0 && st.q[0] == LogicValue::kHigh);
}

void test_detach_retires_serial() {
  Netlist nl;
  place_seq(nl, 0, GateKind::kDff, 1);
  SeqState st;
  logic::init_seq_state(st, nl);
  assert(logic::find_seq_serial(st, 0).has_value());
  kill_gate(nl, 0);
  logic::init_seq_state(st, nl);
  assert(st.live_count == 0);
  assert(!logic::seq_has(st, 0));
  assert(!logic::find_seq_serial(st, 0).has_value()); // retired, never stale
  assert(st.next_serial == 1); // monotonic: never rewound by detach
}

void test_recycle_gets_fresh_serial_and_power_on() {
  Netlist nl;
  place_seq(nl, 0, GateKind::kDff, 1);
  SeqState st;
  logic::init_seq_state(st, nl);
  st.q[0] = LogicValue::kHigh;
  kill_gate(nl, 0);
  logic::init_seq_state(st, nl);
  place_seq(nl, 0, GateKind::kDff, 2); // slot recycled, new flop
  logic::init_seq_state(st, nl);
  assert(st.live_count == 1);
  assert(logic::seq_has(st, 0));
  assert(st.serial[0] == 1); // fresh serial, old one never reused
  assert(st.q[0] == LogicValue::kLow); // power-on, stale High cleared
  const std::optional<std::uint16_t> s = logic::find_seq_serial(st, 1);
  assert(s.has_value() && s.value() == 0);
}

void test_reset_starts_new_lifetime() {
  Netlist nl;
  place_seq(nl, 0, GateKind::kDff, 1);
  SeqState st;
  logic::init_seq_state(st, nl);
  logic::reset_seq_state(st);
  assert(st.live_count == 0 && st.next_serial == 0);
  assert(!logic::seq_has(st, 0));
  logic::init_seq_state(st, nl); // same netlist, new lifetime
  assert(st.live_count == 1 && st.serial[0] == 0);
  assert(st.q[0] == LogicValue::kLow);
}

void test_combinational_solve_unchanged() {
  // The pool's presence changes nothing for combinational eval: NAND via
  // AND+NOT still solves bit-exactly (SEQ2 adds no eval path).
  Netlist nl;
  const std::uint16_t a = logic::add_gate(nl, GateKind::kAnd, 2).value;
  const std::uint16_t n = logic::add_gate(nl, GateKind::kNot, 1).value;
  assert(logic::wire(nl, a, n, 0).ok());
  SeqState st;
  logic::init_seq_state(st, nl);
  assert(st.live_count == 0);
  const LogicValue kL = LogicValue::kLow;
  const LogicValue kH = LogicValue::kHigh;
  const LogicValue in11[2] = {kH, kH};
  LogicValue out[1] = {kL};
  assert(logic::solve(nl, std::span<const LogicValue>(in11, 2), std::span<LogicValue>(out, 1)).ok());
  assert(out[0] == kL); // NAND(1,1) = 0
  const LogicValue in10[2] = {kH, kL};
  assert(logic::solve(nl, std::span<const LogicValue>(in10, 2), std::span<LogicValue>(out, 1)).ok());
  assert(out[0] == kH); // NAND(1,0) = 1
}

} // namespace

int main() {
  test_fresh_and_empty_init();
  test_attach_and_combinational_ignored();
  test_reinit_retains_q_and_serial();
  test_detach_retires_serial();
  test_recycle_gets_fresh_serial_and_power_on();
  test_reset_starts_new_lifetime();
  test_combinational_solve_unchanged();
  std::cout << "test_logic_seq_state: OK\n";
  return 0;
}
