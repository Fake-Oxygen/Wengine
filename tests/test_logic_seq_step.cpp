#include "wengine/logic/eval.hpp"
#include "wengine/logic/netlist.hpp"
#include "wengine/logic/seq_state.hpp"
#include "wengine/logic/seq_step.hpp"
#include "wengine/logic/types.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>

using namespace wengine;
using wengine::logic::GateKind;
using wengine::logic::LogicValue;
using wengine::logic::Netlist;
using wengine::logic::SeqState;

namespace {

// add_gate still refuses sequential kinds (SEQ1 staging), so tests place
// them by hand — same white-box pokes as test_logic_seq_state/cycles.
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

void ignore_input(Netlist &nl, std::uint16_t slot, std::uint8_t input) {
  logic::PinRef pin{};
  pin.slot = slot;
  pin.is_output = false;
  pin.input = input;
  assert(logic::ignore_pin(nl, pin).ok());
}

void test_counter_2bit() {
  // 2-bit binary counter (Q1=MSB, Q0=LSB), zero primary inputs:
  //   D0 = NOT Q0            (LSB toggles every edge)
  //   D1 = Q1 XOR Q0         (MSB toggles on carry)
  // CLKs ignored (edge is the call itself); no block I/O.
  Netlist nl;
  place_seq(nl, 0, GateKind::kDff, 1); // DFF1 = Q0 (LSB)
  place_seq(nl, 1, GateKind::kDff, 2); // DFF2 = Q1 (MSB)
  const std::uint16_t n = logic::add_gate(nl, GateKind::kNot, 1).value;
  const std::uint16_t x = logic::add_gate(nl, GateKind::kXor, 2).value;
  // Feedback build order matters (SEQ3 Q-boundary): comb->flop edges are
  // invisible to reaches_seq, so wire each flop D input FIRST, then the Q
  // fanout — the reverse order trips the incremental cycle check.
  assert(logic::wire(nl, n, 0, 0).ok()); // NOT.Y -> DFF.Q0.D
  assert(logic::wire(nl, 0, n, 0).ok()); // Q0 -> NOT.1
  assert(logic::wire(nl, x, 1, 0).ok()); // XOR.Y -> DFF.Q1.D
  assert(logic::wire(nl, 0, x, 0).ok()); // Q0 -> XOR.1
  assert(logic::wire(nl, 1, x, 1).ok()); // Q1 -> XOR.2
  ignore_input(nl, 0, 1); // DFF1.CLK ignored (edge assumed)
  ignore_input(nl, 1, 1); // DFF2.CLK ignored
  assert(logic::validate_sequential(nl).ok());
  assert(logic::block_input_count(nl) == 0);
  // No Q outputs dangle (both feed logic) and NQ is ignored by default:
  // the counter exposes no block outputs until `use`d or wired.
  assert(logic::block_output_count(nl) == 0);

  SeqState st;
  const LogicValue kL = LogicValue::kLow;
  const LogicValue kH = LogicValue::kHigh;
  // Expected (Q1,Q0): 00 -> 01 -> 10 -> 11 -> 00, two full cycles bit-exact.
  const LogicValue want[9][2] = {
      {kL, kL}, // power-on 00
      {kL, kH}, // 01
      {kH, kL}, // 10
      {kH, kH}, // 11
      {kL, kL}, // 00
      {kL, kH}, // 01
      {kH, kL}, // 10
      {kH, kH}, // 11
      {kL, kL}, // 00
  };
  assert(st.q[0] == kL && st.q[1] == kL);
  for (int step = 1; step <= 8; ++step) {
    assert(logic::seq_step(nl, st, std::span<const LogicValue>{}).ok());
    assert(st.q[1] == want[step][0]);
    assert(st.q[0] == want[step][1]);
  }
}

void test_shift_register_parallel_update() {
  // 2-stage shift register: SER -> DFF0 -> DFF1 -> OUT. One block input
  // (SER), one block output (Q1). Proves end-of-step parallel update: Q1
  // takes OLD Q0, never the just-latched Q0.
  Netlist nl;
  place_seq(nl, 0, GateKind::kDff, 1); // stage 0
  place_seq(nl, 1, GateKind::kDff, 2); // stage 1
  assert(logic::wire(nl, 0, 1, 0).ok()); // Q0 -> DFF1.D
  ignore_input(nl, 0, 1);
  ignore_input(nl, 1, 1);
  assert(logic::block_input_count(nl) == 1);  // DFF0.D (SER)
  // Only DFF1.Q dangles into the interface: DFF0.Q is wired and both NQs
  // are ignored by default (`use` re-exposes them, covered in the pins test).
  assert(logic::block_output_count(nl) == 1);
  SeqState st;
  const LogicValue kL = LogicValue::kLow;
  const LogicValue kH = LogicValue::kHigh;
  const LogicValue ser[3] = {kH, kL, kH};
  // (Q0,Q1,out) after each edge: SER shifts through with one-edge delay.
  const LogicValue want_q0[3] = {kH, kL, kH};
  const LogicValue want_q1[3] = {kL, kH, kL};
  const LogicValue want_out[3] = {kL, kH, kL};
  for (int i = 0; i < 3; ++i) {
    LogicValue out[1] = {kL};
    assert(logic::seq_step(nl, st, std::span<const LogicValue>(&ser[i], 1), std::span<LogicValue>(out, 1)).ok());
    assert(st.q[0] == want_q0[i]);
    assert(st.q[1] == want_q1[i]);
    assert(out[0] == want_out[i]); // post-edge settled output (DFF1.Q)
  }
}

void test_swap_proves_snapshot_isolation() {
  // Direct cross: DFF0.D <- Q1, DFF1.D <- Q0, no comb. Hand-set (1,0);
  // one edge must swap to (0,1) — a cascading (in-place) update would
  // collapse both to the same value instead.
  Netlist nl;
  place_seq(nl, 0, GateKind::kDff, 1);
  place_seq(nl, 1, GateKind::kDff, 2);
  assert(logic::wire(nl, 1, 0, 0).ok()); // Q1 -> DFF0.D
  assert(logic::wire(nl, 0, 1, 0).ok()); // Q0 -> DFF1.D
  ignore_input(nl, 0, 1);
  ignore_input(nl, 1, 1);
  assert(logic::validate_sequential(nl).ok());
  SeqState st;
  logic::init_seq_state(st, nl);
  st.q[0] = LogicValue::kHigh;
  st.q[1] = LogicValue::kLow;
  assert(logic::seq_step(nl, st, std::span<const LogicValue>{}).ok());
  assert(st.q[0] == LogicValue::kLow);
  assert(st.q[1] == LogicValue::kHigh);
  assert(logic::seq_step(nl, st, std::span<const LogicValue>{}).ok());
  assert(st.q[0] == LogicValue::kHigh);
  assert(st.q[1] == LogicValue::kLow);
}

void test_dlatch_edge_capture_seq4() {
  // SEQ4 simplification: DLATCH captures D on the edge like a DFF
  // (level-transparent behavior arrives in SEQ8 and refines this path).
  Netlist nl;
  place_seq(nl, 0, GateKind::kDlatch, 1);
  ignore_input(nl, 0, 1); // EN ignored (edge assumed, validated only)
  assert(logic::block_input_count(nl) == 1); // D
  SeqState st;
  const LogicValue kL = LogicValue::kLow;
  const LogicValue kH = LogicValue::kHigh;
  LogicValue d = kH;
  assert(logic::seq_step(nl, st, std::span<const LogicValue>(&d, 1)).ok());
  assert(st.q[0] == kH);
  d = kL;
  assert(logic::seq_step(nl, st, std::span<const LogicValue>(&d, 1)).ok());
  assert(st.q[0] == kL);
}

void test_xz_and_size_rejected_state_untouched() {
  Netlist nl;
  place_seq(nl, 0, GateKind::kDff, 1);
  ignore_input(nl, 0, 1);
  assert(logic::block_input_count(nl) == 1); // D dangling
  SeqState st;
  const LogicValue kL = LogicValue::kLow;
  const LogicValue kH = LogicValue::kHigh;
  const LogicValue kZ = LogicValue::kHighZ;
  const LogicValue kX = LogicValue::kUnknown;
  LogicValue d = kH;
  assert(logic::seq_step(nl, st, std::span<const LogicValue>(&d, 1)).ok());
  assert(st.q[0] == kH);
  // HighZ / Unknown block inputs fail loudly; committed Q stays put.
  assert(!logic::seq_step(nl, st, std::span<const LogicValue>(&kZ, 1)).ok());
  assert(st.q[0] == kH);
  assert(!logic::seq_step(nl, st, std::span<const LogicValue>(&kX, 1)).ok());
  assert(st.q[0] == kH);
  // Size mismatches fail loudly (and the 3-arg overload sizes outputs itself).
  // One flop with dangling Q (NQ ignored by default): the interface
  // carries 1 output here.
  assert(!logic::seq_step(nl, st, std::span<const LogicValue>{}, std::span<LogicValue>{}).ok());
  assert(st.q[0] == kH);
  LogicValue outs[2] = {kL, kL};
  assert(!logic::seq_step(nl, st, std::span<const LogicValue>(&d, 1), std::span<LogicValue>(outs, 2)).ok());
  assert(st.q[0] == kH);
  // Valid step still works after the refusals (no wedging).
  d = kL;
  assert(logic::seq_step(nl, st, std::span<const LogicValue>(&d, 1)).ok());
  assert(st.q[0] == kL);
}

void test_t_excitation_table() {
  // Hand-derived reference (research R3, 003-jk-t-flops): T=0 holds Q,
  // T=1 inverts pre-edge Q. CLK ignored (edge assumed, validated only).
  Netlist nl;
  const std::uint16_t s = logic::add_gate(nl, GateKind::kT, 2).value;
  ignore_input(nl, s, 1);
  assert(logic::block_input_count(nl) == 1); // T
  assert(logic::validate_sequential(nl).ok());
  SeqState st;
  const LogicValue kL = LogicValue::kLow;
  const LogicValue kH = LogicValue::kHigh;
  assert(st.q[s] == kL); // power-on 0
  LogicValue t = kH;
  const std::span<const LogicValue> drive(&t, 1);
  // Toggle from 0: 0 -> 1 -> 0 -> 1.
  assert(logic::seq_step(nl, st, drive).ok());
  assert(st.q[s] == kH);
  assert(logic::seq_step(nl, st, drive).ok());
  assert(st.q[s] == kL);
  assert(logic::seq_step(nl, st, drive).ok());
  assert(st.q[s] == kH);
  // Hold from 1 across 3 edges.
  t = kL;
  assert(logic::seq_step(nl, st, drive).ok());
  assert(st.q[s] == kH);
  assert(logic::seq_step(nl, st, drive).ok());
  assert(st.q[s] == kH);
  assert(logic::seq_step(nl, st, drive).ok());
  assert(st.q[s] == kH);
  // Back to 0 via toggle, then hold from 0 across 2 edges.
  t = kH;
  assert(logic::seq_step(nl, st, drive).ok());
  assert(st.q[s] == kL);
  t = kL;
  assert(logic::seq_step(nl, st, drive).ok());
  assert(st.q[s] == kL);
  assert(logic::seq_step(nl, st, drive).ok());
  assert(st.q[s] == kL);
  // X/Z on T fails loudly with committed Q untouched.
  const LogicValue kZ = LogicValue::kHighZ;
  const LogicValue kX = LogicValue::kUnknown;
  assert(!logic::seq_step(nl, st, std::span<const LogicValue>(&kZ, 1)).ok());
  assert(st.q[s] == kL);
  assert(!logic::seq_step(nl, st, std::span<const LogicValue>(&kX, 1)).ok());
  assert(st.q[s] == kL);
}

void test_jk_excitation_table() {
  // Hand-derived reference (research R3, 003-jk-t-flops): J=0,K=0 hold;
  // J=0,K=1 reset; J=1,K=0 set; J=1,K=1 toggle from OLD Q. CLK ignored
  // (edge assumed, validated only). Block inputs are J,K in slot order.
  Netlist nl;
  const std::uint16_t s = logic::add_gate(nl, GateKind::kJk, 3).value;
  ignore_input(nl, s, 2);
  assert(logic::block_input_count(nl) == 2); // J, K
  assert(logic::validate_sequential(nl).ok());
  SeqState st;
  const LogicValue kL = LogicValue::kLow;
  const LogicValue kH = LogicValue::kHigh;
  assert(st.q[s] == kL); // power-on 0
  LogicValue jk[2] = {kL, kL};
  const std::span<const LogicValue> drive(jk, 2);
  // From Q=0: hold -> 0, reset -> 0, set -> 1, toggle -> 1.
  assert(logic::seq_step(nl, st, drive).ok());
  assert(st.q[s] == kL);
  jk[1] = kH; // 01 reset
  assert(logic::seq_step(nl, st, drive).ok());
  assert(st.q[s] == kL);
  jk[0] = kH;
  jk[1] = kL; // 10 set
  assert(logic::seq_step(nl, st, drive).ok());
  assert(st.q[s] == kH);
  // From Q=1: hold -> 1, reset -> 0.
  jk[0] = kL;
  jk[1] = kL; // 00 hold
  assert(logic::seq_step(nl, st, drive).ok());
  assert(st.q[s] == kH);
  jk[1] = kH; // 01 reset
  assert(logic::seq_step(nl, st, drive).ok());
  assert(st.q[s] == kL);
  // From Q=0: set -> 1, toggle -> 0.
  jk[0] = kH;
  jk[1] = kL; // 10 set
  assert(logic::seq_step(nl, st, drive).ok());
  assert(st.q[s] == kH);
  jk[1] = kH; // 11 toggle
  assert(logic::seq_step(nl, st, drive).ok());
  assert(st.q[s] == kL);
  // X/Z on J or K fails loudly with committed Q untouched.
  const LogicValue kZ = LogicValue::kHighZ;
  jk[0] = kZ;
  jk[1] = kL;
  assert(!logic::seq_step(nl, st, drive).ok());
  assert(st.q[s] == kL);
  jk[0] = kL;
  jk[1] = kZ;
  assert(!logic::seq_step(nl, st, drive).ok());
  assert(st.q[s] == kL);
}

void test_jk_sync_counter_2bit() {
  // 2-bit synchronous JK counter, no combinational gates: LSB J=K=1
  // (toggles every edge); MSB J=K=Q0 (toggles on carry). Both CLKs
  // ignored; J0/K0 driven High by poke-equivalent block inputs... here
  // wired High is impossible without a driver, so J0/K0 stay dangling
  // and are driven per-edge alongside the step.
  Netlist nl;
  const std::uint16_t lo = logic::add_gate(nl, GateKind::kJk, 3).value; // JK1
  const std::uint16_t hi = logic::add_gate(nl, GateKind::kJk, 3).value; // JK2
  assert(logic::wire(nl, lo, hi, 0).ok()); // Q0 -> JK2.J
  assert(logic::wire(nl, lo, hi, 1).ok()); // Q0 -> JK2.K
  ignore_input(nl, lo, 2);
  ignore_input(nl, hi, 2);
  assert(logic::validate_sequential(nl).ok());
  // Dangling block inputs in slot order: JK1.J, JK1.K.
  assert(logic::block_input_count(nl) == 2);
  SeqState st;
  const LogicValue kL = LogicValue::kLow;
  const LogicValue kH = LogicValue::kHigh;
  const LogicValue want[5][2] = {
      {kL, kL}, // 00 power-on
      {kL, kH}, // 01
      {kH, kL}, // 10
      {kH, kH}, // 11
      {kL, kL}, // 00 wrap
  };
  for (int step = 1; step <= 4; ++step) {
    LogicValue jk[2] = {kH, kH}; // LSB toggles every edge
    assert(logic::seq_step(nl, st, std::span<const LogicValue>(jk, 2)).ok());
    assert(st.q[hi] == want[step][0]);
    assert(st.q[lo] == want[step][1]);
  }
}

void test_mixed_kinds_share_one_edge() {
  // US3 (003-jk-t-flops): one DFF + one JK + one T in a single netlist
  // step together — each slot follows its own resolver from the same OLD
  // Q vector under one reported edge. Block inputs in slot order.
  Netlist nl;
  const std::uint16_t d = logic::add_gate(nl, GateKind::kDff, 2).value;
  const std::uint16_t j = logic::add_gate(nl, GateKind::kJk, 3).value;
  const std::uint16_t t = logic::add_gate(nl, GateKind::kT, 2).value;
  ignore_input(nl, d, 1);
  ignore_input(nl, j, 2);
  ignore_input(nl, t, 1);
  assert(logic::block_input_count(nl) == 4); // D, J, K, T
  assert(logic::validate_sequential(nl).ok());
  SeqState st;
  const LogicValue kL = LogicValue::kLow;
  const LogicValue kH = LogicValue::kHigh;
  // Edge 1: D=1 captures; J=K=1 toggles Q 0->1; T=1 toggles Q 0->1.
  LogicValue in[4] = {kH, kH, kH, kH};
  assert(logic::seq_step(nl, st, std::span<const LogicValue>(in, 4)).ok());
  assert(st.q[d] == kH);
  assert(st.q[j] == kH);
  assert(st.q[t] == kH);
  // Edge 2: D=0 captures; J=0,K=1 resets; T=0 holds.
  LogicValue in2[4] = {kL, kL, kH, kL};
  assert(logic::seq_step(nl, st, std::span<const LogicValue>(in2, 4)).ok());
  assert(st.q[d] == kL);
  assert(st.q[j] == kL);
  assert(st.q[t] == kH);
  // Deterministic replay: identical drives from a fresh pool match.
  SeqState st2;
  LogicValue r1[4] = {kH, kH, kH, kH};
  LogicValue r2[4] = {kL, kL, kH, kL};
  assert(logic::seq_step(nl, st2, std::span<const LogicValue>(r1, 4)).ok());
  assert(logic::seq_step(nl, st2, std::span<const LogicValue>(r2, 4)).ok());
  assert(st2.q[d] == st.q[d] && st2.q[j] == st.q[j] && st2.q[t] == st.q[t]);
}

void test_subset_stepping_holds_non_members() {
  // Selective domains cover the new kinds: stepping only the DFF member
  // leaves JK/T Q untouched for that edge.
  Netlist nl;
  const std::uint16_t d = logic::add_gate(nl, GateKind::kDff, 2).value;
  const std::uint16_t j = logic::add_gate(nl, GateKind::kJk, 3).value;
  const std::uint16_t t = logic::add_gate(nl, GateKind::kT, 2).value;
  ignore_input(nl, d, 1);
  ignore_input(nl, j, 2);
  ignore_input(nl, t, 1);
  assert(logic::validate_sequential(nl).ok());
  SeqState st;
  const LogicValue kL = LogicValue::kLow;
  const LogicValue kH = LogicValue::kHigh;
  // First a global edge with all-toggle drives so JK/T hold nonzero Q.
  LogicValue all[4] = {kH, kH, kH, kH};
  assert(logic::seq_step(nl, st, std::span<const LogicValue>(all, 4)).ok());
  assert(st.q[j] == kH && st.q[t] == kH);
  // Member-only edge: D captures 0 while JK/T would toggle — they hold.
  const std::uint16_t members[1] = {d};
  LogicValue sub[4] = {kL, kH, kH, kH};
  const std::size_t n_out = logic::block_output_count(nl);
  LogicValue outs[8] = {kL, kL, kL, kL, kL, kL, kL, kL};
  assert(logic::seq_step_filtered(nl, st, std::span<const LogicValue>(sub, 4),
                                  std::span<LogicValue>(outs, n_out), members, 1)
             .ok());
  assert(st.q[d] == kL);
  assert(st.q[j] == kH);
  assert(st.q[t] == kH);
}

void test_comb_cycle_still_rejected() {
  // A live-but-uninvolved flop must not shield a real combinational cycle:
  // seq_step takes the sequential validation path and fails loudly.
  Netlist nl;
  place_seq(nl, 0, GateKind::kDff, 1);
  ignore_input(nl, 0, 0); // D ignored so the flop is self-contained
  ignore_input(nl, 0, 1);
  const std::uint16_t a = logic::add_gate(nl, GateKind::kAnd, 2).value;
  const std::uint16_t n = logic::add_gate(nl, GateKind::kNot, 1).value;
  assert(logic::wire(nl, a, n, 0).ok());
  nl.gates[a].drivers[1] = n; // hand-poked comb cycle (wire() refuses it)
  nl.dirty = true;
  SeqState st;
  assert(!logic::validate_sequential(nl).ok());
  assert(!logic::seq_step(nl, st, std::span<const LogicValue>{}).ok());
}

void test_combinational_solve_unchanged() {
  // SEQ4 adds no eval path for combinational nets: NAND via AND+NOT still
  // solves bit-exactly with no SeqState involvement.
  Netlist nl;
  const std::uint16_t a = logic::add_gate(nl, GateKind::kAnd, 2).value;
  const std::uint16_t n = logic::add_gate(nl, GateKind::kNot, 1).value;
  assert(logic::wire(nl, a, n, 0).ok());
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
  test_counter_2bit();
  test_shift_register_parallel_update();
  test_swap_proves_snapshot_isolation();
  test_dlatch_edge_capture_seq4();
  test_t_excitation_table();
  test_jk_excitation_table();
  test_jk_sync_counter_2bit();
  test_mixed_kinds_share_one_edge();
  test_subset_stepping_holds_non_members();
  test_xz_and_size_rejected_state_untouched();
  test_comb_cycle_still_rejected();
  test_combinational_solve_unchanged();
  std::cout << "test_logic_seq_step: OK\n";
  return 0;
}
