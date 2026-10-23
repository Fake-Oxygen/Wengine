#include "wengine/logic/eval.hpp"
#include "wengine/logic/netlist.hpp"
#include "wengine/logic/types.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string_view>

using namespace wengine;
using wengine::logic::BlockPin;
using wengine::logic::GateKind;
using wengine::logic::LogicValue;
using wengine::logic::Netlist;
using wengine::logic::PinRef;

namespace {

constexpr LogicValue kL = LogicValue::kLow;
constexpr LogicValue kH = LogicValue::kHigh;

void test_kinds() {
  assert(logic::valid_fanin(GateKind::kAnd, 2));
  assert(logic::valid_fanin(GateKind::kAnd, 8));
  assert(!logic::valid_fanin(GateKind::kAnd, 1));
  assert(!logic::valid_fanin(GateKind::kAnd, 9));
  assert(logic::valid_fanin(GateKind::kNot, 1));
  assert(!logic::valid_fanin(GateKind::kNot, 2));
  assert(logic::valid_fanin(GateKind::kBuf, 1));
  assert(!logic::valid_fanin(GateKind::kBuf, 0));
  assert(logic::default_fanin(GateKind::kNot) == 1);
  assert(logic::default_fanin(GateKind::kAnd) == 2);

  assert(logic::parse_gate_kind("AND").ok());
  assert(logic::parse_gate_kind("and").value == GateKind::kAnd);
  assert(logic::parse_gate_kind("Xnor").value == GateKind::kXnor);
  assert(logic::parse_gate_kind("buf").value == GateKind::kBuf);
  assert(!logic::parse_gate_kind("").ok());
  assert(!logic::parse_gate_kind("NAND2").ok());
  assert(!logic::parse_gate_kind("ANDD").ok());

  // Bad fan-in is loud, netlist untouched.
  Netlist nl;
  assert(!logic::add_gate(nl, GateKind::kNot, 2).ok());
  assert(!logic::add_gate(nl, GateKind::kAnd, 1).ok());
  assert(!logic::add_gate(nl, GateKind::kAnd, 9).ok());
  assert(nl.live_count == 0);
}

void test_counters_and_slots() {
  Netlist nl;
  auto a = logic::add_gate(nl, GateKind::kAnd, 2);
  assert(a.ok() && a.value == 0);
  assert(nl.gates[0].counter == 1); // AND1
  auto b = logic::add_gate(nl, GateKind::kAnd, 2);
  assert(b.ok() && b.value == 1);
  assert(nl.gates[1].counter == 2); // AND2
  auto n = logic::add_gate(nl, GateKind::kNot, 1);
  assert(n.ok() && n.value == 2);
  assert(nl.gates[2].counter == 1); // NOT1 (per-kind)
  assert(nl.live_count == 3);

  // Remove AND1 (slot 0); slot recycles, counter never reused.
  assert(logic::remove_gate(nl, 0).ok());
  assert(nl.live_count == 2);
  auto o = logic::add_gate(nl, GateKind::kOr, 2);
  assert(o.ok() && o.value == 0); // lowest free slot
  assert(nl.gates[0].kind == GateKind::kOr && nl.gates[0].counter == 1); // OR1
  auto a3 = logic::add_gate(nl, GateKind::kAnd, 2);
  assert(a3.ok() && nl.gates[a3.value].counter == 3); // AND3, never AND1 again
  assert(!logic::remove_gate(nl, 9999).ok());
  assert(!logic::remove_gate(nl, a3.value).ok() == false); // live: ok
  assert(!logic::remove_gate(nl, a3.value).ok()); // now dead: error

  // By-name removal.
  Netlist nl2;
  assert(logic::add_gate(nl2, GateKind::kAnd, 2).ok()); // AND1
  assert(logic::add_gate(nl2, GateKind::kNot, 1).ok()); // NOT1
  assert(logic::remove_gate_by_name(nl2, "NOT1").ok());
  assert(!logic::remove_gate_by_name(nl2, "NOT1").ok());
  assert(!logic::remove_gate_by_name(nl2, "AND9").ok());
  assert(!logic::remove_gate_by_name(nl2, "bogus").ok());
}

void test_pins() {
  Netlist nl;
  assert(logic::add_gate(nl, GateKind::kAnd, 2).ok()); // AND1 @0
  assert(logic::add_gate(nl, GateKind::kAnd, 3).ok()); // AND2 @1
  assert(logic::add_gate(nl, GateKind::kNot, 1).ok()); // NOT1 @2

  auto y = logic::parse_pin(nl, "AND1.Y");
  assert(y.ok() && y.value.slot == 0 && y.value.is_output);
  auto y_low = logic::parse_pin(nl, "and1.y");
  assert(y_low.ok() && y_low.value.slot == 0 && y_low.value.is_output);
  auto i1 = logic::parse_pin(nl, "AND2.1");
  assert(i1.ok() && !i1.value.is_output && i1.value.slot == 1 && i1.value.input == 0);
  auto i3 = logic::parse_pin(nl, "AND2.3");
  assert(i3.ok() && i3.value.input == 2);
  auto ni = logic::parse_pin(nl, "NOT1.1");
  assert(ni.ok() && ni.value.slot == 2);

  assert(!logic::parse_pin(nl, "AND1").ok()); // no dot
  assert(!logic::parse_pin(nl, "AND1.Y.1").ok()); // two dots
  assert(!logic::parse_pin(nl, ".Y").ok());
  assert(!logic::parse_pin(nl, "AND1.").ok());
  assert(!logic::parse_pin(nl, "AND1.0").ok());
  assert(!logic::parse_pin(nl, "AND1.3").ok()); // AND1 has 2 inputs
  assert(!logic::parse_pin(nl, "AND1.Z").ok());
  assert(!logic::parse_pin(nl, "AND9.Y").ok()); // unknown gate
  assert(!logic::parse_pin(nl, "FOO.Y").ok());
  assert(!logic::parse_pin(nl, "NOT1.2").ok()); // NOT has 1 input

  auto g = logic::resolve_gate(nl, "not1");
  assert(g.ok() && g.value == 2);
  assert(!logic::resolve_gate(nl, "AND0").ok()); // 0 never assigned
  assert(!logic::resolve_gate(nl, "").ok());
}

void test_wire_rules() {
  Netlist nl;
  assert(logic::add_gate(nl, GateKind::kAnd, 2).ok()); // AND1 @0
  assert(logic::add_gate(nl, GateKind::kNot, 1).ok()); // NOT1 @1
  assert(logic::add_gate(nl, GateKind::kBuf, 1).ok()); // BUF1 @2

  assert(logic::wire(nl, 0, 1, 0).ok());
  assert(nl.gates[0].output_fanout == 1);
  assert(nl.dirty);

  // Occupied destination is loud; first wire intact.
  assert(!logic::wire(nl, 2, 1, 0).ok());
  assert(nl.gates[1].drivers[0] == 0);

  // Unlimited fan-out: same source to a second sink is fine.
  assert(logic::wire(nl, 0, 2, 0).ok());
  assert(nl.gates[0].output_fanout == 2);

  // Text-form wiring to an occupied pin fails the same way.
  assert(!logic::wire_text(nl, "BUF1.Y", "NOT1.1").ok());
  assert(!logic::wire_text(nl, "AND1.Y", "NOT1.1").ok());
  assert(!logic::wire_text(nl, "NOPE.Y", "NOT1.1").ok());

  // Unwire releases fanout; double-unwire is loud.
  assert(logic::unwire(nl, 2, 0).ok());
  assert(nl.gates[0].output_fanout == 1);
  assert(!logic::unwire(nl, 2, 0).ok());

  // Output-to-output and input-to-input shapes are loud.
  PinRef out0{.slot = 0, .is_output = true};
  PinRef in1{.slot = 1, .is_output = false, .input = 0};
  assert(!logic::wire_pins(nl, in1, in1).ok());
  assert(!logic::wire_pins(nl, out0, out0).ok());
}

void test_cycles() {
  Netlist nl;
  assert(logic::add_gate(nl, GateKind::kAnd, 2).ok()); // @0
  assert(logic::add_gate(nl, GateKind::kOr, 2).ok()); // @1
  assert(logic::add_gate(nl, GateKind::kNot, 1).ok()); // @2

  // Self-loop is a cycle.
  assert(!logic::wire(nl, 0, 0, 0).ok());
  assert(nl.gates[0].drivers[0] == logic::kNoDriver);

  assert(logic::wire(nl, 0, 1, 0).ok());
  assert(logic::wire(nl, 1, 2, 0).ok());
  // Closing the loop is rejected; netlist untouched.
  assert(!logic::wire(nl, 2, 0, 1).ok());
  assert(nl.gates[0].drivers[1] == logic::kNoDriver);
  assert(!logic::wire(nl, 2, 1, 1).ok()); // 1 reaches 2 already
  // Reverse pair is a 2-cycle.
  Netlist nl2;
  assert(logic::add_gate(nl2, GateKind::kBuf, 1).ok()); // @0
  assert(logic::add_gate(nl2, GateKind::kBuf, 1).ok()); // @1
  assert(logic::wire(nl2, 0, 1, 0).ok());
  assert(!logic::wire(nl2, 1, 0, 0).ok());
}

void test_remove_detaches() {
  Netlist nl;
  assert(logic::add_gate(nl, GateKind::kAnd, 2).ok()); // @0
  assert(logic::add_gate(nl, GateKind::kNot, 1).ok()); // @1
  assert(logic::wire(nl, 0, 1, 0).ok());
  assert(nl.gates[0].output_fanout == 1);
  // Removing the sink releases the source fanout.
  assert(logic::remove_gate(nl, 1).ok());
  assert(nl.gates[0].output_fanout == 0);
  // Removing the source clears sinks that it drove.
  assert(logic::add_gate(nl, GateKind::kNot, 1).ok()); // NOT2 @1
  assert(logic::wire(nl, 0, 1, 0).ok());
  assert(logic::remove_gate(nl, 0).ok());
  assert(nl.gates[1].drivers[0] == logic::kNoDriver);
}

void test_eval_gate_goldens() {
  // Hand-derived single-gate truth (independent reference).
  const LogicValue lh[2] = {kL, kH};
  const LogicValue hl[2] = {kH, kL};
  const LogicValue hh[2] = {kH, kH};
  const LogicValue ll[2] = {kL, kL};
  assert(logic::eval_gate(GateKind::kAnd, ll, 2) == kL);
  assert(logic::eval_gate(GateKind::kAnd, lh, 2) == kL);
  assert(logic::eval_gate(GateKind::kAnd, hl, 2) == kL);
  assert(logic::eval_gate(GateKind::kAnd, hh, 2) == kH);
  assert(logic::eval_gate(GateKind::kNand, hh, 2) == kL);
  assert(logic::eval_gate(GateKind::kNand, lh, 2) == kH);
  assert(logic::eval_gate(GateKind::kOr, ll, 2) == kL);
  assert(logic::eval_gate(GateKind::kOr, lh, 2) == kH);
  assert(logic::eval_gate(GateKind::kNor, ll, 2) == kH);
  assert(logic::eval_gate(GateKind::kNor, lh, 2) == kL);
  assert(logic::eval_gate(GateKind::kXor, ll, 2) == kL);
  assert(logic::eval_gate(GateKind::kXor, lh, 2) == kH);
  assert(logic::eval_gate(GateKind::kXor, hh, 2) == kL);
  assert(logic::eval_gate(GateKind::kXnor, lh, 2) == kL);
  assert(logic::eval_gate(GateKind::kXnor, hh, 2) == kH);
  assert(logic::eval_gate(GateKind::kXnor, ll, 2) == kH);
  const LogicValue h[1] = {kH};
  const LogicValue l[1] = {kL};
  assert(logic::eval_gate(GateKind::kNot, h, 1) == kL);
  assert(logic::eval_gate(GateKind::kNot, l, 1) == kH);
  assert(logic::eval_gate(GateKind::kBuf, h, 1) == kH);
  assert(logic::eval_gate(GateKind::kBuf, l, 1) == kL);

  // Fan-in 8: AND needs all eight high; OR needs any high.
  LogicValue all_h[8] = {kH, kH, kH, kH, kH, kH, kH, kH};
  LogicValue one_l[8] = {kH, kH, kH, kL, kH, kH, kH, kH};
  LogicValue all_l[8] = {kL, kL, kL, kL, kL, kL, kL, kL};
  LogicValue one_h[8] = {kL, kL, kL, kH, kL, kL, kL, kL};
  assert(logic::eval_gate(GateKind::kAnd, all_h, 8) == kH);
  assert(logic::eval_gate(GateKind::kAnd, one_l, 8) == kL);
  assert(logic::eval_gate(GateKind::kOr, all_l, 8) == kL);
  assert(logic::eval_gate(GateKind::kOr, one_h, 8) == kH);

  // XNOR odd-parity doc: XOR = odd parity, XNOR = NOT(XOR) = even parity.
  // 3-input exhaustive (2^3 rows, hand-derived).
  const LogicValue t000[3] = {kL, kL, kL};
  const LogicValue t001[3] = {kL, kL, kH};
  const LogicValue t011[3] = {kL, kH, kH};
  const LogicValue t111[3] = {kH, kH, kH};
  assert(logic::eval_gate(GateKind::kXor, t000, 3) == kL); // 0 highs: even
  assert(logic::eval_gate(GateKind::kXor, t001, 3) == kH); // 1 high: odd
  assert(logic::eval_gate(GateKind::kXor, t011, 3) == kL); // 2 highs: even
  assert(logic::eval_gate(GateKind::kXor, t111, 3) == kH); // 3 highs: odd
  assert(logic::eval_gate(GateKind::kXnor, t000, 3) == kH);
  assert(logic::eval_gate(GateKind::kXnor, t001, 3) == kL);
  assert(logic::eval_gate(GateKind::kXnor, t011, 3) == kH);
  assert(logic::eval_gate(GateKind::kXnor, t111, 3) == kL);

  // resolve_net: the single-driver funnel (V2 tri-state seam).
  LogicValue one = kH;
  LogicValue sunk = kL;
  assert(logic::resolve_net(&one, 1, sunk) && sunk == kH);
  assert(!logic::resolve_net(&one, 0, sunk));
  LogicValue two[2] = {kH, kH};
  assert(!logic::resolve_net(two, 2, sunk)); // multi-driver refused
  LogicValue z = LogicValue::kHighZ;
  assert(!logic::resolve_net(&z, 1, sunk)); // Z rejected until V2
  LogicValue x = LogicValue::kUnknown;
  assert(!logic::resolve_net(&x, 1, sunk)); // X rejected until V2
}

void test_solve_single_and() {
  // One AND1, no wires: iface in = [AND1.1, AND1.2], out = [AND1.Y].
  Netlist nl;
  assert(logic::add_gate(nl, GateKind::kAnd, 2).ok());
  assert(logic::block_input_count(nl) == 2);
  assert(logic::block_output_count(nl) == 1);
  const std::array<std::array<LogicValue, 2>, 4> vecs{{{kL, kL}, {kL, kH}, {kH, kL}, {kH, kH}}};
  const std::array<LogicValue, 4> want{kL, kL, kL, kH};
  for (std::size_t r = 0; r < 4; ++r) {
    std::array<LogicValue, 1> out{kL};
    std::span<const LogicValue> in(vecs[r].data(), 2);
    std::span<LogicValue> o(out.data(), 1);
    assert(logic::solve(nl, in, o).ok());
    assert(out[0] == want[r]);
  }
}

void test_solve_nand_chain() {
  // AND1.Y -> NOT1.1 : NAND over the two block inputs.
  Netlist nl;
  assert(logic::add_gate(nl, GateKind::kAnd, 2).ok()); // @0
  assert(logic::add_gate(nl, GateKind::kNot, 1).ok()); // @1
  assert(logic::wire(nl, 0, 1, 0).ok());
  assert(logic::block_input_count(nl) == 2);
  assert(logic::block_output_count(nl) == 1); // AND1 has fanout, only NOT1.Y
  // Levels: AND1 = 0, NOT1 = 1; order lists driver first.
  assert(logic::ensure_topology(nl).ok());
  assert(!nl.dirty && nl.topo_size == 2);
  assert(nl.topo_order[0] == 0 && nl.topo_order[1] == 1);
  assert(nl.topo_level[0] == 0 && nl.topo_level[1] == 1);

  const std::array<std::array<LogicValue, 2>, 4> vecs{{{kL, kL}, {kL, kH}, {kH, kL}, {kH, kH}}};
  const std::array<LogicValue, 4> want{kH, kH, kH, kL}; // NOT(AND)
  for (std::size_t r = 0; r < 4; ++r) {
    std::array<LogicValue, 1> out{kL};
    assert(logic::solve(nl, std::span<const LogicValue>(vecs[r].data(), 2),
                        std::span<LogicValue>(out.data(), 1))
               .ok());
    assert(out[0] == want[r]);
  }

  // Repeated solve is bit-identical; second call reuses compiled order.
  std::array<LogicValue, 2> in{kH, kH};
  for (int rep = 0; rep < 50; ++rep) {
    std::array<LogicValue, 1> out{kL};
    assert(!nl.dirty);
    assert(logic::solve(nl, std::span<const LogicValue>(in.data(), 2),
                        std::span<LogicValue>(out.data(), 1))
               .ok());
    assert(out[0] == kL);
  }
}

void test_solve_fanout_two_outputs() {
  // AND1.Y -> NOT1.1 and AND1.Y -> BUF1.1: outputs [NOT1.Y, BUF1.Y].
  Netlist nl;
  assert(logic::add_gate(nl, GateKind::kAnd, 2).ok()); // @0
  assert(logic::add_gate(nl, GateKind::kNot, 1).ok()); // @1
  assert(logic::add_gate(nl, GateKind::kBuf, 1).ok()); // @2
  assert(logic::wire(nl, 0, 1, 0).ok());
  assert(logic::wire(nl, 0, 2, 0).ok());
  assert(logic::block_input_count(nl) == 2);
  assert(logic::block_output_count(nl) == 2);
  std::array<LogicValue, 2> in{kH, kH}; // AND = 1
  std::array<LogicValue, 2> out{kL, kL};
  assert(logic::solve(nl, std::span<const LogicValue>(in.data(), 2),
                      std::span<LogicValue>(out.data(), 2))
             .ok());
  assert(out[0] == kL); // NOT(1)
  assert(out[1] == kH); // BUF(1)
}

void test_solve_fanin8_and_or() {
  Netlist nl;
  assert(logic::add_gate(nl, GateKind::kAnd, 8).ok());
  std::array<LogicValue, 8> all_h{kH, kH, kH, kH, kH, kH, kH, kH};
  std::array<LogicValue, 8> one_l{kH, kH, kH, kL, kH, kH, kH, kH};
  std::array<LogicValue, 1> out{kL};
  assert(logic::solve(nl, std::span<const LogicValue>(all_h.data(), 8),
                      std::span<LogicValue>(out.data(), 1))
             .ok());
  assert(out[0] == kH);
  assert(logic::solve(nl, std::span<const LogicValue>(one_l.data(), 8),
                      std::span<LogicValue>(out.data(), 1))
             .ok());
  assert(out[0] == kL);

  Netlist no;
  assert(logic::add_gate(no, GateKind::kOr, 8).ok());
  std::array<LogicValue, 8> all_l{kL, kL, kL, kL, kL, kL, kL, kL};
  std::array<LogicValue, 8> one_h{kL, kL, kL, kH, kL, kL, kL, kL};
  assert(logic::solve(no, std::span<const LogicValue>(all_l.data(), 8),
                      std::span<LogicValue>(out.data(), 1))
             .ok());
  assert(out[0] == kL);
  assert(logic::solve(no, std::span<const LogicValue>(one_h.data(), 8),
                      std::span<LogicValue>(out.data(), 1))
             .ok());
  assert(out[0] == kH);
}

void test_solve_xnor_chain_parity() {
  // XOR1(a,b) -> XOR2(x, c): 3-input odd parity through a cascade.
  // Hand-derived: out = parity(a,b,c).
  Netlist nl;
  assert(logic::add_gate(nl, GateKind::kXor, 2).ok()); // XOR1 @0
  assert(logic::add_gate(nl, GateKind::kXor, 2).ok()); // XOR2 @1
  assert(logic::wire(nl, 0, 1, 0).ok());
  // Iface: XOR1.1, XOR1.2, XOR2.2 (XOR2.1 wired). Order is slot order.
  assert(logic::block_input_count(nl) == 3);
  std::array<BlockPin, 4> pins{};
  std::array<BlockPin, 4> outs{};
  std::size_t nin = 0;
  std::size_t nout = 0;
  assert(logic::block_interface(nl, pins.data(), pins.size(), &nin, outs.data(), outs.size(), &nout).ok());
  assert(nin == 3);
  // Exhaustive 2^3 rows.
  for (std::uint32_t row = 0; row < 8; ++row) {
    const LogicValue a = ((row & 4) != 0) ? kH : kL;
    const LogicValue b = ((row & 2) != 0) ? kH : kL;
    const LogicValue c = ((row & 1) != 0) ? kH : kL;
    const std::array<LogicValue, 3> in{a, b, c};
    std::array<LogicValue, 1> out{kL};
    assert(logic::solve(nl, std::span<const LogicValue>(in.data(), 3),
                        std::span<LogicValue>(out.data(), 1))
               .ok());
    const int pop = ((a == kH) ? 1 : 0) + ((b == kH) ? 1 : 0) + ((c == kH) ? 1 : 0);
    const LogicValue want = ((pop % 2) == 1) ? kH : kL;
    assert(out[0] == want);
  }
}

void test_dangling_and_value_errors() {
  Netlist nl;
  assert(logic::add_gate(nl, GateKind::kAnd, 2).ok());
  std::array<LogicValue, 1> one{kH};
  std::array<LogicValue, 2> two{kH, kH};
  std::array<LogicValue, 3> three{kH, kH, kH};
  std::array<LogicValue, 1> out{kL};
  // Too few / too many block inputs fail loudly (dangling contract).
  assert(!logic::solve(nl, std::span<const LogicValue>(one.data(), 1),
                       std::span<LogicValue>(out.data(), 1))
              .ok());
  assert(!logic::solve(nl, std::span<const LogicValue>(three.data(), 3),
                       std::span<LogicValue>(out.data(), 1))
              .ok());
  // Wrong output arity fails too.
  std::array<LogicValue, 2> out2{kL, kL};
  assert(!logic::solve(nl, std::span<const LogicValue>(two.data(), 2),
                       std::span<LogicValue>(out2.data(), 2))
              .ok());
  std::array<LogicValue, 0> no_out{};
  assert(!logic::solve(nl, std::span<const LogicValue>(two.data(), 2),
                       std::span<LogicValue>(no_out.data(), 0))
              .ok());
  // Z/X block inputs are rejected (V2 tri-state slot, never silent).
  std::array<LogicValue, 2> with_z{kH, LogicValue::kHighZ};
  assert(!logic::solve(nl, std::span<const LogicValue>(with_z.data(), 2),
                       std::span<LogicValue>(out.data(), 1))
              .ok());
  std::array<LogicValue, 2> with_x{kH, LogicValue::kUnknown};
  assert(!logic::solve(nl, std::span<const LogicValue>(with_x.data(), 2),
                       std::span<LogicValue>(out.data(), 1))
              .ok());
}

void test_ignore_use_iface() {
  Netlist nl;
  assert(logic::add_gate(nl, GateKind::kAnd, 3).ok()); // AND1 @0
  assert(logic::add_gate(nl, GateKind::kBuf, 1).ok()); // BUF1 @1
  assert(logic::block_input_count(nl) == 4); // 3 + 1
  assert(logic::block_output_count(nl) == 2);

  auto p2 = logic::parse_pin(nl, "AND1.2");
  auto p3 = logic::parse_pin(nl, "AND1.3");
  assert(p2.ok() && p3.ok());
  assert(logic::ignore_pin(nl, p2.value).ok());
  assert(logic::ignore_pin(nl, p3.value).ok());
  assert(logic::ignore_pin(nl, p3.value).ok()); // idempotent
  assert(logic::block_input_count(nl) == 2); // AND1.1 + BUF1.1
  // Ignored dangling inputs tie LOW: AND(1,0,0) = 0.
  std::array<LogicValue, 2> in{kH, kH};
  std::array<LogicValue, 2> out{kL, kL};
  assert(logic::solve(nl, std::span<const LogicValue>(in.data(), 2),
                      std::span<LogicValue>(out.data(), 2))
             .ok());
  assert(out[0] == kL);
  assert(out[1] == kH);
  // use restores the interface.
  assert(logic::use_pin(nl, p2.value).ok());
  assert(logic::block_input_count(nl) == 3);
  assert(logic::use_pin(nl, p2.value).ok()); // idempotent

  // Output ignore removes a dangling output from the interface.
  auto oy = logic::parse_pin(nl, "BUF1.Y");
  assert(oy.ok());
  assert(logic::ignore_pin(nl, oy.value).ok());
  assert(logic::block_output_count(nl) == 1);
  assert(logic::use_pin(nl, oy.value).ok());
  assert(logic::block_output_count(nl) == 2);

  // Interface listing is slot-ordered and cap-checked.
  std::array<BlockPin, 8> ib{};
  std::array<BlockPin, 8> ob{};
  std::size_t nin = 0;
  std::size_t nout = 0;
  assert(logic::block_interface(nl, ib.data(), ib.size(), &nin, ob.data(), ob.size(), &nout).ok());
  assert(nin == 3 && nout == 2);
  assert(ib[0].slot == 0 && ib[0].input == 0);
  assert(ob[0].slot == 0 && ob[0].is_output);
  assert(ob[1].slot == 1 && ob[1].is_output);
  std::array<BlockPin, 1> tiny{};
  std::size_t need_in = 0;
  std::size_t need_out = 0;
  assert(!logic::block_interface(nl, tiny.data(), tiny.size(), &need_in, tiny.data(), tiny.size(),
                                 &need_out)
              .ok());
  assert(need_in == 3 && need_out == 2);
}

void test_reset() {
  Netlist nl;
  assert(logic::add_gate(nl, GateKind::kAnd, 2).ok());
  assert(logic::add_gate(nl, GateKind::kNot, 1).ok());
  assert(logic::wire(nl, 0, 1, 0).ok());
  logic::reset_netlist(nl);
  assert(nl.live_count == 0);
  assert(nl.dirty);
  assert(nl.topo_size == 0);
  assert(logic::block_input_count(nl) == 0);
  assert(logic::block_output_count(nl) == 0);
  // Counters restart after a full reset (no live ids to collide with).
  auto a = logic::add_gate(nl, GateKind::kAnd, 2);
  assert(a.ok() && a.value == 0 && nl.gates[0].counter == 1);
  // Empty solve is a no-op success.
  std::array<LogicValue, 0> no_in{};
  std::array<LogicValue, 0> no_out{};
  logic::reset_netlist(nl);
  assert(logic::solve(nl, std::span<const LogicValue>(no_in.data(), 0),
                      std::span<LogicValue>(no_out.data(), 0))
             .ok());
}

void test_noexcept_pins() {
  // Steady paths must stay noexcept (no throws) and allocation-free by
  // construction (fixed arrays + spans only; no vector/string/new).
  Netlist nl;
  std::array<LogicValue, 0> no_in{};
  std::array<LogicValue, 0> no_out{};
  static_assert(noexcept(logic::ensure_topology(nl)));
  static_assert(noexcept(logic::solve(nl, std::span<const LogicValue>(no_in.data(), 0),
                                      std::span<LogicValue>(no_out.data(), 0))));
  static_assert(noexcept(logic::block_input_count(nl)));
  static_assert(noexcept(logic::reset_netlist(nl)));
  assert(logic::ensure_topology(nl).ok());
}

} // namespace

int main() {
  test_kinds();
  test_counters_and_slots();
  test_pins();
  test_wire_rules();
  test_cycles();
  test_remove_detaches();
  test_eval_gate_goldens();
  test_solve_single_and();
  test_solve_nand_chain();
  test_solve_fanout_two_outputs();
  test_solve_fanin8_and_or();
  test_solve_xnor_chain_parity();
  test_dangling_and_value_errors();
  test_ignore_use_iface();
  test_reset();
  test_noexcept_pins();
  std::cout << "test_logic: OK\n";
  return 0;
}
