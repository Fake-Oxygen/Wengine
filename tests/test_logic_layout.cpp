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
  // wire is a true dogleg (vertical leg at its lane). Same-gap wires take
  // deterministic lanes (midpoint, midpoint + 8, ...) so unrelated trunks
  // never coincide: the first wire keeps xm = 150, the second takes 158.
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
  assert(r.p[1].x == 158 && r.p[1].y == 58);
  assert(r.p[2].x == 158 && r.p[2].y == 144);
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
  // First same-gap wire keeps the midpoint lane; the second staggers +8.
  assert(sc.wires[0].p[1].x == 150 && sc.wires[1].p[2].x == 158);
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

// ---- 008-seq-schematic-symbols: role-based sequential anchors --------
// Contract contracts/layout-anchors.md C2/C3: box 76x56, rows in 6ths.
// Row y-offsets for h=56: r1: 1*56/6=9, r2: 2*56/6=18, r3: 28, r4: 37,
// r5: 5*56/6=46. Q anchor mid (28); NQ anchor row 5 (46).

namespace {

void test_seq_anchor_rows() {
  // Role -> row numerators (contract C2).
  assert(logic::seq_input_row(GateKind::kDff, 0) == 1);     // D
  assert(logic::seq_input_row(GateKind::kDff, 1) == 2);     // CLK
  assert(logic::seq_input_row(GateKind::kDff, 2) == 3);     // RST
  assert(logic::seq_input_row(GateKind::kDff, 3) == 4);     // SET
  assert(logic::seq_input_row(GateKind::kDff, 4) == 5);     // EN
  assert(logic::seq_input_row(GateKind::kDlatch, 0) == 1);  // D
  assert(logic::seq_input_row(GateKind::kDlatch, 1) == 2);  // EN
  assert(logic::seq_input_row(GateKind::kJk, 0) == 1);      // J
  assert(logic::seq_input_row(GateKind::kJk, 1) == 4);      // K
  assert(logic::seq_input_row(GateKind::kJk, 2) == 2);      // CLK
  assert(logic::seq_input_row(GateKind::kT, 0) == 1);       // T
  assert(logic::seq_input_row(GateKind::kT, 1) == 2);       // CLK
  assert(logic::seq_input_row(GateKind::kSrlatch, 0) == 1); // S
  assert(logic::seq_input_row(GateKind::kSrlatch, 1) == 4); // R
  assert(logic::seq_input_row(GateKind::kAnd, 0) == -1);    // not sequential
  // Clock-wedge pins (edge flops only) and latch-enable notch pin.
  assert(logic::is_clock_pin(GateKind::kDff, 1));
  assert(logic::is_clock_pin(GateKind::kJk, 2));
  assert(logic::is_clock_pin(GateKind::kT, 1));
  assert(!logic::is_clock_pin(GateKind::kDff, 0));
  assert(!logic::is_clock_pin(GateKind::kDlatch, 1));
  assert(!logic::is_clock_pin(GateKind::kSrlatch, 0));
  assert(!logic::is_clock_pin(GateKind::kAnd, 0));
  assert(logic::is_latch_enable_pin(GateKind::kDlatch, 1));
  assert(!logic::is_latch_enable_pin(GateKind::kDff, 4));
  // Face letters.
  assert(logic::seq_face(GateKind::kDff) == "D");
  assert(logic::seq_face(GateKind::kJk) == "JK");
  assert(logic::seq_face(GateKind::kT) == "T");
  assert(logic::seq_face(GateKind::kSrlatch) == "SR");
  assert(logic::seq_face(GateKind::kDlatch) == "DL");

  Netlist nl;
  assert(logic::add_gate(nl, GateKind::kDff, 2).ok());     // @0 bare DFF
  assert(logic::add_gate(nl, GateKind::kDff, 5).ok());     // @1 full DFF
  assert(logic::add_gate(nl, GateKind::kDlatch, 2).ok());  // @2
  assert(logic::add_gate(nl, GateKind::kJk, 3).ok());      // @3
  assert(logic::add_gate(nl, GateKind::kT, 2).ok());       // @4
  assert(logic::add_gate(nl, GateKind::kSrlatch, 2).ok()); // @5
  const logic::Schematic sc = logic::layout_schematic(nl);
  assert(sc.gates.size() == 6);
  assert(sc.wires.empty());
  // Unwired gates stack at level 0 in slot order: y = 30 + k*86, x = 52.
  // I-CLK: bare and full-control DFF share D/CLK anchors.
  const logic::WirePt d_bare = logic::seq_input_anchor(sc.gates[0], GateKind::kDff, 0);
  const logic::WirePt c_bare = logic::seq_input_anchor(sc.gates[0], GateKind::kDff, 1);
  const logic::WirePt d_full = logic::seq_input_anchor(sc.gates[1], GateKind::kDff, 0);
  const logic::WirePt c_full = logic::seq_input_anchor(sc.gates[1], GateKind::kDff, 1);
  assert(d_bare.x == 52 && d_bare.y == 30 + 9);
  assert(c_bare.x == 52 && c_bare.y == 30 + 18);
  assert(d_full.y - sc.gates[1].y == d_bare.y - sc.gates[0].y);
  assert(c_full.y - sc.gates[1].y == c_bare.y - sc.gates[0].y);
  // Full-control DFF: five pairwise-distinct anchors in priority order.
  const int full_y = sc.gates[1].y;
  assert(logic::seq_input_anchor(sc.gates[1], GateKind::kDff, 2).y == full_y + 28);
  assert(logic::seq_input_anchor(sc.gates[1], GateKind::kDff, 3).y == full_y + 37);
  assert(logic::seq_input_anchor(sc.gates[1], GateKind::kDff, 4).y == full_y + 46);
  // JK: J top, CLK clock row, K below.
  const int jk_y = sc.gates[3].y;
  assert(logic::seq_input_anchor(sc.gates[3], GateKind::kJk, 0).y == jk_y + 9);
  assert(logic::seq_input_anchor(sc.gates[3], GateKind::kJk, 2).y == jk_y + 18);
  assert(logic::seq_input_anchor(sc.gates[3], GateKind::kJk, 1).y == jk_y + 37);
  // SR latch: S top, R bottom.
  const int sr_y = sc.gates[5].y;
  assert(logic::seq_input_anchor(sc.gates[5], GateKind::kSrlatch, 0).y == sr_y + 9);
  assert(logic::seq_input_anchor(sc.gates[5], GateKind::kSrlatch, 1).y == sr_y + 37);
  // Q anchor keeps the mid convention; NQ anchor sits at row 5.
  const logic::WirePt q = logic::seq_q_anchor(sc.gates[0]);
  const logic::WirePt nq = logic::seq_nq_anchor(sc.gates[0]);
  assert(q.x == 52 + 76 && q.y == 30 + 28);
  assert(nq.x == 52 + 76 && nq.y == 30 + 46);
}

void test_shared_clock_routes() {
  // BUF1.Y -> DFF1.CLK + DFF2.CLK: both clock wires land on the wedge
  // anchor (row 2/6) of their flop.
  Netlist nl;
  assert(logic::add_gate(nl, GateKind::kBuf, 1).ok()); // @0
  assert(logic::add_gate(nl, GateKind::kDff, 2).ok()); // @1
  assert(logic::add_gate(nl, GateKind::kDff, 2).ok()); // @2
  assert(logic::wire(nl, 0, 1, 1).ok());
  assert(logic::wire(nl, 0, 2, 1).ok());
  const logic::Schematic sc = logic::layout_schematic(nl);
  assert(sc.levels == 2);
  assert(sc.wires.size() == 2);
  // Level 1 stacking: DFF1 y=30, DFF2 y=116; CLK row offset 18.
  assert(sc.wires[0].to_slot == 1 && sc.wires[0].to_input == 1);
  assert(sc.wires[0].p[3].x == 172 && sc.wires[0].p[3].y == 48);
  assert(sc.wires[1].to_slot == 2 && sc.wires[1].to_input == 1);
  assert(sc.wires[1].p[3].x == 172 && sc.wires[1].p[3].y == 134);
  // Sources leave the Q anchor (BUF mid: y=30+28=58, x0=128).
  assert(sc.wires[0].p[0].x == 128 && sc.wires[0].p[0].y == 58);
  assert(sc.wires[1].p[0].x == 128 && sc.wires[1].p[0].y == 58);
  // Route endpoints equal the anchor queries (single source of truth).
  const logic::WirePt c1 = logic::seq_input_anchor(sc.gates[1], GateKind::kDff, 1);
  const logic::WirePt c2 = logic::seq_input_anchor(sc.gates[2], GateKind::kDff, 1);
  assert(c1.x == 172 && c1.y == 48);
  assert(c2.x == 172 && c2.y == 134);
  assert(sc.wires[0].p[3].x == c1.x && sc.wires[0].p[3].y == c1.y);
  assert(sc.wires[1].p[3].x == c2.x && sc.wires[1].p[3].y == c2.y);
}

void test_nq_source_route() {
  // DFF1.NQ -> NOT1.1: the wire starts at the NQ anchor (row 5/6).
  Netlist nl;
  assert(logic::add_gate(nl, GateKind::kDff, 2).ok()); // @0
  assert(logic::add_gate(nl, GateKind::kNot, 1).ok()); // @1
  const logic::PinRef from{0, true, 0, true};          // DFF1.NQ
  const logic::PinRef to{1, false, 0, false};          // NOT1.1
  assert(logic::wire_pins(nl, from, to).ok());
  const logic::Schematic sc = logic::layout_schematic(nl);
  assert(sc.wires.size() == 1);
  const logic::WireRoute &r = sc.wires[0];
  assert(r.from_slot == 0 && r.to_slot == 1 && r.to_input == 0);
  assert(r.p[0].x == 128 && r.p[0].y == 30 + 46);
  assert(r.p[3].x == 172 && r.p[3].y == 30 + 28);
  const logic::WirePt nq = logic::seq_nq_anchor(sc.gates[0]);
  assert(nq.x == r.p[0].x && nq.y == r.p[0].y);
}

void test_dangling_clock() {
  // Undriven CLK keeps its anchor and wedge position with no wire.
  Netlist nl;
  assert(logic::add_gate(nl, GateKind::kDff, 2).ok());
  const logic::Schematic sc = logic::layout_schematic(nl);
  assert(sc.gates.size() == 1);
  assert(sc.wires.empty());
  const logic::WirePt c = logic::seq_input_anchor(sc.gates[0], GateKind::kDff, 1);
  assert(c.x == 52 && c.y == 30 + 18);
}

void test_seq_deterministic() {
  Netlist nl;
  assert(logic::add_gate(nl, GateKind::kBuf, 1).ok());
  assert(logic::add_gate(nl, GateKind::kDff, 2).ok());
  assert(logic::add_gate(nl, GateKind::kJk, 3).ok());
  assert(logic::wire(nl, 0, 1, 1).ok());
  assert(logic::wire(nl, 0, 2, 2).ok());
  const logic::Schematic once = logic::layout_schematic(nl);
  const logic::Schematic twice = logic::layout_schematic(nl);
  assert(same_schematic(once, twice));
  Netlist twin;
  assert(logic::add_gate(twin, GateKind::kBuf, 1).ok());
  assert(logic::add_gate(twin, GateKind::kDff, 2).ok());
  assert(logic::add_gate(twin, GateKind::kJk, 3).ok());
  assert(logic::wire(twin, 0, 1, 1).ok());
  assert(logic::wire(twin, 0, 2, 2).ok());
  assert(same_schematic(once, logic::layout_schematic(twin)));
}

void test_unrelated_trunks_stagger() {
  // Screenshot-net regression: T1.Q -> AND1.1 and SRLATCH1.Q -> AND1.2
  // are unrelated nets spanning the same column gap. Before lanes both
  // verticals sat at xm = 150 (one unreadable trunk); now they stagger
  // while endpoints stay pinned on their anchors.
  Netlist nl;
  assert(logic::add_gate(nl, GateKind::kDff, 2).ok());     // @0
  assert(logic::add_gate(nl, GateKind::kJk, 3).ok());      // @1
  assert(logic::add_gate(nl, GateKind::kT, 2).ok());       // @2
  assert(logic::add_gate(nl, GateKind::kSrlatch, 2).ok()); // @3
  assert(logic::add_gate(nl, GateKind::kAnd, 2).ok());     // @4
  assert(logic::wire(nl, 2, 4, 0).ok());                  // T1.Q -> AND1.1
  assert(logic::wire(nl, 3, 4, 1).ok());                  // SRLATCH1.Q -> AND1.2
  const logic::Schematic sc = logic::layout_schematic(nl);
  assert(sc.levels == 2);
  assert(sc.wires.size() == 2);
  // L0 stacking: T1 y = 30 + 2*86 = 202, SRLATCH1 y = 30 + 3*86 = 288.
  // AND1.1 spread y = 30 + 56/3 = 48; AND1.2 y = 30 + 2*56/3 = 67.
  const logic::WireRoute &a = sc.wires[0];
  const logic::WireRoute &b = sc.wires[1];
  assert(a.from_slot == 2 && a.to_input == 0);
  assert(a.p[0].x == 128 && a.p[0].y == 230);
  assert(a.p[1].x == 150 && a.p[1].y == 230);
  assert(a.p[2].x == 150 && a.p[2].y == 48);
  assert(a.p[3].x == 172 && a.p[3].y == 48);
  assert(b.from_slot == 3 && b.to_input == 1);
  assert(b.p[0].x == 128 && b.p[0].y == 316);
  assert(b.p[1].x == 158 && b.p[1].y == 316);
  assert(b.p[2].x == 158 && b.p[2].y == 67);
  assert(b.p[3].x == 172 && b.p[3].y == 67);
}

void test_seq_legibility() {
  // 4-flop chain with a shared clock driver, fanout, and NQ feedback to
  // a sink: no two symbol boxes overlap, and per-output fanout is
  // accounted for the junction-dot rule (Q dots iff output_fanout > 1,
  // NQ dots iff nq_fanout > 1).
  Netlist nl;
  assert(logic::add_gate(nl, GateKind::kBuf, 1).ok()); // @0 clock driver
  assert(logic::add_gate(nl, GateKind::kDff, 2).ok()); // @1
  assert(logic::add_gate(nl, GateKind::kDff, 2).ok()); // @2
  assert(logic::add_gate(nl, GateKind::kDff, 2).ok()); // @3
  assert(logic::add_gate(nl, GateKind::kDff, 2).ok()); // @4
  assert(logic::add_gate(nl, GateKind::kNot, 1).ok()); // @5 NQ sink
  assert(logic::wire(nl, 0, 1, 0).ok());              // BUF1.Y -> DFF1.D
  assert(logic::wire(nl, 0, 1, 1).ok());              // BUF1.Y -> DFF1.CLK
  assert(logic::wire(nl, 0, 2, 1).ok());
  assert(logic::wire(nl, 0, 3, 1).ok());
  assert(logic::wire(nl, 0, 4, 1).ok());
  assert(logic::wire(nl, 1, 2, 0).ok()); // chain Q -> D
  assert(logic::wire(nl, 2, 3, 0).ok());
  assert(logic::wire(nl, 3, 4, 0).ok());
  const logic::PinRef nq_from{4, true, 0, true};
  const logic::PinRef nq_to{5, false, 0, false};
  assert(logic::wire_pins(nl, nq_from, nq_to).ok()); // DFF4.NQ -> NOT1.1
  const logic::Schematic sc = logic::layout_schematic(nl);
  assert(sc.gates.size() == 6);
  assert(sc.levels == 6);
  assert(sc.width == 60 + 6 * 120);
  // No two symbol boxes overlap (standard 76x56 boxes).
  for (std::size_t a = 0; a < sc.gates.size(); ++a) {
    for (std::size_t b = a + 1; b < sc.gates.size(); ++b) {
      const logic::Placed &x = sc.gates[a];
      const logic::Placed &y = sc.gates[b];
      const bool separated = (x.x + 76 <= y.x) || (y.x + 76 <= x.x) || (x.y + 56 <= y.y) ||
                             (y.y + 56 <= x.y);
      assert(separated);
    }
  }
  // Per-output fanout accounting behind the dot rule.
  assert(nl.gates[0].output_fanout == 5); // D + 4x CLK
  assert(nl.gates[0].nq_fanout == 0);
  assert(nl.gates[4].nq_fanout == 1);
  assert(nl.gates[4].output_fanout == 0); // DFF4.Q unwired: no Q dot
  assert(nl.gates[1].output_fanout == 1); // single consumer: no dot
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
  test_seq_anchor_rows();
  test_shared_clock_routes();
  test_nq_source_route();
  test_dangling_clock();
  test_seq_deterministic();
  test_seq_legibility();
  test_unrelated_trunks_stagger();
  std::cout << "test_logic_layout: OK\n";
  return 0;
}
