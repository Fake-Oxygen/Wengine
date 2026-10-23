#include "wengine/logic/netlist.hpp"
#include "wengine/logic/types.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <string_view>

using namespace wengine;
using wengine::logic::GateKind;
using wengine::logic::Netlist;

namespace {

// Append-only bytes: never renumber, never reuse (docs/logic.md §12).
void test_ordinal_values() {
  static_assert(static_cast<std::uint8_t>(GateKind::kAnd) == 0);
  static_assert(static_cast<std::uint8_t>(GateKind::kOr) == 1);
  static_assert(static_cast<std::uint8_t>(GateKind::kNand) == 2);
  static_assert(static_cast<std::uint8_t>(GateKind::kNor) == 3);
  static_assert(static_cast<std::uint8_t>(GateKind::kXor) == 4);
  static_assert(static_cast<std::uint8_t>(GateKind::kXnor) == 5);
  static_assert(static_cast<std::uint8_t>(GateKind::kNot) == 6);
  static_assert(static_cast<std::uint8_t>(GateKind::kBuf) == 7);
  static_assert(static_cast<std::uint8_t>(GateKind::kDff) == 16);
  static_assert(static_cast<std::uint8_t>(GateKind::kDlatch) == 17);
  assert(logic::kind_ordinal(GateKind::kDff) == 16);
  assert(logic::kind_ordinal(GateKind::kDlatch) == 17);
  assert(logic::kind_ordinal(GateKind::kAnd) == 0);
  assert(logic::kind_ordinal(GateKind::kBuf) == 7);
}

void test_names() {
  assert(logic::gate_kind_name(GateKind::kAnd) == "AND");
  assert(logic::gate_kind_name(GateKind::kOr) == "OR");
  assert(logic::gate_kind_name(GateKind::kNand) == "NAND");
  assert(logic::gate_kind_name(GateKind::kNor) == "NOR");
  assert(logic::gate_kind_name(GateKind::kXor) == "XOR");
  assert(logic::gate_kind_name(GateKind::kXnor) == "XNOR");
  assert(logic::gate_kind_name(GateKind::kNot) == "NOT");
  assert(logic::gate_kind_name(GateKind::kBuf) == "BUF");
  assert(logic::gate_kind_name(GateKind::kDff) == "DFF");
  assert(logic::gate_kind_name(GateKind::kDlatch) == "DLATCH");
  assert(logic::gate_kind_name(static_cast<GateKind>(99)) == "?");
}

void test_parse_sequential() {
  // DFF: case-insensitive exact match.
  assert(logic::parse_gate_kind("DFF").ok());
  assert(logic::parse_gate_kind("DFF").value == GateKind::kDff);
  assert(logic::parse_gate_kind("dff").value == GateKind::kDff);
  assert(logic::parse_gate_kind("Dff").value == GateKind::kDff);
  assert(logic::parse_gate_kind("dFf").value == GateKind::kDff);
  // DLATCH: case-insensitive exact match.
  assert(logic::parse_gate_kind("DLATCH").ok());
  assert(logic::parse_gate_kind("DLATCH").value == GateKind::kDlatch);
  assert(logic::parse_gate_kind("dlatch").value == GateKind::kDlatch);
  assert(logic::parse_gate_kind("Dlatch").value == GateKind::kDlatch);
  assert(logic::parse_gate_kind("dLatch").value == GateKind::kDlatch);
  // Near-misses stay loud.
  assert(!logic::parse_gate_kind("").ok());
  assert(!logic::parse_gate_kind("D").ok());
  assert(!logic::parse_gate_kind("DF").ok());
  assert(!logic::parse_gate_kind("DFF2").ok());
  assert(!logic::parse_gate_kind("DLATCH1").ok());
  assert(!logic::parse_gate_kind("FF").ok());
  assert(!logic::parse_gate_kind("LATCH").ok());
  assert(!logic::parse_gate_kind("LATCHD").ok());
  assert(!logic::parse_gate_kind(" DFF").ok());
  assert(!logic::parse_gate_kind("DFF ").ok());
  assert(!logic::parse_gate_kind("D FF").ok());
  // Combinational parsing unchanged.
  assert(logic::parse_gate_kind("AND").value == GateKind::kAnd);
  assert(logic::parse_gate_kind("and").value == GateKind::kAnd);
  assert(logic::parse_gate_kind("Xnor").value == GateKind::kXnor);
  assert(logic::parse_gate_kind("buf").value == GateKind::kBuf);
  assert(!logic::parse_gate_kind("NAND2").ok());
  assert(!logic::parse_gate_kind("ANDD").ok());
}

void test_round_trip() {
  constexpr GateKind kAll[] = {
      GateKind::kAnd, GateKind::kOr, GateKind::kNand, GateKind::kNor,
      GateKind::kXor, GateKind::kXnor, GateKind::kNot, GateKind::kBuf,
      GateKind::kDff, GateKind::kDlatch,
  };
  for (GateKind k : kAll) {
    const std::string_view name = logic::gate_kind_name(k);
    const auto back = logic::parse_gate_kind(name);
    assert(back.ok() && back.value == k);
  }
}

void test_fanin() {
  // DFF: D+CLK, exactly 2. DLATCH: D+EN, exactly 2.
  assert(logic::valid_fanin(GateKind::kDff, 2));
  assert(!logic::valid_fanin(GateKind::kDff, 0));
  assert(!logic::valid_fanin(GateKind::kDff, 1));
  assert(!logic::valid_fanin(GateKind::kDff, 3));
  assert(!logic::valid_fanin(GateKind::kDff, 8));
  assert(logic::valid_fanin(GateKind::kDlatch, 2));
  assert(!logic::valid_fanin(GateKind::kDlatch, 0));
  assert(!logic::valid_fanin(GateKind::kDlatch, 1));
  assert(!logic::valid_fanin(GateKind::kDlatch, 3));
  assert(!logic::valid_fanin(GateKind::kDlatch, 8));
  assert(logic::default_fanin(GateKind::kDff) == 2);
  assert(logic::default_fanin(GateKind::kDlatch) == 2);
  // Combinational rules unchanged.
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
}

void test_no_state_change() {
  // SEQ1 is vocabulary only: creation of sequential gates stays loud and
  // leaves the Netlist untouched (SEQ2 adds the state pool + creation).
  Netlist nl;
  assert(!logic::add_gate(nl, GateKind::kDff, 2).ok());
  assert(!logic::add_gate(nl, GateKind::kDlatch, 2).ok());
  assert(!logic::add_gate(nl, GateKind::kDff).ok());
  assert(nl.live_count == 0);
  // Name resolution on an empty workspace fails loudly (no crash on the
  // >=16 ordinals), and combinational creation still works.
  assert(!logic::resolve_gate(nl, "DFF1").ok());
  assert(!logic::resolve_gate(nl, "DLATCH1").ok());
  auto a = logic::add_gate(nl, GateKind::kAnd, 2);
  assert(a.ok());
  assert(nl.live_count == 1);
  assert(logic::resolve_gate(nl, "AND1").ok());
  assert(!logic::resolve_gate(nl, "DFF1").ok());
}

} // namespace

int main() {
  test_ordinal_values();
  test_names();
  test_parse_sequential();
  test_round_trip();
  test_fanin();
  test_no_state_change();
  std::cout << "test_logic_seq_kinds: OK\n";
  return 0;
}
