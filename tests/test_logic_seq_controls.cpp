#include "wengine/logic/eval.hpp"
#include "wengine/logic/netlist.hpp"
#include "wengine/logic/seq_state.hpp"
#include "wengine/logic/seq_step.hpp"
#include "wengine/logic/types.hpp"
#include "wengine/logic/yaml_store.hpp"
#include "wengine/cli/args.hpp"
#include "wengine/cli/logic_registry.hpp"
#include "wengine/cli/shell.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <span>
#include <sstream>
#include <string>
#include <vector>

using namespace wengine;
using wengine::logic::GateKind;
using wengine::logic::LogicValue;
using wengine::logic::Netlist;
using wengine::logic::SeqState;

namespace {

bool contains(const std::string &hay, const std::string &needle) {
  return hay.find(needle) != std::string::npos;
}

void no_history(cli::Shell &sh) { sh.set_history_file(""); }

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

namespace cli_session {

void test_cli_reset_flow() {
  // Quickstart §1: 2-flop reset-hold-release with step counting from zero.
  cli::Shell sh;
  no_history(sh);
  std::ostringstream out;
  assert(sh.execute("logic reset", out) == 0);
  assert(sh.execute("logic add DFF --inputs 5", out) == 0);
  assert(sh.execute("logic add DFF --inputs 5", out) == 0);
  assert(sh.execute("logic ignore DFF1.CLK", out) == 0);
  assert(sh.execute("logic ignore DFF2.CLK", out) == 0);
  assert(sh.execute("logic poke DFF1.D=1", out) == 0);
  assert(sh.execute("logic poke DFF2.D=1", out) == 0);
  out.str("");
  out.clear();
  assert(sh.execute("logic step 1", out) == 0);
  assert(contains(out.str(), "clk: cycle 1 (1 edges)"));
  assert(contains(out.str(), "DFF1.Q: 0->1"));
  assert(contains(out.str(), "DFF2.Q: 0->1"));
  // Assert reset: immediate force lines, no clock advance.
  out.str("");
  out.clear();
  assert(sh.execute("logic poke DFF1.RST=1", out) == 0);
  assert(contains(out.str(), "poked DFF1.RST=1"));
  assert(contains(out.str(), "DFF1.Q: 1->0 (NQ 0->1)"));
  assert(sh.execute("logic poke DFF2.RST=1", out) == 0);
  assert(contains(out.str(), "DFF2.Q: 1->0 (NQ 0->1)"));
  out.str("");
  out.clear();
  assert(sh.execute("logic peek", out) == 0);
  assert(contains(out.str(), "DFF1.Q=0 NQ=1"));
  assert(contains(out.str(), "DFF2.Q=0 NQ=1"));
  // Held reset freezes counting: two edges, no change.
  out.str("");
  out.clear();
  assert(sh.execute("logic step 2", out) == 0);
  assert(contains(out.str(), "clk: cycle 3 (2 edges)"));
  assert(contains(out.str(), "DFF1.Q: 0 (no change)"));
  assert(contains(out.str(), "DFF2.Q: 0 (no change)"));
  // Release and count from zero.
  assert(sh.execute("logic poke DFF1.RST=release", out) == 0);
  assert(sh.execute("logic poke DFF2.RST=release", out) == 0);
  out.str("");
  out.clear();
  assert(sh.execute("logic step 1", out) == 0);
  assert(contains(out.str(), "DFF1.Q: 0->1"));
  assert(contains(out.str(), "DFF2.Q: 0->1"));
}

void test_cli_reset_beats_hold() {
  // Async RST wins over disabled EN: force lands with no step, and the
  // held edge cannot revive Q while RST stays asserted.
  cli::Shell sh;
  no_history(sh);
  std::ostringstream out;
  assert(sh.execute("logic reset", out) == 0);
  assert(sh.execute("logic add DFF --inputs 5", out) == 0);
  assert(sh.execute("logic ignore DFF1.CLK", out) == 0);
  assert(sh.execute("logic poke DFF1.D=1", out) == 0);
  assert(sh.execute("logic step 1", out) == 0); // Q=1
  assert(sh.execute("logic poke DFF1.EN=0", out) == 0);
  assert(sh.execute("logic poke DFF1.D=0", out) == 0);
  out.str("");
  out.clear();
  assert(sh.execute("logic poke DFF1.RST=1", out) == 0);
  assert(contains(out.str(), "DFF1.Q: 1->0 (NQ 0->1)"));
  out.str("");
  out.clear();
  assert(sh.execute("logic step 1", out) == 0);
  assert(contains(out.str(), "DFF1.Q: 0 (no change)"));
  // Release RST (EN still 0): the flop holds 0, proving release alone
  // never forces and the hold path owns the edge.
  assert(sh.execute("logic poke DFF1.RST=release", out) == 0);
  out.str("");
  out.clear();
  assert(sh.execute("logic step 1", out) == 0);
  assert(contains(out.str(), "DFF1.Q: 0 (no change)"));
  // Release EN: normal capture resumes next edge.
  assert(sh.execute("logic poke DFF1.EN=1", out) == 0);
  assert(sh.execute("logic poke DFF1.D=1", out) == 0);
  out.str("");
  out.clear();
  assert(sh.execute("logic step 1", out) == 0);
  assert(contains(out.str(), "DFF1.Q: 0->1"));
}

void test_yaml_control_roundtrip() {  // Save/load preserves the 5-input shape, ignore mask, and the RST wire;
  // the reloaded net forces identically. Legacy inputs:2 loads with
  // controls absent (bare behavior).
  Netlist nl;
  const std::uint16_t f = logic::add_gate(nl, GateKind::kDff, 5).value;
  const std::uint16_t b = logic::add_gate(nl, GateKind::kBuf, 1).value;
  (void)b;
  logic::PinRef clk{};
  clk.slot = f;
  clk.is_output = false;
  clk.input = 1;
  assert(logic::ignore_pin(nl, clk).ok());
  assert(logic::wire_text(nl, "BUF1.Y", "DFF1.RST").ok());
  std::string text;
  logic::yaml::save_text(nl, text);
  assert(contains(text, "inputs: 5"));
  assert(contains(text, "DFF1.RST"));
  Netlist back;
  std::string err;
  assert(logic::yaml::load_text(text, back, err));
  assert(back.gates[f].input_count == 5);
  assert(back.gates[f].drivers[2] != logic::kNoDriver);
  std::string again;
  logic::yaml::save_text(back, again);
  assert(again == text); // byte-identical round-trip
  // Re-assert on the loaded net: SET raises Q, then driving the wired
  // RST net (BUF1.1 block input) forces it back with no step.
  {
    logic::SeqState st;
    // Interface order: [DFF1.D, DFF1.SET, DFF1.EN, BUF1.1].
    assert(logic::block_input_count(back) == 4);
    const LogicValue seed[4] = {kL, kH, kH, kL};
    assert(logic::apply_async_controls(back, st, std::span<const LogicValue>(seed, 4)).ok());
    assert(st.q[f] == kH);
    const LogicValue force[4] = {kL, kL, kH, kH};
    assert(logic::apply_async_controls(back, st, std::span<const LogicValue>(force, 4)).ok());
    assert(st.q[f] == kL);
  }
  // Legacy bare flop: loads, rejects control names, steps D-only.
  const std::string legacy =
      "version: 2\n"
      "gates:\n"
      "  - {id: DFF1, kind: dff, inputs: 2, ignore: [1, 2], ignore_output: false, delay_ps: 0}\n"
      "wires:\n"
      "ties:\n"
      "meta:\n"
      "  tool: wengine-logic\n";
  Netlist old;
  assert(logic::yaml::load_text(legacy, old, err));
  assert(!logic::parse_pin(old, "DFF1.RST").ok());
  assert(!logic::parse_pin(old, "DFF1.3").ok());
  assert(logic::parse_pin(old, "DFF1.CLK").ok());
}

void test_cli_preset_flow() {
  // Quickstart §2: preset without clocking, hold across edges, release
  // latches nothing (next capture follows D).
  cli::Shell sh;
  no_history(sh);
  std::ostringstream out;
  assert(sh.execute("logic reset", out) == 0);
  assert(sh.execute("logic add DFF --inputs 5", out) == 0);
  assert(sh.execute("logic ignore DFF1.CLK", out) == 0);
  out.str("");
  out.clear();
  assert(sh.execute("logic poke DFF1.SET=1", out) == 0);
  assert(contains(out.str(), "poked DFF1.SET=1"));
  assert(contains(out.str(), "DFF1.Q: 0->1 (NQ 1->0)"));
  out.str("");
  out.clear();
  assert(sh.execute("logic peek DFF1", out) == 0);
  assert(contains(out.str(), "DFF1.Q=1 NQ=0"));
  // SET held across 2 edges with D=0: Q stays 1 (no-change, still forced).
  out.str("");
  out.clear();
  assert(sh.execute("logic step 2", out) == 0);
  assert(contains(out.str(), "DFF1.Q: 1 (no change)"));
  // Release: nothing moves until the next edge captures D=0.
  assert(sh.execute("logic poke DFF1.SET=release", out) == 0);
  out.str("");
  out.clear();
  assert(sh.execute("logic peek DFF1", out) == 0);
  assert(contains(out.str(), "DFF1.Q=1 NQ=0"));
  out.str("");
  out.clear();
  assert(sh.execute("logic step 1", out) == 0);
  assert(contains(out.str(), "DFF1.Q: 1->0 (NQ 0->1)"));
}

void test_cli_counter_seed() {
  // Seed a 2-bit counter (D0=NOT Q0, D1=Q1 XOR Q0) to 10 via SET, then
  // count 10 -> 11 -> 00. DFF1=Q0 (LSB), DFF2=Q1 (MSB).
  cli::Shell sh;
  no_history(sh);
  std::ostringstream out;
  assert(sh.execute("logic reset", out) == 0);
  assert(sh.execute("logic add DFF --inputs 5", out) == 0);
  assert(sh.execute("logic add DFF --inputs 5", out) == 0);
  assert(sh.execute("logic add NOT", out) == 0);
  assert(sh.execute("logic add XOR", out) == 0);
  assert(sh.execute("logic ignore DFF1.CLK", out) == 0);
  assert(sh.execute("logic ignore DFF2.CLK", out) == 0);
  // D-side first (incremental cycle check): comb -> flop D inputs.
  assert(sh.execute("logic wire NOT1.Y DFF1.D", out) == 0);
  assert(sh.execute("logic wire XOR1.Y DFF2.D", out) == 0);
  assert(sh.execute("logic wire DFF1.Q NOT1.1", out) == 0);
  assert(sh.execute("logic wire DFF1.Q XOR1.1", out) == 0);
  assert(sh.execute("logic wire DFF2.Q XOR1.2", out) == 0);
  // Seed (Q1,Q0) = (1,0): SET on DFF2 only.
  assert(sh.execute("logic poke DFF2.SET=1", out) == 0);
  out.str("");
  out.clear();
  assert(sh.execute("logic peek", out) == 0);
  assert(contains(out.str(), "DFF1.Q=0"));
  assert(contains(out.str(), "DFF2.Q=1"));
  assert(sh.execute("logic poke DFF2.SET=release", out) == 0);
  // 10 -> 11: D0 = NOT 0 = 1, D1 = 1 XOR 0 = 1.
  out.str("");
  out.clear();
  assert(sh.execute("logic step 1", out) == 0);
  assert(contains(out.str(), "DFF1.Q: 0->1"));
  assert(contains(out.str(), "DFF2.Q: 1 (no change)"));
  // 11 -> 00: D0 = NOT 1 = 0, D1 = 1 XOR 1 = 0.
  out.str("");
  out.clear();
  assert(sh.execute("logic step 1", out) == 0);
  assert(contains(out.str(), "DFF1.Q: 1->0"));
  assert(contains(out.str(), "DFF2.Q: 1->0"));
  out.str("");
  out.clear();
  assert(sh.execute("logic peek", out) == 0);
  assert(contains(out.str(), "DFF1.Q=0 NQ=1"));
  assert(contains(out.str(), "DFF2.Q=0 NQ=1"));
}

void test_cli_set_reset_conflict() {
  // RST+SET reads 0 everywhere; releasing RST flips to 1 with no step;
  // NQ tracks live. Same script twice -> byte-identical readback.
  const auto run_script = []() {
    cli::Shell sh;
    no_history(sh);
    std::ostringstream out;
    assert(sh.execute("logic reset", out) == 0);
    out.str(""); // drop the reset line: its gate count reflects prior state
    out.clear();
    assert(sh.execute("logic add DFF --inputs 5", out) == 0);
    assert(sh.execute("logic ignore DFF1.CLK", out) == 0);
    assert(sh.execute("logic poke DFF1.RST=1", out) == 0);
    assert(sh.execute("logic poke DFF1.SET=1", out) == 0);
    assert(sh.execute("logic peek DFF1", out) == 0);
    assert(sh.execute("logic poke DFF1.RST=release", out) == 0);
    assert(sh.execute("logic peek DFF1", out) == 0);
    assert(sh.execute("logic poke DFF1.RST=1", out) == 0);
    assert(sh.execute("logic peek DFF1", out) == 0);
    return out.str();
  };
  const std::string first = run_script();
  const std::string second = run_script();
  assert(first == second);
  assert(contains(first, "DFF1.Q=0 NQ=1")); // RST+SET -> 0, then RST alone -> 0
  assert(contains(first, "DFF1.Q: 0->1 (NQ 1->0)")); // RST release lets SET win
  assert(contains(first, "DFF1.Q: 1->0 (NQ 0->1)")); // re-asserted RST forces back
  assert(contains(first, "DFF1.Q=1 NQ=0")); // SET-alone readback
}

void test_cli_stall_flow() {
  // Quickstart §3: EN=0 holds across 3 edges while the sibling captures;
  // release resumes capture; dangling EN captures (enabled by default).
  cli::Shell sh;
  no_history(sh);
  std::ostringstream out;
  assert(sh.execute("logic reset", out) == 0);
  assert(sh.execute("logic add DFF --inputs 5", out) == 0);
  assert(sh.execute("logic add DFF --inputs 5", out) == 0);
  assert(sh.execute("logic ignore DFF1.CLK", out) == 0);
  assert(sh.execute("logic ignore DFF2.CLK", out) == 0);
  assert(sh.execute("logic poke DFF1.EN=0", out) == 0);
  assert(sh.execute("logic poke DFF1.D=1", out) == 0);
  assert(sh.execute("logic poke DFF2.D=1", out) == 0);
  out.str("");
  out.clear();
  assert(sh.execute("logic step 3", out) == 0);
  assert(contains(out.str(), "clk: cycle 3 (3 edges)"));
  assert(contains(out.str(), "DFF1.Q: 0 (no change)"));
  assert(contains(out.str(), "DFF2.Q: 0->1 (NQ 1->0)"));
  out.str("");
  out.clear();
  assert(sh.execute("logic peek", out) == 0);
  assert(contains(out.str(), "DFF1.Q=0 NQ=1"));
  assert(contains(out.str(), "DFF2.Q=1 NQ=0"));
  // Five-edge freeze on the held flop (SC-002): still 0, sibling stays 1.
  out.str("");
  out.clear();
  assert(sh.execute("logic step 5", out) == 0);
  assert(contains(out.str(), "DFF1.Q: 0 (no change)"));
  out.str("");
  out.clear();
  assert(sh.execute("logic peek DFF1", out) == 0);
  assert(contains(out.str(), "DFF1.Q=0 NQ=1"));
  // Release mid-sequence: capture resumes on the very next edge.
  assert(sh.execute("logic poke DFF1.EN=1", out) == 0);
  out.str("");
  out.clear();
  assert(sh.execute("logic step 1", out) == 0);
  assert(contains(out.str(), "DFF1.Q: 0->1 (NQ 1->0)"));
  // Dangling EN (never poked) captures: DFF2 proved it from power-on.
}

void test_cli_subset_enable() {  // Selective hold composes with clock-domain subsets: an EN=0 member
  // holds when its domain steps; an enabled non-member is untouched.
  cli::Shell sh;
  no_history(sh);
  std::ostringstream out;
  assert(sh.execute("logic reset", out) == 0);
  assert(sh.execute("logic add DFF --inputs 5", out) == 0); // DFF1: implicit clk
  assert(sh.execute("logic add DFF --inputs 5", out) == 0); // DFF2: driven clock
  assert(sh.execute("logic add BUF", out) == 0);
  assert(sh.execute("logic ignore DFF1.CLK", out) == 0);
  assert(sh.execute("logic wire BUF1.Y DFF2.CLK", out) == 0);
  assert(sh.execute("logic ignore DFF1.D", out) == 0);
  assert(sh.execute("logic poke DFF1.EN=0", out) == 0);
  assert(sh.execute("logic poke DFF2.D=1", out) == 0);
  assert(sh.execute("logic poke DFF2.EN=0", out) == 0);
  // Step the implicit domain: member holds (EN=0), driven flop untouched.
  out.str("");
  out.clear();
  assert(sh.execute("logic step clk 1", out) == 0);
  assert(contains(out.str(), "clk: cycle 1 (1 edges)"));
  assert(contains(out.str(), "DFF1.Q: 0 (no change)"));
  assert(!contains(out.str(), "DFF2.Q:"));
  // Step the driven domain: its EN=0 member holds too.
  out.str("");
  out.clear();
  assert(sh.execute("logic step BUF1.Y 1", out) == 0);
  assert(contains(out.str(), "BUF1.Y: cycle 1 (1 edges)"));
  assert(contains(out.str(), "DFF2.Q: 0 (no change)"));
  assert(!contains(out.str(), "DFF1.Q:"));
  // Enabled non-members still hold: enable DFF2, step the other domain.
  assert(sh.execute("logic poke DFF2.EN=1", out) == 0);
  out.str("");
  out.clear();
  assert(sh.execute("logic step clk 1", out) == 0);
  assert(!contains(out.str(), "DFF2.Q:"));
  // The enabled member captures when its own domain steps (D=1 poked).
  out.str("");
  out.clear();
  assert(sh.execute("logic step BUF1.Y 1", out) == 0);
  assert(contains(out.str(), "DFF2.Q: 0->1 (NQ 1->0)"));
}

namespace {

// Reference resolver mirroring research.md R3 (independent of the engine
// under test): reset > set > enable-hold > D capture.
char resolve_q(char q_old, char rst, char set, char en, char d) {
  if (rst == '1') {
    return '0';
  }
  if (set == '1') {
    return '1';
  }
  if (en == '0') {
    return q_old;
  }
  return d;
}

void run_case(char q_old, char rst, char set, char en, char d, std::string &peek_out) {
  cli::Shell sh;
  no_history(sh);
  std::ostringstream out;
  assert(sh.execute("logic reset", out) == 0);
  assert(sh.execute("logic add DFF --inputs 5", out) == 0);
  assert(sh.execute("logic ignore DFF1.CLK", out) == 0);
  if (q_old == '1') {
    // Establish Q=1 via SET, then release (release latches nothing).
    assert(sh.execute("logic poke DFF1.SET=1", out) == 0);
    assert(sh.execute("logic poke DFF1.SET=release", out) == 0);
  }
  std::string poke = "logic poke DFF1.RST=";
  poke += rst;
  assert(sh.execute(poke, out) == 0);
  poke = "logic poke DFF1.SET=";
  poke += set;
  assert(sh.execute(poke, out) == 0);
  poke = "logic poke DFF1.EN=";
  poke += en;
  assert(sh.execute(poke, out) == 0);
  poke = "logic poke DFF1.D=";
  poke += d;
  assert(sh.execute(poke, out) == 0);
  // Edge-capture rows (RST=0, SET=0) only resolve on an edge; forced rows
  // re-assert the same value through the step, so one step is uniform.
  assert(sh.execute("logic step 1", out) == 0);
  out.str("");
  out.clear();
  assert(sh.execute("logic peek DFF1", out) == 0);
  peek_out = out.str();
}

} // namespace

void test_cli_conflict_matrix() {
  // All 8 RST×SET×EN combinations × D × both Q_olds against the R3
  // reference. One path (Shell lines) serves REPL, one-shot, and piped
  // input alike (all funnel through Shell::execute); direct spec calls
  // below add the in-process spelling.
  for (char q_old : {'0', '1'}) {
    for (char rst : {'0', '1'}) {
      for (char set : {'0', '1'}) {
        for (char en : {'0', '1'}) {
          for (char d : {'0', '1'}) {
            std::string peek;
            run_case(q_old, rst, set, en, d, peek);
            const char want = resolve_q(q_old, rst, set, en, d);
            const char want_nq = (want == '1') ? '0' : '1';
            std::string line = "DFF1.Q=";
            line += want;
            line += " NQ=";
            line += want_nq;
            assert(contains(peek, line));
          }
        }
      }
    }
  }
  // Direct-call spelling parity on one conflict row (RST+SET, EN held):
  // same force line and readback as the Shell spelling.
  {
    cli::Shell sh;
    no_history(sh);
    std::ostringstream out;
    assert(sh.execute("logic reset", out) == 0);
    assert(sh.execute("logic add DFF --inputs 5", out) == 0);
    assert(sh.execute("logic ignore DFF1.CLK", out) == 0);
    std::array<std::string_view, 1> t_rst{"DFF1.RST=1"};
    cli::ArgCursor c_rst{std::span<const std::string_view>(t_rst)};
    assert(cli::logic::LogicPokeSpec::run(c_rst, out) == 0);
    assert(contains(out.str(), "poked DFF1.RST=1"));
    std::array<std::string_view, 1> t_set{"DFF1.SET=1"};
    cli::ArgCursor c_set{std::span<const std::string_view>(t_set)};
    assert(cli::logic::LogicPokeSpec::run(c_set, out) == 0);
    assert(contains(out.str(), "poked DFF1.SET=1"));
    std::array<std::string_view, 1> t_peek{"DFF1"};
    cli::ArgCursor c_peek{std::span<const std::string_view>(t_peek)};
    std::ostringstream pout;
    assert(cli::logic::LogicPeekSpec::run(c_peek, pout) == 0);
    assert(contains(pout.str(), "DFF1.Q=0 NQ=1")); // reset wins, async, no step
  }
}

void test_cli_control_completion() {
  // New pin names complete after `DFF1.` in wire/poke position (T015:
  // generic collect_pins/poke paths pick them up via pin_text).
  cli::Shell sh;
  no_history(sh);
  std::ostringstream out;
  assert(sh.execute("logic reset", out) == 0);
  assert(sh.execute("logic add DFF --inputs 5", out) == 0);
  assert(sh.execute("logic ignore DFF1.CLK", out) == 0);
  std::vector<std::string> names;
  {
    // One prior pin (the source) selects destination completion.
    const std::array<std::string_view, 1> prior{"BUF1.Y"};
    const std::span<const std::string_view> prior_span(prior);
    cli::logic::LogicWireSpec::complete(prior_span, "DFF1.", names);
    bool rst = false;
    bool set = false;
    bool en = false;
    for (const std::string &n : names) {
      if (n == "DFF1.RST") {
        rst = true;
      }
      if (n == "DFF1.SET") {
        set = true;
      }
      if (n == "DFF1.EN") {
        en = true;
      }
    }
    assert(rst && set && en);
  }
  {
    const std::span<const std::string_view> no_prior{};
    names.clear();
    cli::logic::LogicPokeSpec::complete(no_prior, "DFF1.R", names);
    bool seen = false;
    for (const std::string &n : names) {
      if (n == "DFF1.RST=1" || n == "DFF1.RST=0" || n == "DFF1.RST=release") {
        seen = true;
      }
    }
    assert(seen);
  }
}

void test_cli_determinism_replay() {  // SC-005: reset -> preset -> stall -> advance -> readback, replayed
  // twice byte-identically (async assertion lands between advances).
  const std::vector<std::string> script = {
      "logic add DFF --inputs 5",
      "logic add DFF --inputs 5",
      "logic ignore DFF1.CLK",
      "logic ignore DFF2.CLK",
      "logic poke DFF1.D=1",
      "logic poke DFF2.D=0",
      "logic step 2",
      "logic poke DFF2.SET=1",
      "logic peek",
      "logic poke DFF2.SET=release",
      "logic poke DFF1.EN=0",
      "logic step 3",
      "logic poke DFF1.RST=1",
      "logic peek",
      "logic step 1",
  };
  const auto run = [&]() {
    cli::Shell sh;
    no_history(sh);
    std::ostringstream out;
    assert(sh.execute("logic reset", out) == 0);
    out.str("");
    out.clear();
    for (const std::string &line : script) {
      assert(sh.execute(line, out) == 0);
    }
    return out.str();
  };
  const std::string first = run();
  const std::string second = run();
  assert(first == second);
  assert(contains(first, "DFF1.Q: 1->0 (NQ 0->1)")); // async RST between advances
  assert(contains(first, "clk: cycle 6 (1 edges)")); // 2 + 3 + 1 edges total
}

void test_cli_wired_control_session() {
  // FR-007 session boundary: a wired RST net forces Q with a force line
  // when its driver goes High; the shape survives save/reset/load and
  // re-asserts identically. Usage errors change no state.
  cli::Shell sh;
  no_history(sh);
  std::ostringstream out;
  assert(sh.execute("logic reset", out) == 0);
  assert(sh.execute("logic add DFF --inputs 5", out) == 0);
  assert(sh.execute("logic add BUF", out) == 0);
  assert(sh.execute("logic ignore DFF1.CLK", out) == 0);
  assert(sh.execute("logic ignore DFF1.D", out) == 0);
  // Raise Q via SET so the wired-RST force is observable H->L.
  assert(sh.execute("logic poke DFF1.SET=1", out) == 0);
  assert(sh.execute("logic poke DFF1.SET=release", out) == 0);
  // Wiring a quiet driver (dangling BUF input reads Low) forces nothing.
  out.str("");
  out.clear();
  assert(sh.execute("logic wire BUF1.Y DFF1.RST", out) == 0);
  assert(contains(out.str(), "wired BUF1.Y -> DFF1.RST"));
  assert(!contains(out.str(), "DFF1.Q:"));
  // Driving the net High forces Q at once with a force line.
  out.str("");
  out.clear();
  assert(sh.execute("logic poke BUF1.1=1", out) == 0);
  assert(contains(out.str(), "poked BUF1.1=1"));
  assert(contains(out.str(), "DFF1.Q: 1->0 (NQ 0->1)"));
  out.str("");
  out.clear();
  assert(sh.execute("logic peek DFF1", out) == 0);
  assert(contains(out.str(), "DFF1.Q=0 NQ=1"));
  // Invalid control stimulus is a usage error with no state change.
  out.str("");
  out.clear();
  assert(sh.execute("logic poke DFF1.RST=2", out) != 0);
  assert(sh.execute("logic peek DFF1", out) == 0);
  assert(contains(out.str(), "DFF1.Q=0 NQ=1"));
  // Interface lists the named control pins.
  out.str("");
  out.clear();
  assert(sh.execute("logic list --iface", out) == 0);
  assert(contains(out.str(), "DFF1.SET"));
  assert(contains(out.str(), "DFF1.EN"));
  // Save, wipe, reload: shape, mask, and wire survive; re-assert works.
  const std::string tmp =
      (std::filesystem::temp_directory_path() / "wengine_seq_controls_roundtrip.yaml").string();
  std::remove(tmp.c_str());
  assert(sh.execute(std::string("logic save ") + tmp, out) == 0);
  assert(contains(out.str(), "saved 2 gates 1 wires"));
  assert(sh.execute("logic reset", out) == 0);
  out.str("");
  out.clear();
  assert(sh.execute(std::string("logic load ") + tmp, out) == 0);
  assert(contains(out.str(), "loaded 2 gates 1 wires"));
  out.str("");
  out.clear();
  assert(sh.execute("logic poke DFF1.SET=1", out) == 0);
  assert(contains(out.str(), "DFF1.Q: 0->1 (NQ 1->0)"));
  assert(sh.execute("logic poke DFF1.SET=release", out) == 0);
  out.str("");
  out.clear();
  assert(sh.execute("logic poke BUF1.1=1", out) == 0);
  assert(contains(out.str(), "DFF1.Q: 1->0 (NQ 0->1)"));
  std::remove(tmp.c_str());
  // Bare 2-input flops reject control names like out-of-range numerics.
  assert(sh.execute("logic reset", out) == 0);
  assert(sh.execute("logic add DFF", out) == 0);
  out.str("");
  out.clear();
  assert(sh.execute("logic poke DFF1.RST=1", out) != 0);
  assert(sh.execute("logic peek DFF1", out) == 0);
  assert(contains(out.str(), "DFF1.Q=0 NQ=1"));
}

void test_cli_step_zero_live_controls() {
  // `step 0` with live async controls: reports current (forced) state,
  // moves no cycle counter, and emits no duplicate force line (every
  // mutation path already applied).
  cli::Shell sh;
  no_history(sh);
  std::ostringstream out;
  assert(sh.execute("logic reset", out) == 0);
  assert(sh.execute("logic add DFF --inputs 5", out) == 0);
  assert(sh.execute("logic ignore DFF1.CLK", out) == 0);
  assert(sh.execute("logic poke DFF1.D=1", out) == 0);
  assert(sh.execute("logic step 1", out) == 0); // Q=1, cycle 1
  assert(sh.execute("logic poke DFF1.RST=1", out) == 0); // force 1->0 now
  out.str("");
  out.clear();
  assert(sh.execute("logic step 0", out) == 0);
  assert(contains(out.str(), "clk: cycle 1 (0 edges, no-op)"));
  assert(contains(out.str(), "DFF1.Q=0 NQ=1"));
  assert(!contains(out.str(), "DFF1.Q:"));
}

} // namespace cli_session

int main() {
  test_fanin_control();
  test_parse_control_pins();
  test_truth_table();
  test_async_apply();
  test_hold();
  test_zero_migration();
  test_wired_rst();
  cli_session::test_cli_reset_flow();
  cli_session::test_cli_reset_beats_hold();
  cli_session::test_yaml_control_roundtrip();
  cli_session::test_cli_preset_flow();
  cli_session::test_cli_counter_seed();
  cli_session::test_cli_set_reset_conflict();
  cli_session::test_cli_stall_flow();
  cli_session::test_cli_subset_enable();
  cli_session::test_cli_conflict_matrix();
  cli_session::test_cli_control_completion();
  cli_session::test_cli_determinism_replay();
  cli_session::test_cli_wired_control_session();
  cli_session::test_cli_step_zero_live_controls();
  std::cout << "test_logic_seq_controls: OK\n";
  return 0;
}
