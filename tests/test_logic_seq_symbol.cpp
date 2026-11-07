// Shared sequential symbol model: SeqSym ints == anchor queries + NQ/dot
// predicates. Header-only (`wengine/logic/seq_symbol.hpp`), SDL/ImGui-free:
// links wengine_core alone and runs headless. Pins the paint Seam for both
// raster Adapters (one model per box) and the single-owner contract (every
// seq_* query reads the pins.hpp KindPin table).

#include "wengine/logic/layout.hpp"
#include "wengine/logic/netlist.hpp"
#include "wengine/logic/pins.hpp"
#include "wengine/logic/seq_symbol.hpp"
#include "wengine/logic/types.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>

using namespace wengine;
using wengine::logic::GateKind;
using wengine::logic::Netlist;

namespace {

void test_table_single_owner() {
  // Every seq_* adapter reads the same KindPin/KindInfo row it claims.
  for (const logic::KindInfo &e : logic::kKinds) {
    for (std::uint8_t p = 0; p < e.pin_count; ++p) {
      const std::uint8_t input = e.pins[p].input;
      assert(logic::seq_input_row(e.kind, input) == e.pins[p].anchor_row);
      assert(logic::is_clock_pin(e.kind, input) == e.pins[p].is_clock);
      assert(logic::is_latch_enable_pin(e.kind, input) == e.pins[p].is_notch);
    }
    if (logic::is_sequential_kind(e.kind)) {
      assert(logic::seq_face(e.kind) == e.face);
      assert(!e.face.empty());
    } else {
      assert(logic::seq_face(e.kind) == "?");
    }
  }
  assert(logic::seq_input_row(GateKind::kAnd, 0) == -1);
  assert(!logic::is_clock_pin(GateKind::kAnd, 0));
  assert(!logic::is_latch_enable_pin(GateKind::kDff, 4)); // EN is not the notch
}

void test_dff_symbol() {
  Netlist nl;
  assert(logic::add_gate(nl, GateKind::kDff, 5).ok()); // @0 full-control
  const logic::Placed box{0, 0, 52, 30};
  const logic::SeqSym sym = logic::seq_symbol(box, nl.gates[0]);
  assert(sym.lx == 52 + 10 && sym.rx == 52 + 76 - 2);
  assert(sym.top == 30 + 4 && sym.bot == 30 + 56 - 4 && sym.mid == 30 + 28);
  assert(sym.face == "D");
  assert(sym.mark == logic::SeqMark::kWedge);
  assert(sym.mark_y == 30 + 18); // CLK row 2/6
  assert(sym.stub_n == 5);
  for (std::uint8_t i = 0; i < 5; ++i) {
    assert(sym.stub_y[i] == logic::seq_input_anchor(box, GateKind::kDff, i).y);
  }
  assert(sym.q.x == 52 + 76 && sym.q.y == 30 + 28);
  assert(sym.nq_pt.x == 52 + 76 && sym.nq_pt.y == 30 + 46);
  assert(sym.nq == ((nl.gates[0].nq_fanout > 0) || (!nl.gates[0].ignore_nq_output)));
  assert(!sym.q_dot && !sym.nq_dot); // fresh gate: no fanout
}

void test_mark_vocabulary() {
  Netlist nl;
  assert(logic::add_gate(nl, GateKind::kDlatch, 2).ok());  // @0
  assert(logic::add_gate(nl, GateKind::kJk, 3).ok());      // @1
  assert(logic::add_gate(nl, GateKind::kSrlatch, 2).ok()); // @2
  assert(logic::add_gate(nl, GateKind::kT, 2).ok());       // @3
  const logic::Placed b0{0, 0, 52, 30};
  const logic::Placed b1{1, 0, 52, 116};
  const logic::Placed b2{2, 0, 52, 202};
  const logic::Placed b3{3, 0, 52, 288};
  const logic::SeqSym dl = logic::seq_symbol(b0, nl.gates[0]);
  const logic::SeqSym jk = logic::seq_symbol(b1, nl.gates[1]);
  const logic::SeqSym sr = logic::seq_symbol(b2, nl.gates[2]);
  const logic::SeqSym t = logic::seq_symbol(b3, nl.gates[3]);
  assert(dl.face == "DL" && dl.mark == logic::SeqMark::kNotch && dl.mark_y == 30 + 18);
  assert(jk.face == "JK" && jk.mark == logic::SeqMark::kWedge && jk.mark_y == 116 + 18);
  assert(sr.face == "SR" && sr.mark == logic::SeqMark::kNone);
  assert(t.face == "T" && t.mark == logic::SeqMark::kWedge && t.mark_y == 288 + 18);
}

void test_dots_from_model() {
  Netlist nl;
  assert(logic::add_gate(nl, GateKind::kBuf, 1).ok()); // @0 driver
  assert(logic::add_gate(nl, GateKind::kDff, 2).ok()); // @1
  assert(logic::add_gate(nl, GateKind::kDff, 2).ok()); // @2
  assert(logic::wire(nl, 0, 1, 1).ok());
  assert(logic::wire(nl, 0, 2, 1).ok());
  const logic::Placed box{0, 0, 52, 30};
  const logic::SeqSym sym = logic::seq_symbol(box, nl.gates[0]);
  assert(sym.q_dot);   // output_fanout == 2
  assert(!sym.nq_dot); // combinational: never
  assert(sym.mark == logic::SeqMark::kNone && sym.face == "?");
}

} // namespace

int main() {
  test_table_single_owner();
  test_dff_symbol();
  test_mark_vocabulary();
  test_dots_from_model();
  std::cout << "test_logic_seq_symbol: OK\n";
  return 0;
}
