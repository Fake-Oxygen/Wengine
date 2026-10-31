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

constexpr LogicValue kL = LogicValue::kLow;
constexpr LogicValue kH = LogicValue::kHigh;

void ignore_input(Netlist &nl, std::uint16_t slot, std::uint8_t input) {
  logic::PinRef pin{};
  pin.slot = slot;
  pin.is_output = false;
  pin.input = input;
  assert(logic::ignore_pin(nl, pin).ok());
}

void use_input(Netlist &nl, std::uint16_t slot, std::uint8_t input) {
  logic::PinRef pin{};
  pin.slot = slot;
  pin.is_output = false;
  pin.input = input;
  assert(logic::use_pin(nl, pin).ok());
  assert((nl.gates[slot].ignored_mask & static_cast<std::uint8_t>(1u << input)) == 0);
}

void test_fanin_control() {
  // DFF: bare D+CLK (2, default) or full control set (5, opt-in).
  assert(logic::valid_fanin(GateKind::kDff, 2));
  assert(logic::valid_fanin(GateKind::kDff, 5));
  assert(!logic::valid_fanin(GateKind::kDff, 0));
  assert(!logic::valid_fanin(GateKind::kDff, 1));
  assert(!logic::valid_fanin(GateKind::kDff, 3));
  assert(!logic::valid_fanin(GateKind::kDff, 4));
  assert(!logic::valid_fanin(GateKind::kDff, 6));
  assert(!logic::valid_fanin(GateKind::kDff, 8));
  assert(logic::default_fanin(GateKind::kDff) == 2);
  // DLATCH unchanged: exactly 2.
  assert(logic::valid_fanin(GateKind::kDlatch, 2));
  assert(!logic::valid_fanin(GateKind::kDlatch, 5));
  assert(logic::default_fanin(GateKind::kDlatch) == 2);
  Netlist nl;
  auto bare = logic::add_gate(nl, GateKind::kDff);
  assert(bare.ok());
  assert(nl.gates[bare.value].input_count == 2);
  auto full = logic::add_gate(nl, GateKind::kDff, 5);
  assert(full.ok());
  assert(nl.gates[full.value].input_count == 5);
  assert(!logic::add_gate(nl, GateKind::kDff, 3).ok());
  assert(!logic::add_gate(nl, GateKind::kDff, 4).ok());
}

void test_parse_control_pins() {
  Netlist nl;
  const std::uint16_t f5 = logic::add_gate(nl, GateKind::kDff, 5).value; // DFF1
  const std::uint16_t f2 = logic::add_gate(nl, GateKind::kDff).value;    // DFF2 (bare)
  const std::uint16_t l = logic::add_gate(nl, GateKind::kDlatch).value;  // DLATCH1
  // Full-control flop: named + numeric + case-insensitive.
  auto rst = logic::parse_pin(nl, "DFF1.RST");
  assert(rst.ok() && rst.value.slot == f5 && rst.value.input == 2);
  auto set = logic::parse_pin(nl, "DFF1.SET");
  assert(set.ok() && set.value.slot == f5 && set.value.input == 3);
  auto en = logic::parse_pin(nl, "DFF1.EN");
  assert(en.ok() && en.value.slot == f5 && en.value.input == 4);
  auto rst_lo = logic::parse_pin(nl, "dff1.rst");
  assert(rst_lo.ok() && rst_lo.value.input == 2);
  auto n3 = logic::parse_pin(nl, "DFF1.3");
  assert(n3.ok() && n3.value.input == 2);
  auto n4 = logic::parse_pin(nl, "DFF1.4");
  assert(n4.ok() && n4.value.input == 3);
  auto n5 = logic::parse_pin(nl, "DFF1.5");
  assert(n5.ok() && n5.value.input == 4);
  // Bare flop rejects control names exactly like out-of-range numerics.
  assert(!logic::parse_pin(nl, "DFF2.RST").ok());
  assert(!logic::parse_pin(nl, "DFF2.SET").ok());
  assert(!logic::parse_pin(nl, "DFF2.EN").ok());
  assert(!logic::parse_pin(nl, "DFF2.3").ok());
  assert(nl.gates[f2].input_count == 2); // bare default, controls absent
  // Re-expose helper coverage: ignore then use a control pin.
  ignore_input(nl, f5, 2);
  use_input(nl, f5, 2);
  // DLATCH vocabulary untouched (EN stays input 1, no RST/SET).
  auto len = logic::parse_pin(nl, "DLATCH1.EN");
  assert(len.ok() && len.value.slot == l && len.value.input == 1);
  assert(!logic::parse_pin(nl, "DLATCH1.RST").ok());
  assert(!logic::parse_pin(nl, "DLATCH1.SET").ok());
}

struct Row {
  LogicValue q_old;
  LogicValue rst;
  LogicValue set;
  LogicValue en;
  LogicValue d;
  LogicValue want;
};

// Independent reference: hand-derived from the research.md R3 priority
// table (RST > SET > EN-hold > D; reset wins; dangling EN enabled).
// Field order: q_old, rst, set, en, d -> want.
constexpr Row kRows[] = {
    // q_old = 0
    {kL, kL, kL, kL, kL, kL}, // idle
    {kL, kL, kL, kL, kH, kL}, // hold: D ignored
    {kL, kL, kL, kH, kL, kL}, // capture 0
    {kL, kL, kL, kH, kH, kH}, // capture 1
    {kL, kL, kH, kL, kL, kH}, // set (EN irrelevant)
    {kL, kL, kH, kL, kH, kH}, // set overrides hold
    {kL, kL, kH, kH, kL, kH}, // set overrides capture-0
    {kL, kL, kH, kH, kH, kH}, // set
    {kL, kH, kL, kL, kL, kL}, // reset (already 0)
    {kL, kH, kL, kL, kH, kL}, // reset overrides hold
    {kL, kH, kL, kH, kL, kL}, // reset overrides capture
    {kL, kH, kL, kH, kH, kL}, // reset
    {kL, kH, kH, kL, kL, kL}, // reset wins over set
    {kL, kH, kH, kL, kH, kL}, // reset wins over set
    {kL, kH, kH, kH, kL, kL}, // reset wins over set
    {kL, kH, kH, kH, kH, kL}, // reset wins over set
    // q_old = 1
    {kH, kL, kL, kL, kL, kH}, // hold
    {kH, kL, kL, kL, kH, kH}, // hold: D ignored
    {kH, kL, kL, kH, kL, kL}, // capture 0
    {kH, kL, kL, kH, kH, kH}, // capture 1
    {kH, kL, kH, kL, kL, kH}, // set (already 1)
    {kH, kL, kH, kL, kH, kH}, // set overrides hold
    {kH, kL, kH, kH, kL, kH}, // set overrides capture-0
    {kH, kL, kH, kH, kH, kH}, // set
    {kH, kH, kL, kL, kL, kL}, // reset forces 0
    {kH, kH, kL, kL, kH, kL}, // reset overrides hold
    {kH, kH, kL, kH, kL, kL}, // reset overrides capture
    {kH, kH, kL, kH, kH, kL}, // reset
    {kH, kH, kH, kL, kL, kL}, // reset wins over set
    {kH, kH, kH, kL, kH, kL}, // reset wins over set
    {kH, kH, kH, kH, kL, kL}, // reset wins over set
    {kH, kH, kH, kH, kH, kL}, // reset wins over set
};

void test_truth_table() {
  // One exposed 5-input flop; CLK ignored so the block vector reads
  // [D, RST, SET, EN] in pin order.
  for (const Row &r : kRows) {
    Netlist nl;
    const std::uint16_t f = logic::add_gate(nl, GateKind::kDff, 5).value;
    ignore_input(nl, f, 1); // CLK ignored (edge is the call)
    assert(logic::block_input_count(nl) == 4);
    SeqState st;
    logic::init_seq_state(st, nl);
    if (r.q_old == kH) {
      st.q[f] = kH;
    }
    const LogicValue in[4] = {r.d, r.rst, r.set, r.en};
    assert(logic::seq_step(nl, st, std::span<const LogicValue>(in, 4)).ok());
    assert(st.q[f] == r.want);
  }
}

void test_async_apply() {
  Netlist nl;
  const std::uint16_t f = logic::add_gate(nl, GateKind::kDff, 5).value;
  ignore_input(nl, f, 1); // CLK ignored
  SeqState st;
  // Reset wins over D with no clock advance.
  {
    const LogicValue in[4] = {kH, kH, kL, kL};
    assert(logic::apply_async_controls(nl, st, std::span<const LogicValue>(in, 4)).ok());
    assert(st.q[f] == kL);
  }
  // Set forces 1 with no clock advance.
  {
    const LogicValue in[4] = {kL, kL, kH, kL};
    assert(logic::apply_async_controls(nl, st, std::span<const LogicValue>(in, 4)).ok());
    assert(st.q[f] == kH);
  }
  // Quiet controls: Q untouched, D ignored by the async path.
  {
    const LogicValue in[4] = {kH, kL, kL, kL};
    assert(logic::apply_async_controls(nl, st, std::span<const LogicValue>(in, 4)).ok());
    assert(st.q[f] == kH);
  }
  // Simultaneous RST+SET: reset wins.
  {
    const LogicValue in[4] = {kL, kH, kH, kL};
    assert(logic::apply_async_controls(nl, st, std::span<const LogicValue>(in, 4)).ok());
    assert(st.q[f] == kL);
  }
  // Size mismatch fails loudly.
  {
    const LogicValue in[3] = {kL, kL, kL};
    assert(!logic::apply_async_controls(nl, st, std::span<const LogicValue>(in, 3)).ok());
  }
  // Nets without full-control flops: success no-op.
  {
    Netlist bare;
    const std::uint16_t b = logic::add_gate(bare, GateKind::kDff).value;
    ignore_input(bare, b, 0);
    ignore_input(bare, b, 1);
    SeqState bst;
    assert(logic::block_input_count(bare) == 0);
    assert(logic::apply_async_controls(bare, bst, std::span<const LogicValue>{}).ok());
  }
}

void test_hold() {
  Netlist nl;
  const std::uint16_t f = logic::add_gate(nl, GateKind::kDff, 5).value;
  ignore_input(nl, f, 1); // CLK ignored
  SeqState st;
  logic::init_seq_state(st, nl);
  // EN=0 across 3 edges with toggling D: Q frozen at 0.
  const LogicValue held[4] = {kH, kL, kL, kL};
  for (int i = 0; i < 3; ++i) {
    assert(logic::seq_step(nl, st, std::span<const LogicValue>(held, 4)).ok());
    assert(st.q[f] == kL);
  }
  // Release enable: D captures next edge.
  const LogicValue go[4] = {kH, kL, kL, kH};
  assert(logic::seq_step(nl, st, std::span<const LogicValue>(go, 4)).ok());
  assert(st.q[f] == kH);
  // Hold again at 1 with D=0: frozen, then capture on enable.
  const LogicValue hold1[4] = {kL, kL, kL, kL};
  assert(logic::seq_step(nl, st, std::span<const LogicValue>(hold1, 4)).ok());
  assert(st.q[f] == kH);
  const LogicValue cap0[4] = {kL, kL, kL, kH};
  assert(logic::seq_step(nl, st, std::span<const LogicValue>(cap0, 4)).ok());
  assert(st.q[f] == kL);
}

void test_zero_migration() {
  // Bare 2-input flops behave exactly as before (no EN pin → enabled).
  Netlist nl;
  const std::uint16_t f = logic::add_gate(nl, GateKind::kDff).value;
  assert(nl.gates[f].input_count == 2);
  ignore_input(nl, f, 1);
  assert(logic::block_input_count(nl) == 1);
  SeqState st;
  const LogicValue hi[1] = {kH};
  assert(logic::seq_step(nl, st, std::span<const LogicValue>(hi, 1)).ok());
  assert(st.q[f] == kH);
  const LogicValue lo[1] = {kL};
  assert(logic::seq_step(nl, st, std::span<const LogicValue>(lo, 1)).ok());
  assert(st.q[f] == kL);
}

void test_wired_rst() {
  // Wired async path: DFF1.Q drives DFF2.RST; DFF2.D follows DFF2.Q
  // through a BUF (feedback hold), so the (Q1=H, Q2=H) state is
  // reachable and the wired RST force H->L is observable with no step.
  Netlist nl;
  const std::uint16_t drv = logic::add_gate(nl, GateKind::kDff, 5).value; // DFF1 driver
  const std::uint16_t tgt = logic::add_gate(nl, GateKind::kDff, 5).value; // DFF2 target
  const std::uint16_t buf = logic::add_gate(nl, GateKind::kBuf, 1).value; // BUF1 hold loop
  ignore_input(nl, drv, 1);
  ignore_input(nl, drv, 2);
  ignore_input(nl, drv, 3);
  ignore_input(nl, drv, 4);
  ignore_input(nl, tgt, 1);
  ignore_input(nl, tgt, 4);
  // D-side first (incremental cycle check): BUF1.Y -> DFF2.D, then
  // DFF2.Q -> BUF1.1. Exposed pins left: DFF1.D, DFF2.RST, DFF2.SET.
  assert(logic::wire(nl, buf, tgt, 0).ok());
  assert(logic::wire(nl, tgt, buf, 0).ok());
  assert(logic::wire(nl, drv, tgt, 2).ok()); // DFF1.Q -> DFF2.RST
  // Interface: [DFF1.D, DFF2.SET] in slot order; no block outputs
  // (every Q/NQ is wired or hidden).
  assert(logic::block_input_count(nl) == 2);
  assert(logic::block_output_count(nl) == 0);
  SeqState st;
  // Raise target Q via SET while the wired RST is quiet (driver Q=0).
  {
    const LogicValue in[2] = {kL, kH};
    assert(logic::apply_async_controls(nl, st, std::span<const LogicValue>(in, 2)).ok());
    assert(st.q[tgt] == kH);
  }
  // Step the driver to Q=1; the target holds via D=Q feedback
  // (snapshot RST reads the OLD driver Q=0). Now (Q1=H, Q2=H).
  {
    const LogicValue in[2] = {kH, kL};
    assert(logic::seq_step(nl, st, std::span<const LogicValue>(in, 2)).ok());
    assert(st.q[drv] == kH);
    assert(st.q[tgt] == kH);
  }
  // Wired RST is live: async-apply forces H->L with no clock advance.
  {
    const LogicValue in[2] = {kL, kL};
    assert(logic::apply_async_controls(nl, st, std::span<const LogicValue>(in, 2)).ok());
    assert(st.q[drv] == kH);
    assert(st.q[tgt] == kL);
  }
  // Simultaneous wired-RST + poked-SET: reset wins.
  {
    const LogicValue in[2] = {kL, kH};
    assert(logic::apply_async_controls(nl, st, std::span<const LogicValue>(in, 2)).ok());
    assert(st.q[tgt] == kL);
  }
  // Edge path agrees: step with live RST held forces rather than captures.
  {
    const LogicValue in[2] = {kL, kL};
    assert(logic::seq_step(nl, st, std::span<const LogicValue>(in, 2)).ok());
    assert(st.q[drv] == kL); // driver captures D=0 this edge...
    assert(st.q[tgt] == kL); // ...while the target is forced by old Q1=1
  }
  // RST quiet again: SET raises the target, proving the path is intact.
  {
    const LogicValue in[2] = {kL, kH};
    assert(logic::apply_async_controls(nl, st, std::span<const LogicValue>(in, 2)).ok());
    assert(st.q[tgt] == kH);
  }
}

} // namespace

int main() {
  test_fanin_control();
  test_parse_control_pins();
  test_truth_table();
  test_async_apply();
  test_hold();
  test_zero_migration();
  test_wired_rst();
  std::cout << "test_logic_seq_controls: OK\n";
  return 0;
}
