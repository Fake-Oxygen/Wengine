#include "wengine/logic/eval.hpp"
#include "wengine/logic/netlist.hpp"
#include "wengine/logic/optimize.hpp"
#include "wengine/logic/types.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string_view>
#include <vector>

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
  static_assert(noexcept(logic::bit_words_for_inputs(0)));
  static_assert(noexcept(logic::input_column_word(0, 0)));
  std::array<std::uint64_t, logic::kMaxGates> scratch{};
  static_assert(noexcept(logic::eval_block_word(nl, 0, std::span<std::uint64_t>(scratch.data(), scratch.size()),
                                                std::span<std::uint64_t>())));
  assert(logic::ensure_topology(nl).ok());
}

// Full bit-parallel == scalar sweep for one netlist: every truth row is
// solved both ways (scalar `solve` per row, word slice per 64 rows) and
// every output bit must agree. Caller builds small goldens (n <= 10 so
// full enumeration stays cheap); the mapping under test is the shared
// row convention row r -> input j = (r>>j)&1.
void check_bit_matches_scalar(Netlist &nl) {
  const std::size_t nin = logic::block_input_count(nl);
  const std::size_t nout = logic::block_output_count(nl);
  assert(nin <= 10);
  const std::uint64_t rows = static_cast<std::uint64_t>(1ULL << nin);
  const std::size_t need = logic::bit_words_for_inputs(nin);
  assert(need == ((static_cast<std::size_t>(rows) + 63) / 64));
  std::vector<std::uint64_t> scratch(logic::kMaxGates, 0);
  std::vector<std::uint64_t> words(nout == 0 ? 1 : nout, 0);
  std::vector<LogicValue> scalar_in(nin, kL);
  std::vector<LogicValue> scalar_out(nout == 0 ? 1 : nout, kL);
  std::vector<LogicValue> want(nout, kL);
  for (std::size_t w = 0; w < need; ++w) {
    std::span<std::uint64_t> out_span(words.data(), nout);
    assert(logic::eval_block_word(nl, static_cast<std::uint64_t>(w),
                                  std::span<std::uint64_t>(scratch.data(), scratch.size()),
                                  out_span)
               .ok());
    for (std::uint64_t b = 0; b < 64; ++b) {
      const std::uint64_t row = static_cast<std::uint64_t>(w) * 64 + b;
      if (row >= rows) {
        // Don't-care lanes past the last row must read zero.
        for (std::size_t o = 0; o < nout; ++o) {
          assert(((words[o] >> b) & 1ULL) == 0ULL);
        }
        continue;
      }
      for (std::size_t j = 0; j < nin; ++j) {
        scalar_in[j] = (((row >> j) & 1ULL) != 0ULL) ? kH : kL;
      }
      std::span<const LogicValue> in_span(scalar_in.data(), nin);
      std::span<LogicValue> out_ref(scalar_out.data(), nout);
      assert(logic::solve(nl, in_span, out_ref).ok());
      for (std::size_t o = 0; o < nout; ++o) {
        const bool bit = (((words[o] >> b) & 1ULL) != 0ULL);
        const bool ref = (scalar_out[o] == kH);
        assert(bit == ref);
      }
      (void)want;
    }
  }
}

void test_bit_columns() {
  assert(logic::bit_words_for_inputs(0) == 1);
  assert(logic::bit_words_for_inputs(1) == 1);
  assert(logic::bit_words_for_inputs(6) == 1);
  assert(logic::bit_words_for_inputs(7) == 2);
  assert(logic::bit_words_for_inputs(8) == 4);
  assert(logic::bit_words_for_inputs(16) == 1024);
  assert(logic::bit_words_for_inputs(31) == 0);
  assert(logic::bit_words_for_inputs(100) == 0);
  assert(logic::input_column_word(0, 0) == 0xAAAAAAAAAAAAAAAAULL);
  assert(logic::input_column_word(1, 99) == 0xCCCCCCCCCCCCCCCCULL);
  assert(logic::input_column_word(2, 7) == 0xF0F0F0F0F0F0F0F0ULL);
  assert(logic::input_column_word(5, 123) == 0xFFFFFFFF00000000ULL);
  assert(logic::input_column_word(6, 0) == 0ULL);
  assert(logic::input_column_word(6, 1) == 0xFFFFFFFFFFFFFFFFULL);
  // Column definition vs the row convention, both regimes (tiled < 6,
  // constant >= 6), over several slices and lanes.
  const std::size_t positions[8] = {0, 1, 2, 3, 5, 6, 7, 13};
  const std::uint64_t slices[5] = {0, 1, 2, 5, 12345};
  for (std::size_t pi = 0; pi < 8; ++pi) {
    const std::size_t pos = positions[pi];
    for (std::size_t wi = 0; wi < 5; ++wi) {
      const std::uint64_t word = logic::input_column_word(pos, slices[wi]);
      for (std::uint64_t b = 0; b < 64; ++b) {
        const std::uint64_t row = slices[wi] * 64 + b;
        const bool bit = (((word >> b) & 1ULL) != 0ULL);
        const bool ref = (((row >> pos) & 1ULL) != 0ULL);
        assert(bit == ref);
      }
    }
  }
}

void test_bit_matches_scalar_goldens() {
  // One AND2 (2 inputs, 1 partial word + mask).
  {
    Netlist nl;
    assert(logic::add_gate(nl, GateKind::kAnd, 2).ok());
    check_bit_matches_scalar(nl);
  }
  // NAND chain AND1.Y -> NOT1.1 (2 inputs).
  {
    Netlist nl;
    assert(logic::add_gate(nl, GateKind::kAnd, 2).ok());
    assert(logic::add_gate(nl, GateKind::kNot, 1).ok());
    assert(logic::wire(nl, 0, 1, 0).ok());
    check_bit_matches_scalar(nl);
  }
  // Fan-out AND1.Y -> NOT1.1 + BUF1.1 (2 inputs, 2 outputs).
  {
    Netlist nl;
    assert(logic::add_gate(nl, GateKind::kAnd, 2).ok());
    assert(logic::add_gate(nl, GateKind::kNot, 1).ok());
    assert(logic::add_gate(nl, GateKind::kBuf, 1).ok());
    assert(logic::wire(nl, 0, 1, 0).ok());
    assert(logic::wire(nl, 0, 2, 0).ok());
    check_bit_matches_scalar(nl);
  }
  // XOR cascade parity (3 inputs, 8 rows).
  {
    Netlist nl;
    assert(logic::add_gate(nl, GateKind::kXor, 2).ok());
    assert(logic::add_gate(nl, GateKind::kXor, 2).ok());
    assert(logic::wire(nl, 0, 1, 0).ok());
    check_bit_matches_scalar(nl);
  }
  // All 8 combinational kinds at fan-in 8 (256 rows, 4 words) plus
  // NOT/BUF single-input slices. Exhaustive here, not sampled.
  {
    const GateKind kinds[6] = {GateKind::kAnd, GateKind::kOr, GateKind::kNand,
                               GateKind::kNor, GateKind::kXor, GateKind::kXnor};
    for (std::size_t k = 0; k < 6; ++k) {
      Netlist nl;
      assert(logic::add_gate(nl, kinds[k], 8).ok());
      check_bit_matches_scalar(nl);
    }
    Netlist nn;
    assert(logic::add_gate(nn, GateKind::kNot, 1).ok());
    check_bit_matches_scalar(nn);
    Netlist nb;
    assert(logic::add_gate(nb, GateKind::kBuf, 1).ok());
    check_bit_matches_scalar(nb);
  }
  // Ignored dangling inputs tie LOW in both paths (AND3 + BUF1 with two
  // AND inputs ignored: 2 block inputs, tie-off visible in words).
  {
    Netlist nl;
    assert(logic::add_gate(nl, GateKind::kAnd, 3).ok());
    assert(logic::add_gate(nl, GateKind::kBuf, 1).ok());
    auto p2 = logic::parse_pin(nl, "AND1.2");
    auto p3 = logic::parse_pin(nl, "AND1.3");
    assert(p2.ok() && p3.ok());
    assert(logic::ignore_pin(nl, p2.value).ok());
    assert(logic::ignore_pin(nl, p3.value).ok());
    check_bit_matches_scalar(nl);
  }
  // Empty netlist: one (empty) assignment, no outputs.
  {
    Netlist nl;
    std::vector<std::uint64_t> scratch(logic::kMaxGates, 0);
    std::span<std::uint64_t> empty_out;
    assert(logic::eval_block_word(nl, 0, std::span<std::uint64_t>(scratch.data(), scratch.size()),
                                  empty_out)
               .ok());
  }
}

void test_bit_arg_errors() {
  Netlist nl;
  assert(logic::add_gate(nl, GateKind::kAnd, 2).ok());
  std::vector<std::uint64_t> scratch(logic::kMaxGates, 0);
  std::vector<std::uint64_t> out(1, 0);
  // Bad scratch / output arity fails loudly.
  {
    std::span<std::uint64_t> tiny(scratch.data(), 8);
    assert(!logic::eval_block_word(nl, 0, tiny, std::span<std::uint64_t>(out.data(), out.size()))
                .ok());
  }
  {
    std::span<std::uint64_t> no_out;
    assert(!logic::eval_block_word(nl, 0, std::span<std::uint64_t>(scratch.data(), scratch.size()),
                                   no_out)
                .ok());
  }
  // Slice past the truth (2 inputs = 4 rows = word 0 only) is loud.
  assert(!logic::eval_block_word(nl, 1, std::span<std::uint64_t>(scratch.data(), scratch.size()),
                                 std::span<std::uint64_t>(out.data(), out.size()))
              .ok());
  // Empty netlist with a non-empty output span is loud too.
  {
    Netlist empty;
    std::span<std::uint64_t> one_out(out.data(), 1);
    assert(!logic::eval_block_word(empty, 0,
                                   std::span<std::uint64_t>(scratch.data(), scratch.size()),
                                   one_out)
                .ok());
  }
}

// ---- LOGIC-4 optimizer -------------------------------------------------
// Equivalence helper: positional by default; `proj` maps each optimized
// block input to the original block input it replicates (identity when the
// level preserves the interface; explicit groups when O2 duplicates a pin
// across a tied-together inverter, e.g. NAND(a,a) -> proj {0,0}).
// Exhaustive over the ORIGINAL input space up to 10 inputs, deterministic
// xorshift samples + corners beyond.

bool netlists_equal(const Netlist &a, const Netlist &b) {
  if (a.live_count != b.live_count) {
    return false;
  }
  for (std::size_t k = 0; k < 8; ++k) {
    if (a.next_counter[k] != b.next_counter[k]) {
      return false;
    }
  }
  for (std::uint16_t s = 0; s < logic::kMaxGates; ++s) {
    const logic::Gate &x = a.gates[s];
    const logic::Gate &y = b.gates[s];
    if (x.live != y.live || x.kind != y.kind || x.counter != y.counter ||
        x.input_count != y.input_count || x.ignored_mask != y.ignored_mask ||
        x.ignore_output != y.ignore_output || x.output_fanout != y.output_fanout ||
        x.propagation_delay_ps != y.propagation_delay_ps) {
      return false;
    }
    for (std::size_t i = 0; i < logic::kMaxInputs; ++i) {
      if (x.drivers[i] != y.drivers[i]) {
        return false;
      }
    }
  }
  return true;
}

void assert_names_unique(const Netlist &nl) {
  for (std::uint16_t s = 0; s < logic::kMaxGates; ++s) {
    if (!nl.gates[s].live) {
      continue;
    }
    for (std::uint16_t t = static_cast<std::uint16_t>(s + 1); t < logic::kMaxGates; ++t) {
      if (!nl.gates[t].live) {
        continue;
      }
      assert(!(nl.gates[s].kind == nl.gates[t].kind && nl.gates[s].counter == nl.gates[t].counter));
    }
  }
}

void check_equiv_proj(Netlist &orig, Netlist &opt, const std::vector<std::size_t> &proj) {
  const std::size_t nin_o = logic::block_input_count(orig);
  const std::size_t nout_o = logic::block_output_count(orig);
  const std::size_t nin_p = logic::block_input_count(opt);
  const std::size_t nout_p = logic::block_output_count(opt);
  assert(proj.size() == nin_p);
  assert(nout_o == nout_p && nout_o > 0);
  for (std::size_t j = 0; j < nin_p; ++j) {
    assert(proj[j] < nin_o);
  }
  std::vector<LogicValue> in_o(nin_o == 0 ? 1 : nin_o, kL);
  std::vector<LogicValue> in_p(nin_p == 0 ? 1 : nin_p, kL);
  std::vector<LogicValue> oo(nout_o, kL);
  std::vector<LogicValue> pp(nout_p, kL);
  auto run_row = [&]() {
    for (std::size_t j = 0; j < nin_p; ++j) {
      in_p[j] = in_o[proj[j]];
    }
    assert(logic::solve(orig, std::span<const LogicValue>(in_o.data(), nin_o),
                         std::span<LogicValue>(oo.data(), nout_o))
                .ok());
    assert(logic::solve(opt, std::span<const LogicValue>(in_p.data(), nin_p),
                        std::span<LogicValue>(pp.data(), nout_p))
                .ok());
    for (std::size_t o = 0; o < nout_o; ++o) {
      assert(oo[o] == pp[o]);
    }
  };
  if (nin_o <= 10) {
    const auto rows = static_cast<std::uint64_t>(1ULL << nin_o);
    for (std::uint64_t r = 0; r < rows; ++r) {
      for (std::size_t j = 0; j < nin_o; ++j) {
        in_o[j] = (((r >> j) & 1ULL) != 0ULL) ? kH : kL;
      }
      run_row();
    }
    return;
  }
  for (std::size_t j = 0; j < nin_o; ++j) {
    in_o[j] = kL;
  }
  run_row();
  for (std::size_t j = 0; j < nin_o; ++j) {
    in_o[j] = kH;
  }
  run_row();
  for (std::size_t w = 0; w < nin_o; ++w) {
    for (std::size_t j = 0; j < nin_o; ++j) {
      in_o[j] = (j == w) ? kH : kL;
    }
    run_row();
    for (std::size_t j = 0; j < nin_o; ++j) {
      in_o[j] = (j == w) ? kL : kH;
    }
    run_row();
  }
  std::uint32_t st = 0xC10C4E96u;
  for (std::size_t rep = 0; rep < 1024; ++rep) {
    st ^= st << 13;
    st ^= st >> 17;
    st ^= st << 5;
    std::uint32_t v = st;
    for (std::size_t j = 0; j < nin_o; ++j) {
      in_o[j] = (((v >> (j % 32)) & 1u) != 0u) ? kH : kL;
      if ((j % 32) == 31) {
        st ^= st << 13;
        st ^= st >> 17;
        st ^= st << 5;
        v = st;
      }
    }
    run_row();
  }
}

void check_equiv(Netlist &orig, Netlist &opt) {
  const std::size_t nin = logic::block_input_count(orig);
  assert(logic::block_input_count(opt) == nin);
  std::vector<std::size_t> proj(nin, 0);
  for (std::size_t j = 0; j < nin; ++j) {
    proj[j] = j;
  }
  check_equiv_proj(orig, opt, proj);
}

void test_optimize_parses() {
  assert(logic::parse_opt_level("O0").value == logic::OptLevel::kO0);
  assert(logic::parse_opt_level("O1").value == logic::OptLevel::kO1);
  assert(logic::parse_opt_level("o2").value == logic::OptLevel::kO2);
  assert(!logic::parse_opt_level("").ok());
  assert(!logic::parse_opt_level("O3").ok());
  assert(!logic::parse_opt_level("O").ok());
  assert(!logic::parse_opt_level("0").ok());
  assert(!logic::parse_opt_level("nand").ok());
  assert(logic::parse_opt_target("nand").value == logic::OptTarget::kNand);
  assert(logic::parse_opt_target("NAND").value == logic::OptTarget::kNand);
  assert(logic::parse_opt_target("Nor").value == logic::OptTarget::kNor);
  assert(!logic::parse_opt_target("").ok());
  assert(!logic::parse_opt_target("and").ok());
  assert(!logic::parse_opt_target("nandx").ok());
  assert(!logic::parse_opt_target("O1").ok());
}

void test_optimize_purity() {
  Netlist nl;
  assert(logic::add_gate(nl, GateKind::kAnd, 2).ok()); // AND1 @0
  assert(logic::add_gate(nl, GateKind::kNot, 1).ok()); // NOT1 @1
  assert(logic::add_gate(nl, GateKind::kOr, 2).ok());  // OR1 @2
  assert(logic::add_gate(nl, GateKind::kBuf, 1).ok()); // BUF1 @3
  assert(logic::wire(nl, 0, 1, 0).ok());
  assert(logic::wire(nl, 0, 2, 0).ok());
  assert(logic::wire(nl, 1, 3, 0).ok());
  const Netlist snapshot = nl;
  const logic::OptLevel levels[3] = {logic::OptLevel::kO0, logic::OptLevel::kO1,
                                     logic::OptLevel::kO2};
  for (int li = 0; li < 3; ++li) {
    for (int ti = 0; ti < 2; ++ti) {
      logic::OptimizeOptions opts;
      opts.level = levels[li];
      opts.target = (ti == 0) ? logic::OptTarget::kNand : logic::OptTarget::kNor;
      auto r = logic::optimize(nl, opts);
      assert(r.ok());
      assert_names_unique(r.value);
      assert(netlists_equal(nl, snapshot)); // input untouched at every level
    }
  }
}

void test_optimize_o0_bypass() {
  // AND1(a,b) -> NOT1 -> NOT2 -> OR1(c): internal double negation folds,
  // 4 gates -> 2, interface (3 in, 1 out) preserved positionally.
  Netlist nl;
  assert(logic::add_gate(nl, GateKind::kAnd, 2).ok());
  assert(logic::add_gate(nl, GateKind::kNot, 1).ok());
  assert(logic::add_gate(nl, GateKind::kNot, 1).ok());
  assert(logic::add_gate(nl, GateKind::kOr, 2).ok());
  assert(logic::wire(nl, 0, 1, 0).ok());
  assert(logic::wire(nl, 1, 2, 0).ok());
  assert(logic::wire(nl, 2, 3, 0).ok());
  logic::OptimizeOptions o0;
  auto r = logic::optimize(nl, o0);
  assert(r.ok());
  Netlist opt = r.value;
  assert_names_unique(opt);
  assert(nl.live_count == 4 && opt.live_count == 2);
  assert(logic::block_input_count(opt) == 3);
  assert(logic::block_output_count(opt) == 1);
  check_equiv(nl, opt); // OR(AND(a,b),c) both ways, exhaustive 2^3
}

void test_optimize_o0_buf() {
  // AND1(a,b) -> BUF1 -> NOT1(out): BUF bypass, 3 -> 2.
  Netlist nl;
  assert(logic::add_gate(nl, GateKind::kAnd, 2).ok());
  assert(logic::add_gate(nl, GateKind::kBuf, 1).ok());
  assert(logic::add_gate(nl, GateKind::kNot, 1).ok());
  assert(logic::wire(nl, 0, 1, 0).ok());
  assert(logic::wire(nl, 1, 2, 0).ok());
  logic::OptimizeOptions o0;
  auto r = logic::optimize(nl, o0);
  assert(r.ok());
  Netlist opt = r.value;
  assert(opt.live_count == 2);
  check_equiv(nl, opt); // NOT(AND(a,b)) both ways
}

void test_optimize_o0_const_fold() {
  // AND1(a, tied-0) -> NOT1(out): the AND is constant-0, the output folds
  // to constant-1; 2 -> 1 gate, 1 -> 0 block inputs.
  Netlist nl;
  assert(logic::add_gate(nl, GateKind::kAnd, 2).ok());
  assert(logic::add_gate(nl, GateKind::kNot, 1).ok());
  assert(logic::wire(nl, 0, 1, 0).ok());
  auto pin = logic::parse_pin(nl, "AND1.2");
  assert(pin.ok());
  assert(logic::ignore_pin(nl, pin.value).ok()); // AND1.2 ties to 0
  logic::OptimizeOptions o0;
  auto r = logic::optimize(nl, o0);
  assert(r.ok());
  Netlist opt = r.value;
  assert(opt.live_count == 1);
  assert(logic::block_input_count(opt) == 0);
  assert(logic::block_output_count(opt) == 1);
  check_equiv_proj(nl, opt, std::vector<std::size_t>{}); // vacuous: HIGH for both rows
  std::array<LogicValue, 1> out{kL};
  assert(logic::solve(opt, std::span<const LogicValue>(), std::span<LogicValue>(out.data(), 1)).ok());
  assert(out[0] == kH);
}

void test_optimize_o0_identity() {
  // OR1(a, tied-0) -> out: identity drops the tied input, 2-fan-in OR
  // becomes single-input BUF. Gate count stays 1.
  {
    Netlist nl;
    assert(logic::add_gate(nl, GateKind::kOr, 2).ok());
    auto pin = logic::parse_pin(nl, "OR1.2");
    assert(pin.ok());
    assert(logic::ignore_pin(nl, pin.value).ok());
    logic::OptimizeOptions o0;
    auto r = logic::optimize(nl, o0);
    assert(r.ok());
    Netlist opt = r.value;
    assert(opt.live_count == 1);
    assert(opt.gates[0].live && opt.gates[0].kind == GateKind::kBuf);
    assert(opt.gates[0].input_count == 1);
    check_equiv(nl, opt);
  }
  // AND2(C1, b) -> out with C1 = NAND(all-tied): AND(x,1) -> x drops the
  // constant input, the constant source is DCE'd. 2 -> 1.
  {
    Netlist nl;
    assert(logic::add_gate(nl, GateKind::kNand, 2).ok()); // NAND1: the C1 source
    auto p1 = logic::parse_pin(nl, "NAND1.1");
    auto p2 = logic::parse_pin(nl, "NAND1.2");
    assert(p1.ok() && p2.ok());
    assert(logic::ignore_pin(nl, p1.value).ok());
    assert(logic::ignore_pin(nl, p2.value).ok());
    assert(logic::add_gate(nl, GateKind::kAnd, 2).ok()); // AND1
    assert(logic::wire(nl, 0, 1, 0).ok());
    logic::OptimizeOptions o0;
    auto r = logic::optimize(nl, o0);
    assert(r.ok());
    Netlist opt = r.value;
    assert(opt.live_count == 1);
    assert(logic::block_input_count(opt) == 1);
    check_equiv(nl, opt); // AND(1,b) == b
  }
  // AND(a, tied-0) -> OR1 -> out: the annihilator propagates through
  // the OR's drop rule; the dead AND is DCE'd. 2 -> 1 gate, 2 -> 1 input
  // (original pin 1 survives as the only optimized pin).
  {
    Netlist nl;
    assert(logic::add_gate(nl, GateKind::kAnd, 2).ok());
    auto pa = logic::parse_pin(nl, "AND1.2");
    assert(pa.ok());
    assert(logic::ignore_pin(nl, pa.value).ok());
    assert(logic::add_gate(nl, GateKind::kOr, 2).ok());
    assert(logic::wire(nl, 0, 1, 0).ok());
    logic::OptimizeOptions o0;
    auto r = logic::optimize(nl, o0);
    assert(r.ok());
    Netlist opt = r.value;
    assert(opt.live_count == 1);
    assert(logic::block_input_count(opt) == 1);
    const std::vector<std::size_t> proj{1};
    check_equiv_proj(nl, opt, proj);
  }
}

void test_optimize_o0_keeps_ignored_cone() {
  // Ignored outputs hide logic but do not kill it: the cone feeding an
  // ignored output survives O0 (DCE roots include ignored sinks).
  Netlist nl;
  assert(logic::add_gate(nl, GateKind::kAnd, 2).ok());
  assert(logic::add_gate(nl, GateKind::kNot, 1).ok());
  assert(logic::wire(nl, 0, 1, 0).ok());
  auto oy = logic::parse_pin(nl, "NOT1.Y");
  assert(oy.ok());
  assert(logic::ignore_pin(nl, oy.value).ok());
  assert(logic::block_output_count(nl) == 0);
  logic::OptimizeOptions o0;
  auto r = logic::optimize(nl, o0);
  assert(r.ok());
  assert(r.value.live_count == 2); // nothing removed
}

void assert_o1_canonical(const Netlist &nl) {
  for (std::uint16_t s = 0; s < logic::kMaxGates; ++s) {
    const logic::Gate &g = nl.gates[s];
    if (!g.live || g.kind != GateKind::kNot) {
      continue;
    }
    const std::uint16_t d = g.drivers[0];
    if (d == logic::kNoDriver) {
      continue;
    }
    assert(nl.gates[d].live);
    if (g.output_fanout == 0) {
      // Output NOT: only a preserved double-negation may remain.
      assert(nl.gates[d].kind == GateKind::kNot);
    } else {
      assert(false); // internal NOT must dangle after O1
    }
  }
}

void test_optimize_o1_demorgan() {
  // NOT(AND) -> NAND, NOT(OR) -> NOR, NOT(NAND) -> AND, NOT(XOR) -> XNOR:
  // each 2 -> 1 with the fused kind, exhaustive equivalence.
  const GateKind inner[4] = {GateKind::kAnd, GateKind::kOr, GateKind::kNand, GateKind::kXor};
  const GateKind fused[4] = {GateKind::kNand, GateKind::kNor, GateKind::kAnd, GateKind::kXnor};
  for (int k = 0; k < 4; ++k) {
    Netlist nl;
    assert(logic::add_gate(nl, inner[k], 2).ok());
    assert(logic::add_gate(nl, GateKind::kNot, 1).ok());
    assert(logic::wire(nl, 0, 1, 0).ok());
    logic::OptimizeOptions o1;
    o1.level = logic::OptLevel::kO1;
    auto r = logic::optimize(nl, o1);
    assert(r.ok());
    Netlist opt = r.value;
    assert_names_unique(opt);
    assert(opt.live_count == 1);
    assert(logic::block_input_count(opt) == 2);
    bool found = false;
    for (std::uint16_t s = 0; s < logic::kMaxGates; ++s) {
      if (opt.gates[s].live) {
        assert(opt.gates[s].kind == fused[k]);
        found = true;
      }
    }
    assert(found);
    assert_o1_canonical(opt);
    check_equiv(nl, opt);
  }
  // Internal double negation collapses through fusion:
  // AND(a,b) -> NOT -> NOT(out) becomes a single AND. 3 -> 1.
  {
    Netlist nl;
    assert(logic::add_gate(nl, GateKind::kAnd, 2).ok());
    assert(logic::add_gate(nl, GateKind::kNot, 1).ok());
    assert(logic::add_gate(nl, GateKind::kNot, 1).ok());
    assert(logic::wire(nl, 0, 1, 0).ok());
    assert(logic::wire(nl, 1, 2, 0).ok());
    logic::OptimizeOptions o1;
    o1.level = logic::OptLevel::kO1;
    auto r = logic::optimize(nl, o1);
    assert(r.ok());
    Netlist opt = r.value;
    assert(opt.live_count == 1);
    assert(opt.gates[opt.topo_order[0]].kind == GateKind::kAnd);
    check_equiv(nl, opt);
  }
  // Pure output double negation is preserved (interface stability wins):
  // NOT(a) -> NOT(out) stays 2 gates.
  {
    Netlist nl;
    assert(logic::add_gate(nl, GateKind::kNot, 1).ok());
    assert(logic::add_gate(nl, GateKind::kNot, 1).ok());
    assert(logic::wire(nl, 0, 1, 0).ok());
    logic::OptimizeOptions o1;
    o1.level = logic::OptLevel::kO1;
    auto r = logic::optimize(nl, o1);
    assert(r.ok());
    assert(r.value.live_count == 2);
    assert_o1_canonical(r.value);
    Netlist opt = r.value;
    check_equiv(nl, opt);
  }
}

void test_optimize_o2_nand_family() {
  logic::OptimizeOptions o2;
  o2.level = logic::OptLevel::kO2;
  o2.target = logic::OptTarget::kNand;
  auto all_nand = [](const Netlist &nl) {
    for (std::uint16_t s = 0; s < logic::kMaxGates; ++s) {
      if (nl.gates[s].live) {
        assert(nl.gates[s].kind == GateKind::kNand);
      }
    }
  };
  // OR(a,b) -> NAND(~a,~b): 1 -> 3, inverter pins duplicate positionally.
  {
    Netlist nl;
    assert(logic::add_gate(nl, GateKind::kOr, 2).ok());
    auto r = logic::optimize(nl, o2);
    assert(r.ok());
    Netlist opt = r.value;
    assert(opt.live_count == 3);
    all_nand(opt);
    const std::vector<std::size_t> proj{0, 0, 1, 1};
    check_equiv_proj(nl, opt, proj);
  }
  // XOR(a,b) -> 4 NANDs: 1 -> 4.
  {
    Netlist nl;
    assert(logic::add_gate(nl, GateKind::kXor, 2).ok());
    auto r = logic::optimize(nl, o2);
    assert(r.ok());
    Netlist opt = r.value;
    assert(opt.live_count == 4);
    all_nand(opt);
    const std::vector<std::size_t> proj{0, 1, 0, 1};
    check_equiv_proj(nl, opt, proj);
  }
  // AND(a,b) -> NOT(out): O1 fuses to one NAND, O2 keeps it. 2 -> 1.
  {
    Netlist nl;
    assert(logic::add_gate(nl, GateKind::kAnd, 2).ok());
    assert(logic::add_gate(nl, GateKind::kNot, 1).ok());
    assert(logic::wire(nl, 0, 1, 0).ok());
    auto r = logic::optimize(nl, o2);
    assert(r.ok());
    Netlist opt = r.value;
    assert(opt.live_count == 1);
    all_nand(opt);
    check_equiv(nl, opt);
  }
  // BUF(a) -> out: double negation, 1 -> 2, duplicated inverter pin.
  {
    Netlist nl;
    assert(logic::add_gate(nl, GateKind::kBuf, 1).ok());
    auto r = logic::optimize(nl, o2);
    assert(r.ok());
    Netlist opt = r.value;
    assert(opt.live_count == 2);
    all_nand(opt);
    const std::vector<std::size_t> proj{0, 0};
    check_equiv_proj(nl, opt, proj);
  }
}

void test_optimize_o2_nor_family() {
  logic::OptimizeOptions o2;
  o2.level = logic::OptLevel::kO2;
  o2.target = logic::OptTarget::kNor;
  auto all_nor = [](const Netlist &nl) {
    for (std::uint16_t s = 0; s < logic::kMaxGates; ++s) {
      if (nl.gates[s].live) {
        assert(nl.gates[s].kind == GateKind::kNor);
      }
    }
  };
  // AND(a,b) -> NOR(~a,~b): 1 -> 3.
  {
    Netlist nl;
    assert(logic::add_gate(nl, GateKind::kAnd, 2).ok());
    auto r = logic::optimize(nl, o2);
    assert(r.ok());
    Netlist opt = r.value;
    assert(opt.live_count == 3);
    all_nor(opt);
    const std::vector<std::size_t> proj{0, 0, 1, 1};
    check_equiv_proj(nl, opt, proj);
  }
  // XOR(a,b) -> 5 NORs: 1 -> 5.
  {
    Netlist nl;
    assert(logic::add_gate(nl, GateKind::kXor, 2).ok());
    auto r = logic::optimize(nl, o2);
    assert(r.ok());
    Netlist opt = r.value;
    assert(opt.live_count == 5);
    all_nor(opt);
    const std::vector<std::size_t> proj{0, 1, 0, 0, 1, 1};
    check_equiv_proj(nl, opt, proj);
  }
  // OR(a,b) -> NOT(out): fused to one NOR. 2 -> 1.
  {
    Netlist nl;
    assert(logic::add_gate(nl, GateKind::kOr, 2).ok());
    assert(logic::add_gate(nl, GateKind::kNot, 1).ok());
    assert(logic::wire(nl, 0, 1, 0).ok());
    auto r = logic::optimize(nl, o2);
    assert(r.ok());
    Netlist opt = r.value;
    assert(opt.live_count == 1);
    all_nor(opt);
    check_equiv(nl, opt);
  }
}

void test_optimize_o2_multioutput_order() {
  // Two outputs keep their relative order and functions positionally:
  // AND(a,b)->NOT(o1), OR(c,d)->NOT(o2). O1 fuses both (4 -> 2 NANDs),
  // O2 keeps the single gates. Exhaustive over 2^4.
  Netlist nl;
  assert(logic::add_gate(nl, GateKind::kAnd, 2).ok()); // @0
  assert(logic::add_gate(nl, GateKind::kNot, 1).ok()); // @1 -> o1
  assert(logic::add_gate(nl, GateKind::kOr, 2).ok());  // @2
  assert(logic::add_gate(nl, GateKind::kNot, 1).ok()); // @3 -> o2
  assert(logic::wire(nl, 0, 1, 0).ok());
  assert(logic::wire(nl, 2, 3, 0).ok());
  assert(logic::block_output_count(nl) == 2);
  {
    // O1 fuses both NOTs (4 -> 2 NAND/NOR gates); outputs stay positional.
    logic::OptimizeOptions o1;
    o1.level = logic::OptLevel::kO1;
    auto r = logic::optimize(nl, o1);
    assert(r.ok());
    Netlist opt = r.value;
    assert(opt.live_count == 2);
    assert(logic::block_input_count(opt) == 4);
    assert(logic::block_output_count(opt) == 2);
    check_equiv(nl, opt); // [NAND(a,b), NOR(c,d)] positionally
  }
  {
    // O2(nand): the NAND stays single, the NOR expands to 4 NANDs (2 -> 5).
    logic::OptimizeOptions o2;
    o2.level = logic::OptLevel::kO2;
    auto r = logic::optimize(nl, o2);
    assert(r.ok());
    Netlist opt = r.value;
    assert(opt.live_count == 5);
    assert(logic::block_input_count(opt) == 6);
    assert(logic::block_output_count(opt) == 2);
    const std::vector<std::size_t> proj{0, 1, 2, 2, 3, 3};
    check_equiv_proj(nl, opt, proj);
  }
}

void test_optimize_o2_const_pool() {
  // A constant-0 output (AND with both inputs tied) converts under NAND
  // through the shared C0 pool: 1 -> 4 gates, still 0 inputs, still LOW.
  Netlist nl;
  assert(logic::add_gate(nl, GateKind::kAnd, 2).ok());
  auto p1 = logic::parse_pin(nl, "AND1.1");
  auto p2 = logic::parse_pin(nl, "AND1.2");
  assert(p1.ok() && p2.ok());
  assert(logic::ignore_pin(nl, p1.value).ok());
  assert(logic::ignore_pin(nl, p2.value).ok());
  logic::OptimizeOptions o2;
  o2.level = logic::OptLevel::kO2;
  auto r = logic::optimize(nl, o2);
  assert(r.ok());
  Netlist opt = r.value;
  assert(opt.live_count == 4);
  for (std::uint16_t s = 0; s < logic::kMaxGates; ++s) {
    if (opt.gates[s].live) {
      assert(opt.gates[s].kind == GateKind::kNand);
    }
  }
  assert(logic::block_input_count(opt) == 0);
  check_equiv_proj(nl, opt, std::vector<std::size_t>{});
  std::array<LogicValue, 1> out{kH};
  assert(logic::solve(opt, std::span<const LogicValue>(), std::span<LogicValue>(out.data(), 1)).ok());
  assert(out[0] == kL);
}

void test_optimize_o2_overflow() {
  // 40 XNOR8 (8 dangling inputs each): O2 needs 40*29 = 1160 > 1024 gates
  // and must fail loudly; O0/O1 never grow and succeed; input untouched.
  Netlist nl;
  for (int k = 0; k < 40; ++k) {
    assert(logic::add_gate(nl, GateKind::kXnor, 8).ok());
  }
  assert(nl.live_count == 40);
  const Netlist snapshot = nl;
  logic::OptimizeOptions o0;
  assert(logic::optimize(nl, o0).ok());
  logic::OptimizeOptions o1;
  o1.level = logic::OptLevel::kO1;
  assert(logic::optimize(nl, o1).ok());
  logic::OptimizeOptions o2;
  o2.level = logic::OptLevel::kO2;
  auto r = logic::optimize(nl, o2);
  assert(!r.ok());
  assert(r.error.find("budget") != std::string_view::npos);
  assert(netlists_equal(nl, snapshot));
  // Just under the cap fits exactly: 35 XNOR8 -> 35*29 = 1015 gates.
  Netlist small;
  for (int k = 0; k < 35; ++k) {
    assert(logic::add_gate(small, GateKind::kXnor, 8).ok());
  }
  auto ok = logic::optimize(small, o2);
  assert(ok.ok());
  assert(ok.value.live_count == 1015);
}

void test_optimize_sampled_large() {
  // 12 block inputs exceed the exhaustive cap (10): the sampled path runs
  // (corners + deterministic xorshift). AND8 + OR4 feeding XOR2.
  Netlist nl;
  assert(logic::add_gate(nl, GateKind::kAnd, 8).ok()); // @0: inputs 0..7
  assert(logic::add_gate(nl, GateKind::kOr, 4).ok());  // @1: inputs 8..11
  assert(logic::add_gate(nl, GateKind::kXor, 2).ok()); // @2: out
  assert(logic::wire(nl, 0, 2, 0).ok());
  assert(logic::wire(nl, 1, 2, 1).ok());
  assert(logic::block_input_count(nl) == 12);
  logic::OptimizeOptions o1;
  o1.level = logic::OptLevel::kO1;
  {
    auto r = logic::optimize(nl, o1);
    assert(r.ok());
    Netlist opt = r.value;
    assert(opt.live_count == 3); // nothing to fuse, nothing dead
    check_equiv(nl, opt);        // sampled (12 > 10)
  }
  logic::OptimizeOptions o2;
  o2.level = logic::OptLevel::kO2;
  {
    auto r = logic::optimize(nl, o2);
    assert(r.ok());
    Netlist opt = r.value;
    // AND8 -> NAND+NOT (2), OR4 -> 4 inverters + mid (5), XOR2 -> 4.
    assert(opt.live_count == 11);
    assert(logic::block_input_count(opt) == 16);
    const std::vector<std::size_t> proj{0, 1, 2, 3, 4, 5, 6, 7, 8, 8, 9, 9, 10, 10, 11, 11};
    check_equiv_proj(nl, opt, proj); // sampled (12 > 10)
  }
}

void test_optimize_report() {
  // Gate-count report golden (the before/after numbers LOGIC-8's `optimize`
  // verb will print): AND(a,b) -> NOT -> NOT -> OR(c) -> out, 4 gates.
  Netlist nl;
  assert(logic::add_gate(nl, GateKind::kAnd, 2).ok());
  assert(logic::add_gate(nl, GateKind::kNot, 1).ok());
  assert(logic::add_gate(nl, GateKind::kNot, 1).ok());
  assert(logic::add_gate(nl, GateKind::kOr, 2).ok());
  assert(logic::wire(nl, 0, 1, 0).ok());
  assert(logic::wire(nl, 1, 2, 0).ok());
  assert(logic::wire(nl, 2, 3, 0).ok());
  const std::uint16_t before = nl.live_count;
  assert(before == 4);
  logic::OptimizeOptions o0;
  auto r0 = logic::optimize(nl, o0);
  assert(r0.ok() && r0.value.live_count == 2);
  logic::OptimizeOptions o1;
  o1.level = logic::OptLevel::kO1;
  auto r1 = logic::optimize(nl, o1);
  assert(r1.ok() && r1.value.live_count == 2);
  logic::OptimizeOptions o2n;
  o2n.level = logic::OptLevel::kO2;
  auto r2n = logic::optimize(nl, o2n);
  assert(r2n.ok() && r2n.value.live_count == 5);
  logic::OptimizeOptions o2r;
  o2r.level = logic::OptLevel::kO2;
  o2r.target = logic::OptTarget::kNor;
  auto r2r = logic::optimize(nl, o2r);
  assert(r2r.ok() && r2r.value.live_count == 5);
  std::cout << "optimize report: O0: " << before << " -> " << r0.value.live_count << "; O1: " << before
            << " -> " << r1.value.live_count << "; O2(nand): " << before << " -> "
            << r2n.value.live_count << "; O2(nor): " << before << " -> " << r2r.value.live_count
            << '\n';
  Netlist o0n = r0.value;
  check_equiv(nl, o0n);
}

void test_optimize_noexcept() {
  Netlist nl;
  static_assert(noexcept(logic::parse_opt_level("O0")));
  static_assert(noexcept(logic::parse_opt_target("nand")));
  logic::OptimizeOptions opts;
  static_assert(noexcept(logic::optimize(nl, opts)));
  assert(logic::optimize(nl, opts).ok()); // empty net optimizes to empty
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
  test_bit_columns();
  test_bit_matches_scalar_goldens();
  test_bit_arg_errors();
  test_optimize_parses();
  test_optimize_purity();
  test_optimize_o0_bypass();
  test_optimize_o0_buf();
  test_optimize_o0_const_fold();
  test_optimize_o0_identity();
  test_optimize_o0_keeps_ignored_cone();
  test_optimize_o1_demorgan();
  test_optimize_o2_nand_family();
  test_optimize_o2_nor_family();
  test_optimize_o2_multioutput_order();
  test_optimize_o2_const_pool();
  test_optimize_o2_overflow();
  test_optimize_sampled_large();
  test_optimize_report();
  test_optimize_noexcept();
  std::cout << "test_logic: OK\n";
  return 0;
}
