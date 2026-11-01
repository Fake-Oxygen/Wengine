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
  static_assert(static_cast<std::uint8_t>(GateKind::kJk) == 18);
  static_assert(static_cast<std::uint8_t>(GateKind::kT) == 19);
  assert(logic::kind_ordinal(GateKind::kDff) == 16);
  assert(logic::kind_ordinal(GateKind::kDlatch) == 17);
  assert(logic::kind_ordinal(GateKind::kJk) == 18);
  assert(logic::kind_ordinal(GateKind::kT) == 19);
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
  assert(logic::gate_kind_name(GateKind::kJk) == "JK");
  assert(logic::gate_kind_name(GateKind::kT) == "T");
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
  // JK: case-insensitive exact match.
  assert(logic::parse_gate_kind("JK").value == GateKind::kJk);
  assert(logic::parse_gate_kind("jk").value == GateKind::kJk);
  assert(logic::parse_gate_kind("Jk").value == GateKind::kJk);
  // T: case-insensitive exact match (single letter, no collisions).
  assert(logic::parse_gate_kind("T").value == GateKind::kT);
  assert(logic::parse_gate_kind("t").value == GateKind::kT);
  // Near-misses stay loud.
  assert(!logic::parse_gate_kind("J").ok());
  assert(!logic::parse_gate_kind("K").ok());
  assert(!logic::parse_gate_kind("JK1").ok());
  assert(!logic::parse_gate_kind("JK ").ok());
  assert(!logic::parse_gate_kind(" JK").ok());
  assert(!logic::parse_gate_kind("T1").ok());
  assert(!logic::parse_gate_kind("TT").ok());
  assert(!logic::parse_gate_kind("T ").ok());
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
      GateKind::kDff, GateKind::kDlatch, GateKind::kJk, GateKind::kT,
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
  // JK: J+K+CLK, exactly 3 (default 3). T: T+CLK, exactly 2 (default 2).
  assert(logic::valid_fanin(GateKind::kJk, 3));
  assert(!logic::valid_fanin(GateKind::kJk, 0));
  assert(!logic::valid_fanin(GateKind::kJk, 2));
  assert(!logic::valid_fanin(GateKind::kJk, 4));
  assert(!logic::valid_fanin(GateKind::kJk, 8));
  assert(logic::default_fanin(GateKind::kJk) == 3);
  assert(logic::valid_fanin(GateKind::kT, 2));
  assert(!logic::valid_fanin(GateKind::kT, 0));
  assert(!logic::valid_fanin(GateKind::kT, 1));
  assert(!logic::valid_fanin(GateKind::kT, 3));
  assert(!logic::valid_fanin(GateKind::kT, 8));
  assert(logic::default_fanin(GateKind::kT) == 2);
  // Both new kinds are sequential (share the stepper/state pool).
  assert(logic::is_sequential_kind(GateKind::kJk));
  assert(logic::is_sequential_kind(GateKind::kT));
  assert(!logic::is_sequential_kind(GateKind::kAnd));
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

void test_creation() {
  // Sequential creation path (T003): DFF/DLATCH take exactly 2 inputs,
  // with per-kind never-reused counters (DFF1, DFF2, ...).
  Netlist nl;
  auto d1 = logic::add_gate(nl, GateKind::kDff, 2);
  assert(d1.ok());
  assert(nl.gates[d1.value].counter == 1);
  assert(nl.gates[d1.value].input_count == 2);
  auto l1 = logic::add_gate(nl, GateKind::kDlatch); // default fan-in is 2
  assert(l1.ok());
  assert(nl.gates[l1.value].counter == 1);
  assert(nl.live_count == 2);
  // Wrong fan-in stays loud, state untouched.
  assert(!logic::add_gate(nl, GateKind::kDff, 1).ok());
  assert(!logic::add_gate(nl, GateKind::kDff, 3).ok());
  assert(!logic::add_gate(nl, GateKind::kDlatch, 8).ok());
  assert(nl.live_count == 2);
  // Names resolve (case-insensitive kind).
  assert(logic::resolve_gate(nl, "DFF1").value == d1.value);
  assert(logic::resolve_gate(nl, "dlatch1").value == l1.value);
  assert(!logic::resolve_gate(nl, "DFF2").ok());
  // JK/T creation: fixed fan-in with per-kind never-reused counters.
  auto j1 = logic::add_gate(nl, GateKind::kJk, 3);
  assert(j1.ok());
  assert(nl.gates[j1.value].counter == 1);
  assert(nl.gates[j1.value].input_count == 3);
  auto t1 = logic::add_gate(nl, GateKind::kT); // default fan-in is 2
  assert(t1.ok());
  assert(nl.gates[t1.value].counter == 1);
  assert(nl.gates[t1.value].input_count == 2);
  assert(logic::resolve_gate(nl, "JK1").value == j1.value);
  assert(logic::resolve_gate(nl, "t1").value == t1.value);
  assert(!logic::resolve_gate(nl, "JK2").ok());
  assert(!logic::resolve_gate(nl, "T2").ok());
  // Wrong fan-in stays loud, state untouched.
  assert(!logic::add_gate(nl, GateKind::kJk, 2).ok());
  assert(!logic::add_gate(nl, GateKind::kJk, 5).ok());
  assert(!logic::add_gate(nl, GateKind::kT, 1).ok());
  assert(!logic::add_gate(nl, GateKind::kT, 3).ok());
  assert(nl.live_count == 4);
  // Counters never reused: remove DFF1, the next DFF is DFF2.
  assert(logic::remove_gate(nl, d1.value).ok());
  auto d2 = logic::add_gate(nl, GateKind::kDff, 2);
  assert(d2.ok());
  assert(nl.gates[d2.value].counter == 2);
  assert(logic::resolve_gate(nl, "DFF2").value == d2.value);
  // Combinational creation unchanged.
  auto a = logic::add_gate(nl, GateKind::kAnd, 2);
  assert(a.ok());
  assert(nl.gates[a.value].counter == 1);
}

void test_jk_t_pins() {
  // Named pins resolve to fixed indices; Q/NQ read outputs; wrong-kind
  // names and out-of-range numerics stay loud.
  Netlist nl;
  auto jk = logic::add_gate(nl, GateKind::kJk, 3);
  assert(jk.ok());
  auto t = logic::add_gate(nl, GateKind::kT, 2);
  assert(t.ok());
  auto d = logic::add_gate(nl, GateKind::kDff, 2);
  assert(d.ok());
  // JK rows (case-insensitive).
  auto j = logic::parse_pin(nl, "JK1.J");
  assert(j.ok() && !j.value.is_output && j.value.input == 0);
  auto k = logic::parse_pin(nl, "jk1.k");
  assert(k.ok() && !k.value.is_output && k.value.input == 1);
  auto clk = logic::parse_pin(nl, "JK1.CLK");
  assert(clk.ok() && !clk.value.is_output && clk.value.input == 2);
  auto q = logic::parse_pin(nl, "JK1.Q");
  assert(q.ok() && q.value.is_output && !q.value.negated);
  auto nq = logic::parse_pin(nl, "JK1.NQ");
  assert(nq.ok() && nq.value.is_output && nq.value.negated);
  // T rows (case-insensitive).
  auto tv = logic::parse_pin(nl, "T1.T");
  assert(tv.ok() && !tv.value.is_output && tv.value.input == 0);
  auto tclk = logic::parse_pin(nl, "t1.clk");
  assert(tclk.ok() && !tclk.value.is_output && tclk.value.input == 1);
  auto tq = logic::parse_pin(nl, "T1.Q");
  assert(tq.ok() && tq.value.is_output && !tq.value.negated);
  auto tnq = logic::parse_pin(nl, "T1.NQ");
  assert(tnq.ok() && tnq.value.is_output && tnq.value.negated);
  // Numeric aliases keep working; range validates against fan-in.
  assert(logic::parse_pin(nl, "JK1.1").value.input == 0);
  assert(logic::parse_pin(nl, "JK1.3").value.input == 2);
  assert(!logic::parse_pin(nl, "JK1.4").ok());
  assert(logic::parse_pin(nl, "T1.2").value.input == 1);
  assert(!logic::parse_pin(nl, "T1.3").ok());
  // Kind-foreign names rejected (fall through to the numeric parser).
  assert(!logic::parse_pin(nl, "JK1.D").ok());
  assert(!logic::parse_pin(nl, "JK1.T").ok());
  assert(!logic::parse_pin(nl, "JK1.RST").ok());
  assert(!logic::parse_pin(nl, "T1.D").ok());
  assert(!logic::parse_pin(nl, "T1.J").ok());
  assert(!logic::parse_pin(nl, "T1.EN").ok());
  assert(!logic::parse_pin(nl, "DFF1.J").ok());
  assert(!logic::parse_pin(nl, "DFF1.T").ok());
  // DFF/DLATCH rows unchanged.
  assert(logic::parse_pin(nl, "DFF1.D").value.input == 0);
  assert(logic::parse_pin(nl, "DFF1.CLK").value.input == 1);
}

} // namespace

int main() {
  test_ordinal_values();
  test_names();
  test_parse_sequential();
  test_round_trip();
  test_fanin();
  test_creation();
  test_jk_t_pins();
  std::cout << "test_logic_seq_kinds: OK\n";
  return 0;
}
