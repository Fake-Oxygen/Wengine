// LOGIC-7 layout goldens: pure layered geometry over the compiled topology.
// Header-only (`wengine/logic/layout.hpp`), SDL-free: links wengine_core
// alone and runs headless. Pins column assignment (level -> x), stacking
// (slot order -> y), canvas extents, and Manhattan wire routes.

#include "wengine/logic/eval.hpp"
#include "wengine/logic/layout.hpp"
#include "wengine/logic/netlist.hpp"
#include "wengine/logic/types.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>

using namespace wengine;
using wengine::logic::GateKind;
using wengine::logic::Netlist;

namespace {

void test_empty() {
  Netlist nl;
  const logic::Schematic sc = logic::layout_schematic(nl);
  assert(sc.levels == 0);
  assert(sc.gates.empty());
  assert(sc.wires.empty());
  assert(sc.width == 2 * logic::kLayoutMargin);
  assert(sc.height == 2 * logic::kLayoutMargin);
}

void test_single_gate() {
  Netlist nl;
  assert(logic::add_gate(nl, GateKind::kAnd, 2).ok());
  const logic::Schematic sc = logic::layout_schematic(nl);
  assert(sc.levels == 1);
  assert(sc.gates.size() == 1);
  assert(sc.wires.empty());
  // Column 0, first row: x = margin + (colW - boxW) / 2, y = margin.
  assert(sc.gates[0].slot == 0);
  assert(sc.gates[0].level == 0);
  assert(sc.gates[0].x == 30 + (120 - 76) / 2);
  assert(sc.gates[0].y == 30);
  assert(sc.width == 60 + 1 * 120);
  assert(sc.height == 60 + 1 * (56 + 30));
}

void test_chain_columns_and_route() {
  // AND1.Y -> NOT1.1: two columns, one straight horizontal wire.
  Netlist nl;
  assert(logic::add_gate(nl, GateKind::kAnd, 2).ok()); // @0
  assert(logic::add_gate(nl, GateKind::kNot, 1).ok()); // @1
  assert(logic::wire(nl, 0, 1, 0).ok());
  const logic::Schematic sc = logic::layout_schematic(nl);
  assert(sc.levels == 2);
  assert(sc.gates.size() == 2);
  assert(sc.gates[0].slot == 0 && sc.gates[0].level == 0);
  assert(sc.gates[1].slot == 1 && sc.gates[1].level == 1);
  assert(sc.gates[0].x == 52 && sc.gates[1].x == 172);
  assert(sc.gates[0].y == 30 && sc.gates[1].y == 30);
  assert(sc.width == 300 && sc.height == 146);
  assert(sc.wires.size() == 1);
  const logic::WireRoute &r = sc.wires[0];
  assert(r.from_slot == 0 && r.to_slot == 1 && r.to_input == 0);
  // y0 = 30 + 56/2 = 58 (source mid); y1 = 30 + 1*56/2 = 58 (single input
  // mid); x0 = 52 + 76 = 128 (source right edge); x1 = 172 (sink left
  // edge); xm = (128 + 172) / 2 = 150.
  assert(r.p[0].x == 128 && r.p[0].y == 58);
  assert(r.p[1].x == 150 && r.p[1].y == 58);
  assert(r.p[2].x == 150 && r.p[2].y == 58);
  assert(r.p[3].x == 172 && r.p[3].y == 58);
}

void test_fanout_stacking_and_dogleg() {
  // AND1.Y -> NOT1.1 + BUF1.1: level 1 stacks in slot order; the second
  // wire is a true dogleg (vertical leg at the mid column).
  Netlist nl;
  assert(logic::add_gate(nl, GateKind::kAnd, 2).ok()); // @0
  assert(logic::add_gate(nl, GateKind::kNot, 1).ok()); // @1
  assert(logic::add_gate(nl, GateKind::kBuf, 1).ok()); // @2
  assert(logic::wire(nl, 0, 1, 0).ok());
  assert(logic::wire(nl, 0, 2, 0).ok());
  const logic::Schematic sc = logic::layout_schematic(nl);
  assert(sc.levels == 2);
  assert(sc.width == 300);
  assert(sc.height == 60 + 2 * 86); // two gates share level 1
  assert(sc.gates[1].x == 172 && sc.gates[1].y == 30);
  assert(sc.gates[2].x == 172 && sc.gates[2].y == 116);
  assert(sc.wires.size() == 2);
  // Destination slot order.
  assert(sc.wires[0].to_slot == 1 && sc.wires[1].to_slot == 2);
  const logic::WireRoute &r = sc.wires[1]; // AND1.Y -> BUF1.1
  assert(r.p[0].x == 128 && r.p[0].y == 58);
  assert(r.p[1].x == 150 && r.p[1].y == 58);
  assert(r.p[2].x == 150 && r.p[2].y == 144);
  assert(r.p[3].x == 172 && r.p[3].y == 144);
}

void test_input_spread_and_diamond() {
  // AND1 @0, AND2 @1 feed OR1.2 / OR1.3 (3-input OR @2): inputs spread
  // evenly down the left edge ((i+1) * boxH / (n+1)).
  Netlist nl;
  assert(logic::add_gate(nl, GateKind::kAnd, 2).ok()); // @0
  assert(logic::add_gate(nl, GateKind::kAnd, 2).ok()); // @1
  assert(logic::add_gate(nl, GateKind::kOr, 3).ok());  // @2
  assert(logic::wire(nl, 0, 2, 1).ok());
  assert(logic::wire(nl, 1, 2, 2).ok());
  const logic::Schematic sc = logic::layout_schematic(nl);
  assert(sc.levels == 2);
  assert(sc.gates[0].y == 30 && sc.gates[1].y == 116); // level 0 stacking
  assert(sc.gates[2].x == 172 && sc.gates[2].level == 1);
  assert(sc.wires.size() == 2);
  assert(sc.wires[0].to_input == 1 && sc.wires[1].to_input == 2);
  // OR1.2: y = 30 + 2*56/4 = 58; OR1.3: y = 30 + 3*56/4 = 72.
  // Sources: AND1 mid y = 58, AND2 mid y = 144.
  assert(sc.wires[0].p[0].y == 58 && sc.wires[0].p[3].y == 58);
  assert(sc.wires[1].p[0].y == 144 && sc.wires[1].p[3].y == 72);
  assert(sc.wires[0].p[1].x == 150 && sc.wires[1].p[2].x == 150);
  assert(sc.wires[0].p[3].x == 172 && sc.wires[1].p[0].x == 128);
}

void test_deep_chain() {
  // A -> B -> C: one column per level, inputs left, outputs right.
  Netlist nl;
  assert(logic::add_gate(nl, GateKind::kAnd, 2).ok()); // @0
  assert(logic::add_gate(nl, GateKind::kOr, 2).ok());   // @1
  assert(logic::add_gate(nl, GateKind::kNot, 1).ok());  // @2
  assert(logic::wire(nl, 0, 1, 0).ok());
  assert(logic::wire(nl, 1, 2, 0).ok());
  const logic::Schematic sc = logic::layout_schematic(nl);
  assert(sc.levels == 3);
  assert(sc.gates[0].x == 52 && sc.gates[1].x == 172 && sc.gates[2].x == 292);
  assert(sc.width == 60 + 3 * 120);
}

bool same_schematic(const logic::Schematic &a, const logic::Schematic &b) {
  if (a.levels != b.levels || a.width != b.width || a.height != b.height) {
    return false;
  }
  if (a.gates.size() != b.gates.size() || a.wires.size() != b.wires.size()) {
    return false;
  }
  for (std::size_t k = 0; k < a.gates.size(); ++k) {
    if (a.gates[k].slot != b.gates[k].slot || a.gates[k].level != b.gates[k].level ||
        a.gates[k].x != b.gates[k].x || a.gates[k].y != b.gates[k].y) {
      return false;
    }
  }
  for (std::size_t k = 0; k < a.wires.size(); ++k) {
    const logic::WireRoute &x = a.wires[k];
    const logic::WireRoute &y = b.wires[k];
    if (x.from_slot != y.from_slot || x.to_slot != y.to_slot || x.to_input != y.to_input) {
      return false;
    }
    for (int q = 0; q < 4; ++q) {
      if (x.p[q].x != y.p[q].x || x.p[q].y != y.p[q].y) {
        return false;
      }
    }
  }
  return true;
}

void test_deterministic() {
  Netlist nl;
  assert(logic::add_gate(nl, GateKind::kAnd, 2).ok());
  assert(logic::add_gate(nl, GateKind::kOr, 3).ok());
  assert(logic::add_gate(nl, GateKind::kNot, 1).ok());
  assert(logic::wire(nl, 0, 1, 0).ok());
  assert(logic::wire(nl, 1, 2, 0).ok());
  const logic::Schematic once = logic::layout_schematic(nl);
  const logic::Schematic twice = logic::layout_schematic(nl);
  assert(same_schematic(once, twice));
  // An identical rebuild lands on identical pixels.
  Netlist twin;
  assert(logic::add_gate(twin, GateKind::kAnd, 2).ok());
  assert(logic::add_gate(twin, GateKind::kOr, 3).ok());
  assert(logic::add_gate(twin, GateKind::kNot, 1).ok());
  assert(logic::wire(twin, 0, 1, 0).ok());
  assert(logic::wire(twin, 1, 2, 0).ok());
  assert(same_schematic(once, logic::layout_schematic(twin)));
}

} // namespace

int main() {
  test_empty();
  test_single_gate();
  test_chain_columns_and_route();
  test_fanout_stacking_and_dogleg();
  test_input_spread_and_diamond();
  test_deep_chain();
  test_deterministic();
  std::cout << "test_logic_layout: OK\n";
  return 0;
}
