// test_logic_buffer: enable-gated buffer + per-net pulls (012) — engine
// seam (kind lifecycle, buffer eval truth table, netlist seam, pull
// mapping, termination) through CLI verbs to YAML round-trip. Links
// wengine_cli + wengine_logic_yaml to prove the seam-to-verb-to-artifact
// chain.
//
// Reference anchoring (constitution I/III, FR-013): the buffer truth
// table and pulled-resolution goldens below are hand-derived from the
// spec contracts (contracts/engine-seam.md), never from the
// implementation's output — a golden that merely echoes the code proves
// nothing.
#include "wengine/logic/block_interface.hpp"
#include "wengine/logic/eval.hpp"
#include "wengine/logic/netlist.hpp"
#include "wengine/logic/optimize.hpp"
#include "wengine/logic/types.hpp"
#include "wengine/logic/yaml_store.hpp"

#include "wengine/cli/logic_registry.hpp"
#include "wengine/cli/shell.hpp"
#include "wengine/tui/exec.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace wengine;

namespace {

constexpr logic::LogicValue kL = logic::LogicValue::kLow;
constexpr logic::LogicValue kH = logic::LogicValue::kHigh;
constexpr logic::LogicValue kZ = logic::LogicValue::kHighZ;
constexpr logic::LogicValue kX = logic::LogicValue::kUnknown;

// US1 (T009): byte identity, display naming, fan-in, per-kind counters.
// The byte is pinned by static_assert: renumbering is a loud break.
void test_tbuf_kind_identity() {
  static_assert(static_cast<std::uint8_t>(logic::GateKind::kTbuf) == 8);
  assert(logic::kind_ordinal(logic::GateKind::kTbuf) == 8);
  assert(logic::gate_kind_name(logic::GateKind::kTbuf) == "TBUF");
  assert(logic::valid_fanin(logic::GateKind::kTbuf, 2));
  assert(!logic::valid_fanin(logic::GateKind::kTbuf, 0));
  assert(!logic::valid_fanin(logic::GateKind::kTbuf, 1));
  assert(!logic::valid_fanin(logic::GateKind::kTbuf, 3));
  assert(!logic::valid_fanin(logic::GateKind::kTbuf, 8));
  assert(logic::default_fanin(logic::GateKind::kTbuf) == 2);
  assert(!logic::is_sequential_kind(logic::GateKind::kTbuf));
  assert(logic::parse_gate_kind("TBUF").value == logic::GateKind::kTbuf);
  assert(logic::parse_gate_kind("tbuf").value == logic::GateKind::kTbuf);
  assert(logic::parse_gate_kind("Tbuf").value == logic::GateKind::kTbuf);
  assert(!logic::parse_gate_kind("TBUF1").ok());
  assert(!logic::parse_gate_kind("TBU").ok());

  logic::Netlist nl{};
  const auto t = logic::add_gate(nl, logic::GateKind::kTbuf, 2);
  assert(t.ok());
  assert(nl.gates[t.value].counter == 1);
  assert(nl.gates[t.value].input_count == 2);
  assert(nl.next_counter[8] == 2);
  // Display identity TBUF<n>; bad fan-in fails loudly.
  assert(logic::resolve_gate(nl, "TBUF1").value == t.value);
  assert(!logic::add_gate(nl, logic::GateKind::kTbuf, 1).ok());
  assert(!logic::add_gate(nl, logic::GateKind::kTbuf, 3).ok());
  assert(nl.live_count == 1);
  // Per-kind counters are never reused (slots recycle, counters don't).
  assert(logic::remove_gate(nl, t.value).ok());
  const auto t2 = logic::add_gate(nl, logic::GateKind::kTbuf, 2);
  assert(t2.ok() && nl.gates[t2.value].counter == 2);
  assert(logic::resolve_gate(nl, "TBUF2").value == t2.value);
  assert(!logic::resolve_gate(nl, "TBUF1").ok());
  // Prefix fallthrough (research D1): "T1" never matches TBUF1 — the T
  // row's numeric tail check fails and the scan keeps going.
  assert(!logic::resolve_gate(nl, "T1").ok());
  assert(!logic::resolve_gate(nl, "TBUF").ok());
}

// US1 (T009): the independently derived ENxD table (research D2):
// released enable drives high-Z regardless of data; asserted enable
// drives the data level. Indexed [enable][data]; written as literals
// from the contract, never generated from eval_gate.
void test_tbuf_eval_truth_table() {
  constexpr logic::LogicValue kWant[2][2] = {
      {kZ, kZ}, // EN low:  D=0 -> Z, D=1 -> Z
      {kL, kH}, // EN high: D=0 -> L, D=1 -> H
  };
  constexpr logic::LogicValue kLevel[2] = {kL, kH};
  for (std::uint8_t e = 0; e < 2; ++e) {
    for (std::uint8_t d = 0; d < 2; ++d) {
      const logic::LogicValue ins[2] = {kLevel[d], kLevel[e]};
      assert(logic::eval_gate(logic::GateKind::kTbuf, ins, 2) == kWant[e][d]);
    }
  }
}

// Solve one netlist over one input vector; fails loudly on X/Z.
bool try_solve(logic::Netlist &nl, std::span<const logic::LogicValue> ins,
               std::span<logic::LogicValue> outs, logic::SolveDiag *diag = nullptr) {
  return logic::solve(nl, ins, outs, diag).ok();
}

// AND1 (data src) + AND2 (enable src) + TBUF1 + NOT1 (sink).
struct BufferedBus {
  std::uint16_t data = 0;
  std::uint16_t en = 0;
  std::uint16_t tbuf = 0;
  std::uint16_t sink = 0;
};

BufferedBus make_buffered_bus(logic::Netlist &nl) {
  BufferedBus b{};
  b.data = logic::add_gate(nl, logic::GateKind::kAnd, 2).value;
  b.en = logic::add_gate(nl, logic::GateKind::kAnd, 2).value;
  b.tbuf = logic::add_gate(nl, logic::GateKind::kTbuf, 2).value;
  b.sink = logic::add_gate(nl, logic::GateKind::kNot, 1).value;
  assert(logic::wire(nl, b.data, b.tbuf, 0).ok());
  assert(logic::wire(nl, b.en, b.tbuf, 1).ok());
  assert(logic::wire(nl, b.tbuf, b.sink, 0).ok());
  return b;
}

// US1 (T010): buffer inputs wire with the full per-wire validation —
// buffer data/enable edges bypass nothing.
void test_tbuf_wire_validation() {
  logic::Netlist nl{};
  const BufferedBus b = make_buffered_bus(nl);
  (void)b;
  const logic::Netlist before = nl;
  // Second driver on an occupied, unmarked buffer input (slot 2 is
  // TBUF1): the single-driver rule holds for buffer inputs too.
  const auto extra = logic::add_gate(nl, logic::GateKind::kAnd, 2);
  assert(extra.ok());
  assert(!logic::wire(nl, extra.value, 2, 0).ok());
  assert(!logic::wire(nl, extra.value, 2, 1).ok());
  // Dead slots, out-of-range pins, and output pins as sinks fail.
  assert(!logic::wire(nl, extra.value, 999, 0).ok());
  assert(!logic::wire(nl, 999, 0, 0).ok());
  assert(!logic::wire(nl, extra.value, 0, 7).ok());
  const logic::PinRef out{.slot = extra.value, .is_output = true, .input = 0};
  const logic::PinRef in{.slot = 0, .is_output = false, .input = 0};
  assert(!logic::wire_pins(nl, in, out).ok());
  // Marked buffer output attaches to a shared net within the cap.
  logic::Netlist shared{};
  const auto s = logic::add_gate(shared, logic::GateKind::kOr, 2);
  const auto tb = logic::add_gate(shared, logic::GateKind::kTbuf, 2);
  assert(s.ok() && tb.ok());
  assert(logic::mark_shared(shared, s.value, 0).ok());
  assert(logic::wire(shared, tb.value, s.value, 0).ok());
  assert(logic::shared_attachment_count(shared, s.value, 0) == 1);
}

// US1 (T010): enabled drives data identically to an ordinary driver;
// released contributes high-Z and the consumer fails loudly with the
// netlist uncorrupted (a later successful solve still works).
void test_tbuf_enable_gating() {
  logic::Netlist nl{};
  (void)make_buffered_bus(nl);
  // Block inputs (slot order): AND1.1, AND1.2, AND2.1, AND2.2.
  // Reference netlist: AND1 wired straight into NOT1.
  logic::Netlist ref{};
  const auto rd = logic::add_gate(ref, logic::GateKind::kAnd, 2).value;
  const auto rs = logic::add_gate(ref, logic::GateKind::kNot, 1).value;
  assert(logic::wire(ref, rd, rs, 0).ok());
  for (std::uint8_t a1 = 0; a1 < 2; ++a1) {
    for (std::uint8_t a2 = 0; a2 < 2; ++a2) {
      for (std::uint8_t e1 = 0; e1 < 2; ++e1) {
        for (std::uint8_t e2 = 0; e2 < 2; ++e2) {
          const logic::LogicValue lv[4] = {
              a1 != 0 ? kH : kL, a2 != 0 ? kH : kL, e1 != 0 ? kH : kL, e2 != 0 ? kH : kL,
          };
          const logic::LogicValue r_in[2] = {lv[0], lv[1]};
          std::array<logic::LogicValue, 1> r_out = {kL};
          assert(try_solve(ref, r_in, r_out));
          std::array<logic::LogicValue, 1> out = {kL};
          const bool enabled = (e1 != 0) && (e2 != 0);
          if (enabled) {
            // Enabled: identical to the ordinary driver, every combo.
            assert(try_solve(nl, lv, out));
            assert(out[0] == r_out[0]);
          } else {
            // Released: the sink sees Z and solve fails loudly.
            assert(!try_solve(nl, lv, out));
          }
        }
      }
    }
  }
  // The netlist is uncorrupted by the failed solves: enable and win.
  const logic::LogicValue win[4] = {kH, kH, kH, kH};
  std::array<logic::LogicValue, 1> out = {kL};
  assert(try_solve(nl, win, out));
  assert(out[0] == kL); // NOT(1 & 1)
}

// US1 (T010): X/Z/dangling at the buffer's own seams fail loudly.
// Dangling enable never defaults on or off; dangling data never
// invents a value; an X driver and a Z driver both refuse at the seam.
void test_tbuf_loud_input_seams() {
  {
    // Dangling enable is an ordinary block input (caller-driven, never
    // defaulted on/off): driven high the buffer conducts; a short
    // vector (missing drive) and an X drive both refuse loudly.
    logic::Netlist nl{};
    const auto d = logic::add_gate(nl, logic::GateKind::kAnd, 2).value;
    const auto t = logic::add_gate(nl, logic::GateKind::kTbuf, 2).value;
    const auto s = logic::add_gate(nl, logic::GateKind::kNot, 1).value;
    assert(logic::wire(nl, d, t, 0).ok());
    assert(logic::wire(nl, t, s, 0).ok());
    // Block inputs: AND1.1, AND1.2, TBUF1.2 (dangling enable).
    const logic::LogicValue ins[3] = {kH, kH, kH};
    std::array<logic::LogicValue, 1> out = {kL};
    assert(try_solve(nl, ins, out));
    assert(out[0] == kL); // NOT(1 & 1), conducted
    const logic::LogicValue short_vec[2] = {kH, kH};
    assert(!try_solve(nl, short_vec, out)); // missing drive: loud
    // A block-level X on the enable also refuses loudly.
    const logic::LogicValue bad[3] = {kH, kH, kX};
    assert(!try_solve(nl, bad, out));
  }
  {
    // Dangling data with a driven enable conducts when driven; an X
    // drive refuses loudly (never invents a value).
    logic::Netlist nl{};
    const auto e = logic::add_gate(nl, logic::GateKind::kAnd, 2).value;
    const auto t = logic::add_gate(nl, logic::GateKind::kTbuf, 2).value;
    const auto s = logic::add_gate(nl, logic::GateKind::kNot, 1).value;
    assert(logic::wire(nl, e, t, 1).ok());
    assert(logic::wire(nl, t, s, 0).ok());
    // Block inputs: AND1.1, AND1.2, TBUF1.1 (dangling data).
    const logic::LogicValue ins[3] = {kH, kH, kH};
    std::array<logic::LogicValue, 1> out = {kL};
    assert(try_solve(nl, ins, out));
    assert(out[0] == kL);
    const logic::LogicValue bad[3] = {kX, kH, kH};
    assert(!try_solve(nl, bad, out));
  }
  {
    // X at the data seam (shared-net contention upstream) fails loudly
    // at the buffer input with the fight counted.
    logic::Netlist nl{};
    const auto a1 = logic::add_gate(nl, logic::GateKind::kAnd, 2).value;
    const auto a2 = logic::add_gate(nl, logic::GateKind::kAnd, 2).value;
    const auto o = logic::add_gate(nl, logic::GateKind::kOr, 2).value;
    const auto e = logic::add_gate(nl, logic::GateKind::kAnd, 2).value;
    const auto t = logic::add_gate(nl, logic::GateKind::kTbuf, 2).value;
    const auto s = logic::add_gate(nl, logic::GateKind::kNot, 1).value;
    assert(logic::mark_shared(nl, o, 0).ok());
    assert(logic::wire(nl, a1, o, 0).ok());
    assert(logic::wire(nl, a2, o, 0).ok());
    assert(logic::wire(nl, o, t, 0).ok());
    assert(logic::wire(nl, e, t, 1).ok());
    assert(logic::wire(nl, t, s, 0).ok());
    // a1 drives 1, a2 drives 0 -> contention X at TBUF1.1.
    // Block inputs: AND1.1, AND1.2, AND2.1, AND2.2, OR1.2, AND3.1, AND3.2.
    const logic::LogicValue ins[7] = {kH, kH, kL, kL, kH, kH, kH};
    std::array<logic::LogicValue, 1> out = {kL};
    logic::SolveDiag diag{};
    assert(!try_solve(nl, ins, out, &diag));
    assert(diag.per_net[logic::tie_flat(o, 0)] == 1);
  }
  {
    // Z at the data seam (released buffer upstream) fails loudly at
    // the downstream buffer's input.
    logic::Netlist nl{};
    const auto d = logic::add_gate(nl, logic::GateKind::kAnd, 2).value;
    const auto en = logic::add_gate(nl, logic::GateKind::kAnd, 2).value;
    const auto t1 = logic::add_gate(nl, logic::GateKind::kTbuf, 2).value;
    const auto t2 = logic::add_gate(nl, logic::GateKind::kTbuf, 2).value;
    const auto s = logic::add_gate(nl, logic::GateKind::kNot, 1).value;
    assert(logic::wire(nl, d, t1, 0).ok());
    assert(logic::wire(nl, en, t1, 1).ok());
    assert(logic::wire(nl, t1, t2, 0).ok());
    assert(logic::wire(nl, en, t2, 1).ok());
    assert(logic::wire(nl, t2, s, 0).ok());
    // Enable low: t1 releases, t2's data seam sees Z -> loud failure.
    const logic::LogicValue ins[4] = {kH, kH, kL, kL};
    std::array<logic::LogicValue, 1> out = {kL};
    assert(!try_solve(nl, ins, out));
  }
}

// --- US2 (pulls) -------------------------------------------------------------

// US2 (T013): the all-Z map and its inertias, hand-derived from the
// contract (contracts/engine-seam.md): an all-floating span maps through
// the destination pull (up -> High, down -> Low, none -> loud failure);
// driven spans resolve exactly as without any pull under every setting.
void test_pull_mapping() {
  logic::Netlist nl{};
  const auto o = logic::add_gate(nl, logic::GateKind::kOr, 2).value;
  const auto a1 = logic::add_gate(nl, logic::GateKind::kAnd, 2).value;
  const auto a2 = logic::add_gate(nl, logic::GateKind::kAnd, 2).value;
  assert(logic::mark_shared(nl, o, 0).ok());
  std::array<logic::LogicValue, logic::kMaxGates> gate_out{};
  gate_out.fill(kL);
  std::array<logic::LogicValue, logic::kMaxGates * logic::kMaxInputs> resolved{};
  resolved.fill(kL);
  logic::LogicValue out = kL;
  const std::size_t flat = logic::tie_flat(o, 0);
  // Empty span (no drivers at all): unpulled fails loudly (shipped
  // rule) — the pull map only fires when a pull is set.
  assert(!logic::is_pulled_flat(nl, flat));
  assert(!logic::resolve_input(nl, o, 0, gate_out.data(), resolved.data(), out, nullptr));

  assert(logic::set_pull(nl, o, 0, 1).ok());
  assert(logic::resolve_input(nl, o, 0, gate_out.data(), resolved.data(), out, nullptr));
  assert(out == kH);
  assert(logic::set_pull(nl, o, 0, 2).ok());
  assert(logic::resolve_input(nl, o, 0, gate_out.data(), resolved.data(), out, nullptr));
  assert(out == kL);
  assert(logic::clear_pull(nl, o, 0).ok());
  assert(!logic::resolve_input(nl, o, 0, gate_out.data(), resolved.data(), out, nullptr));

  // Driven agreement is inert under every pull setting.
  assert(logic::wire(nl, a1, o, 0).ok());
  for (std::uint8_t p = 0; p < 3; ++p) {
    if (p == 1) {
      assert(logic::set_pull(nl, o, 0, 1).ok());
    } else if (p == 2) {
      assert(logic::set_pull(nl, o, 0, 2).ok());
    } else {
      assert(!logic::is_pulled_flat(nl, flat)); // already unpulled here
    }
    gate_out[a1] = kH;
    assert(logic::resolve_input(nl, o, 0, gate_out.data(), resolved.data(), out, nullptr));
    assert(out == kH);
    gate_out[a1] = kL;
    assert(logic::resolve_input(nl, o, 0, gate_out.data(), resolved.data(), out, nullptr));
    assert(out == kL);
  }
  // Low-vs-high contention refuses loudly under every pull, counted once.
  assert(logic::wire(nl, a2, o, 0).ok());
  gate_out[a1] = kH;
  gate_out[a2] = kL;
  for (std::uint8_t p = 0; p < 3; ++p) {
    if (p == 1) {
      assert(logic::set_pull(nl, o, 0, 1).ok());
    } else if (p == 2) {
      assert(logic::set_pull(nl, o, 0, 2).ok());
    } else {
      assert(logic::clear_pull(nl, o, 0).ok());
    }
    logic::SolveDiag diag{};
    assert(!logic::resolve_input(nl, o, 0, gate_out.data(), resolved.data(), out, &diag));
    assert(diag.per_net[flat] == 1);
  }
  // Unknown dominance refuses loudly under every pull, never counted.
  assert(logic::unwire_driver(nl, a2, o, 0).ok()); // drop the extra
  assert(logic::shared_attachment_count(nl, o, 0) == 1);
  gate_out[a1] = kX;
  for (std::uint8_t p = 0; p < 3; ++p) {
    if (p == 1) {
      assert(logic::set_pull(nl, o, 0, 1).ok());
    } else if (p == 2) {
      assert(logic::set_pull(nl, o, 0, 2).ok());
    } else {
      assert(logic::clear_pull(nl, o, 0).ok());
    }
    logic::SolveDiag diag{};
    assert(!logic::resolve_input(nl, o, 0, gate_out.data(), resolved.data(), out, &diag));
    assert(diag.per_net[flat] == 0);
  }
}

// US2 (T013): attachment order never changes the resolved value — the
// funnel is multiset-order-independent with pulls in play.
void test_pull_permutation_invariance() {
  logic::Netlist fwd{};
  const auto fo = logic::add_gate(fwd, logic::GateKind::kOr, 2).value;
  const auto fa = logic::add_gate(fwd, logic::GateKind::kAnd, 2).value;
  const auto fb = logic::add_gate(fwd, logic::GateKind::kAnd, 2).value;
  assert(logic::mark_shared(fwd, fo, 0).ok());
  assert(logic::wire(fwd, fa, fo, 0).ok());
  assert(logic::wire(fwd, fb, fo, 0).ok());
  logic::Netlist rev{};
  const auto ro = logic::add_gate(rev, logic::GateKind::kOr, 2).value;
  const auto ra = logic::add_gate(rev, logic::GateKind::kAnd, 2).value;
  const auto rb = logic::add_gate(rev, logic::GateKind::kAnd, 2).value;
  assert(logic::mark_shared(rev, ro, 0).ok());
  assert(logic::wire(rev, rb, ro, 0).ok());
  assert(logic::wire(rev, ra, ro, 0).ok());
  std::array<logic::LogicValue, logic::kMaxGates> gate_out{};
  gate_out.fill(kL);
  std::array<logic::LogicValue, logic::kMaxGates * logic::kMaxInputs> resolved{};
  resolved.fill(kL);
  for (std::uint8_t p = 0; p < 3; ++p) {
    if (p > 0) {
      assert(logic::set_pull(fwd, fo, 0, p).ok());
      assert(logic::set_pull(rev, ro, 0, p).ok());
    }
    for (std::uint8_t va = 0; va < 2; ++va) {
      for (std::uint8_t vb = 0; vb < 2; ++vb) {
        gate_out[fa] = (va != 0) ? kH : kL;
        gate_out[fb] = (vb != 0) ? kH : kL;
        gate_out[ra] = gate_out[fa];
        gate_out[rb] = gate_out[fb];
        logic::LogicValue of = kL, orr = kL;
        logic::SolveDiag df{}, dr{};
        const bool okf = logic::resolve_input(fwd, fo, 0, gate_out.data(), resolved.data(), of, &df);
        const bool okr = logic::resolve_input(rev, ro, 0, gate_out.data(), resolved.data(), orr, &dr);
        assert(okf == okr);
        if (okf) {
          assert(of == orr);
        }
        assert(df.per_net[logic::tie_flat(fo, 0)] == dr.per_net[logic::tie_flat(ro, 0)]);
      }
    }
  }
}

// US2 (T013): parked-bus solve — the released buffer's Z maps to idle
// under a pull and fails loudly without one. End-to-end through solve.
void test_pull_parked_bus_solve() {
  logic::Netlist nl{};
  (void)make_buffered_bus(nl);
  // Park the bus: released enable (AND2 low), data high.
  const logic::LogicValue parked[4] = {kH, kH, kL, kL};
  std::array<logic::LogicValue, 1> out = {kL};
  assert(!try_solve(nl, parked, out)); // no pull: loud Z at the sink
  // Pull up on the *sink* input (NOT1.1, wired to the buffer output):
  // Z maps to idle High.
  assert(logic::set_pull(nl, 3, 0, 1).ok());
  assert(try_solve(nl, parked, out));
  assert(out[0] == kL); // NOT(idle High)
  assert(logic::set_pull(nl, 3, 0, 2).ok());
  assert(try_solve(nl, parked, out));
  assert(out[0] == kH); // NOT(idle Low)
  assert(logic::clear_pull(nl, 3, 0).ok());
  assert(!try_solve(nl, parked, out));
}

// US2 (T014): pulled dangling inputs terminate — invisible to interface
// derivation, resolved to idle without consuming a block-input column.
void test_pull_termination() {
  logic::Netlist nl{};
  const auto a = logic::add_gate(nl, logic::GateKind::kAnd, 2).value;
  assert(logic::set_pull(nl, a, 0, 1).ok());
  assert(logic::block_input_count(nl) == 1); // only AND1.2
  assert(logic::input_index(nl, a, 0) == logic::kNoDriver);
  assert(logic::input_index(nl, a, 1) != logic::kNoDriver);
  std::uint16_t slot = 0;
  std::uint8_t input = 0;
  assert(logic::block_input_pin(nl, 0, slot, input));
  assert(slot == a && input == 1);
  // The idle level flows as ordinary data.
  const logic::LogicValue hi[1] = {kH};
  std::array<logic::LogicValue, 1> out = {kL};
  assert(try_solve(nl, hi, out));
  assert(out[0] == kH); // H & idle-H
  const logic::LogicValue lo[1] = {kL};
  assert(try_solve(nl, lo, out));
  assert(out[0] == kL); // L & idle-H
  // Pull-down flips the terminated value.
  assert(logic::set_pull(nl, a, 0, 2).ok());
  assert(try_solve(nl, hi, out));
  assert(out[0] == kL); // H & idle-L
  // Predicates.
  assert(logic::any_pull(nl));
  assert(!logic::has_buffer_gates(nl));
  logic::Netlist fresh{};
  assert(!logic::any_pull(fresh));
  assert(!logic::has_buffer_gates(fresh));
}

// US2 (T014): tie/pull and ignore/pull are disjoint in both directions;
// failures leave the netlist untouched.
void test_pull_disjointness() {
  {
    logic::Netlist nl{};
    const auto a = logic::add_gate(nl, logic::GateKind::kAnd, 2).value;
    const auto b = logic::add_gate(nl, logic::GateKind::kAnd, 2).value;
    // Tie first, then pull: refused.
    const logic::PinRef pa{.slot = a, .is_output = false, .input = 0};
    const logic::PinRef pb{.slot = b, .is_output = false, .input = 0};
    assert(logic::tie_pins(nl, pa, pb).ok());
    const logic::Netlist before = nl;
    assert(!logic::set_pull(nl, a, 0, 1).ok());
    assert(!logic::set_pull(nl, b, 0, 1).ok());
    assert(nl == before);
    // Pull first, then tie: refused.
    assert(logic::untie_pin(nl, pa).ok());
    assert(logic::set_pull(nl, a, 0, 1).ok());
    assert(!logic::tie_pins(nl, pa, pb).ok());
    assert(logic::is_pulled_flat(nl, logic::tie_flat(a, 0)));
  }
  {
    logic::Netlist nl{};
    const auto a = logic::add_gate(nl, logic::GateKind::kAnd, 2).value;
    // Ignore first, then pull: refused.
    const logic::PinRef p{.slot = a, .is_output = false, .input = 0};
    assert(logic::ignore_pin(nl, p).ok());
    const logic::Netlist before = nl;
    assert(!logic::set_pull(nl, a, 0, 1).ok());
    assert(nl == before);
    // Pull first, then ignore: refused.
    assert(logic::use_pin(nl, p).ok());
    assert(logic::set_pull(nl, a, 0, 2).ok());
    assert(!logic::ignore_pin(nl, p).ok());
    assert(logic::pull_level_flat(nl, logic::tie_flat(a, 0)) == 2);
  }
  {
    logic::Netlist nl{};
    const auto a = logic::add_gate(nl, logic::GateKind::kAnd, 2).value;
    // Pull lifecycle refusals: bad levels, dead pins, unpulled clear.
    assert(!logic::set_pull(nl, a, 0, 0).ok());
    assert(!logic::set_pull(nl, a, 0, 3).ok());
    assert(!logic::set_pull(nl, 999, 0, 1).ok());
    assert(!logic::set_pull(nl, a, 7, 1).ok());
    assert(!logic::clear_pull(nl, a, 0).ok()); // no pull set
    assert(!logic::clear_pull(nl, 999, 0).ok());
    assert(logic::set_pull(nl, a, 0, 1).ok());
    assert(logic::set_pull(nl, a, 0, 1).ok()); // idempotent re-set
    assert(logic::set_pull(nl, a, 0, 2).ok()); // replace
    assert(logic::clear_pull(nl, a, 0).ok());
    assert(!logic::is_pulled_flat(nl, logic::tie_flat(a, 0)));
  }
}

// US2 (T014): unwire-to-dangling keeps the stored pull (the net becomes
// terminated-dangling); remove/reset clear it.
void test_pull_lifecycle_storage() {
  logic::Netlist nl{};
  const auto a = logic::add_gate(nl, logic::GateKind::kAnd, 2).value;
  const auto n = logic::add_gate(nl, logic::GateKind::kNot, 1).value;
  assert(logic::wire(nl, a, n, 0).ok());
  assert(logic::set_pull(nl, n, 0, 1).ok());
  assert(logic::unwire(nl, n, 0).ok());
  assert(logic::is_pulled_flat(nl, logic::tie_flat(n, 0))); // stored pull survives
  assert(logic::input_index(nl, n, 0) == logic::kNoDriver); // terminated
  // Remove clears the slot's pulls.
  assert(logic::remove_gate(nl, n).ok());
  const auto n2 = logic::add_gate(nl, logic::GateKind::kNot, 1);
  assert(n2.ok());
  for (std::size_t f = 0; f < logic::kMaxGates * logic::kMaxInputs; ++f) {
    assert(!logic::is_pulled_flat(nl, f));
  }
  // Reset zeroes the array.
  assert(logic::set_pull(nl, a, 0, 1).ok());
  assert(logic::any_pull(nl));
  logic::reset_netlist(nl);
  assert(!logic::any_pull(nl));
  for (std::size_t f = 0; f < logic::kMaxGates * logic::kMaxInputs; ++f) {
    assert(nl.pull_cfg[f] == 0);
  }
}

// --- US3 (one funnel) -------------------------------------------------------

namespace detail {
// Reference outcome for one gated span, hand-derived from the shipped
// 010/011 funnel table plus the two 012 float-mapping rows (research
// D4): X present -> loud failure; low+high -> loud failure (counted);
// all-Z/empty -> idle level under a pull, loud failure without one;
// otherwise the agreed level. Pulls never alter a driven outcome.
// Returns true on success (want set), false on loud failure.
bool reference_span(const logic::LogicValue *span, std::size_t n, std::uint8_t pull, logic::LogicValue &want,
                    bool &want_count) {
  bool saw_low = false;
  bool saw_high = false;
  bool saw_x = false;
  for (std::size_t k = 0; k < n; ++k) {
    saw_low = saw_low || (span[k] == kL);
    saw_high = saw_high || (span[k] == kH);
    saw_x = saw_x || (span[k] == kX);
  }
  want_count = saw_low && saw_high; // span assembly, before any pull map
  if (saw_x) {
    return false;
  }
  if (saw_low && saw_high) {
    return false;
  }
  if (!saw_low && !saw_high) {
    if (pull == 1) {
      want = kH;
      return true;
    }
    if (pull == 2) {
      want = kL;
      return true;
    }
    return false;
  }
  want = saw_high ? kH : kL;
  (void)pull; // driven spans are inert under every pull setting
  return true;
}
} // namespace detail

// Build a marked sink with n attachments wired in the given driver
// order; returns the sink slot. Drivers are plain AND gates whose
// gate_out values the test sets directly (resolve_input reads values,
// never driver kinds — Z carriers are faithful to released buffers).
std::uint16_t make_span(logic::Netlist &nl, const std::uint16_t *order, std::size_t n, std::uint8_t pull) {
  const auto sink = logic::add_gate(nl, logic::GateKind::kOr, 2).value;
  assert(logic::mark_shared(nl, sink, 0).ok());
  if (pull != 0) {
    assert(logic::set_pull(nl, sink, 0, pull).ok());
  }
  for (std::size_t k = 0; k < n; ++k) {
    assert(logic::wire(nl, order[k], sink, 0).ok());
  }
  return sink;
}

// US3 (T019): exhaustive gated-span verification — every enabled/
// disabled x ordinary mix up to the cap, every pull setting, forward
// and reverse attachment orders. Values, ok-ness, and counts match the
// independent reference exactly and are order-invariant by funnel
// construction.
void test_gated_span_exhaustive() {
  constexpr logic::LogicValue kLevels[3] = {kL, kH, kZ};
  for (std::size_t n = 0; n <= 4; ++n) {
    // Enumerate value vectors over {L, H, Z} (3^n, 1..81 cases).
    std::size_t total = 1;
    for (std::size_t k = 0; k < n; ++k) {
      total *= 3;
    }
    for (std::size_t code = 0; code < total; ++code) {
      logic::LogicValue span[4] = {kL, kL, kL, kL};
      std::size_t c = code;
      for (std::size_t k = 0; k < n; ++k) {
        span[k] = kLevels[c % 3];
        c /= 3;
      }
      for (std::uint8_t pull = 0; pull < 3; ++pull) {
        logic::LogicValue want = kL;
        bool want_count = false;
        const bool wok = detail::reference_span(span, n, pull, want, want_count);
        // Forward and reverse attachment orders converge to identical
        // arrays by funnel construction (multiset-order-independent).
        for (int flip = 0; flip < 2; ++flip) {
          logic::Netlist nl{};
          std::array<std::uint16_t, 4> drivers{};
          for (std::size_t k = 0; k < n; ++k) {
            drivers[k] = logic::add_gate(nl, logic::GateKind::kAnd, 2).value;
          }
          std::array<std::uint16_t, 4> order{};
          for (std::size_t k = 0; k < n; ++k) {
            order[k] = drivers[(flip == 0) ? k : (n - 1 - k)];
          }
          const std::uint16_t sink = make_span(nl, order.data(), n, pull);
          std::array<logic::LogicValue, logic::kMaxGates> gate_out{};
          gate_out.fill(kL);
          for (std::size_t k = 0; k < n; ++k) {
            gate_out[order[k]] = span[k];
          }
          std::array<logic::LogicValue, logic::kMaxGates * logic::kMaxInputs> resolved{};
          resolved.fill(kL);
          logic::LogicValue got = kL;
          logic::SolveDiag diag{};
          const std::size_t flat = logic::tie_flat(sink, 0);
          const bool gok =
              logic::resolve_input(nl, sink, 0, gate_out.data(), resolved.data(), got, &diag);
          assert(gok == wok);
          if (wok) {
            assert(got == want);
          }
          assert((diag.per_net[flat] != 0) == want_count);
        }
      }
    }
  }
  // X spot-cases: unknown dominates under every pull, never counted.
  struct XCase {
    std::size_t n;
    logic::LogicValue span[3];
  };
  const XCase kXCases[] = {
      {1, {kX, kL, kL}}, {2, {kX, kL, kL}}, {2, {kX, kH, kL}}, {2, {kX, kZ, kL}}, {3, {kX, kL, kH}},
  };
  for (const XCase &xc : kXCases) {
    for (std::uint8_t pull = 0; pull < 3; ++pull) {
      logic::LogicValue want = kL;
      bool want_count = false;
      assert(!detail::reference_span(xc.span, xc.n, pull, want, want_count));
      // Counting is span-assembly (L+H present), even when X dominates.
      bool has_l = false;
      bool has_h = false;
      for (std::size_t k = 0; k < xc.n; ++k) {
        has_l = has_l || (xc.span[k] == kL);
        has_h = has_h || (xc.span[k] == kH);
      }
      assert(want_count == (has_l && has_h));
      logic::Netlist nl{};
      std::array<std::uint16_t, 3> drivers{};
      for (std::size_t k = 0; k < xc.n; ++k) {
        drivers[k] = logic::add_gate(nl, logic::GateKind::kAnd, 2).value;
      }
      const std::uint16_t sink = make_span(nl, drivers.data(), xc.n, pull);
      std::array<logic::LogicValue, logic::kMaxGates> gate_out{};
      gate_out.fill(kL);
      for (std::size_t k = 0; k < xc.n; ++k) {
        gate_out[drivers[k]] = xc.span[k];
      }
      std::array<logic::LogicValue, logic::kMaxGates * logic::kMaxInputs> resolved{};
      resolved.fill(kL);
      logic::LogicValue got = kL;
      logic::SolveDiag diag{};
      assert(!logic::resolve_input(nl, sink, 0, gate_out.data(), resolved.data(), got, &diag));
      assert((diag.per_net[logic::tie_flat(sink, 0)] != 0) == want_count);
    }
  }
}

// A gated bus that succeeds on every input combination: buffer data and
// enable from ordinary drivers, buffer + data driver sharing a marked
// sink with a pull. Released, the Z carrier is transparent beside the
// agreeing driver; pulled, the parked net idles.
struct GatedBus {
  std::uint16_t a = 0;
  std::uint16_t e = 0;
  std::uint16_t t = 0;
  std::uint16_t s = 0;
};

GatedBus make_gated_bus(logic::Netlist &nl) {
  GatedBus b{};
  b.a = logic::add_gate(nl, logic::GateKind::kAnd, 2).value;
  b.e = logic::add_gate(nl, logic::GateKind::kAnd, 2).value;
  b.t = logic::add_gate(nl, logic::GateKind::kTbuf, 2).value;
  b.s = logic::add_gate(nl, logic::GateKind::kOr, 2).value;
  assert(logic::wire(nl, b.a, b.t, 0).ok());
  assert(logic::wire(nl, b.e, b.t, 1).ok());
  assert(logic::mark_shared(nl, b.s, 0).ok());
  assert(logic::wire(nl, b.t, b.s, 0).ok());
  assert(logic::wire(nl, b.a, b.s, 0).ok());
  assert(logic::set_pull(nl, b.s, 0, 1).ok());
  // The sink's spare input stays out of the interface (ignored dangles
  // tie Low) so the bus has exactly the four driver block inputs.
  const logic::PinRef spare{.slot = b.s, .is_output = false, .input = 1};
  assert(logic::ignore_pin(nl, spare).ok());
  return b;
}

// US3 (T019): O0/O1 preserve gated-bus resolution exactly, freeze the
// buffer, keep the pull — and optimize-of-optimize is idempotent.
void test_optimizer_preserves_gated_bus() {
  for (int level = 0; level <= 1; ++level) {
    logic::Netlist nl{};
    const GatedBus b = make_gated_bus(nl);
    (void)b;
    logic::OptimizeOptions opts{};
    opts.level = (level == 0) ? logic::OptLevel::kO0 : logic::OptLevel::kO1;
    auto r = logic::optimize(nl, opts);
    assert(r.ok());
    logic::Netlist opt = r.value;
    // The buffer survives every pass: live, still a TBUF, still wired
    // as data+enable; the span and the pull are intact.
    assert(opt.gates[b.t].live && opt.gates[b.t].kind == logic::GateKind::kTbuf);
    assert(logic::is_shared_input(opt, b.s, 0));
    assert(logic::shared_attachment_count(opt, b.s, 0) == 2);
    assert(logic::is_pulled_flat(nl, logic::tie_flat(b.s, 0)));
    assert(logic::pull_level_flat(opt, logic::tie_flat(b.s, 0)) == 1);
    // All 16 input combos agree pre/post (ok-ness and outputs).
    for (int row = 0; row < 16; ++row) {
      const logic::LogicValue in[4] = {(row & 1) != 0 ? kH : kL, (row & 2) != 0 ? kH : kL,
                                       (row & 4) != 0 ? kH : kL, (row & 8) != 0 ? kH : kL};
      std::array<logic::LogicValue, 1> want = {kL};
      std::array<logic::LogicValue, 1> got = {kL};
      const bool wok = logic::solve(nl, std::span<const logic::LogicValue>(in, 4),
                                    std::span<logic::LogicValue>(want.data(), 1))
                           .ok();
      const bool gok = logic::solve(opt, std::span<const logic::LogicValue>(in, 4),
                                    std::span<logic::LogicValue>(got.data(), 1))
                           .ok();
      assert(wok && gok); // this bus never floats: Z is transparent here
      assert(want[0] == got[0]);
      assert(want[0] == (((row & 3) == 3) ? kH : kL)); // out == A1 == in0&in1
    }
    // Optimize-of-optimize is idempotent (equality covers pull_cfg).
    auto r2 = logic::optimize(opt, opts);
    assert(r2.ok());
    assert(r2.value == opt);
  }
}

// US3 (T019): sink-side skip on pulled inputs. Two shapes, both of
// which a skip-less optimizer observably corrupts:
// (A) collapse target: a pulled root with a constant label would be
// replaced by a canonical constant gate, silently dropping the pull
// (persistent topology state — list/save/O2-refusal all see it);
// (B) drop target: a pulled input that shifts under constant-input
// dropping orphans the pull flat-keyed behind it (the 011 extras
// rationale), changing values and the interface.
void test_optimizer_skips_pulled_sink() {
  for (int level = 0; level <= 1; ++level) {
    logic::OptimizeOptions opts{};
    opts.level = (level == 0) ? logic::OptLevel::kO0 : logic::OptLevel::kO1;
    {
      // Shape A: R = AND(C0, pulled-idle-H) = C0, R.Y dangling (root).
      logic::Netlist nl{};
      const auto src = logic::add_gate(nl, logic::GateKind::kAnd, 2).value;
      const logic::PinRef s0{.slot = src, .is_output = false, .input = 0};
      const logic::PinRef s1{.slot = src, .is_output = false, .input = 1};
      assert(logic::ignore_pin(nl, s0).ok());
      assert(logic::ignore_pin(nl, s1).ok());
      const auto r = logic::add_gate(nl, logic::GateKind::kAnd, 2).value;
      assert(logic::wire(nl, src, r, 0).ok());
      assert(logic::set_pull(nl, r, 1, 1).ok());
      assert(logic::block_input_count(nl) == 0); // pulled input terminates
      std::array<logic::LogicValue, 1> want = {kL};
      assert(logic::solve(nl, std::span<const logic::LogicValue>(), std::span<logic::LogicValue>(want.data(), 1))
                 .ok());
      assert(want[0] == kL); // C0 & idle-H
      auto res = logic::optimize(nl, opts);
      assert(res.ok());
      logic::Netlist opt = res.value;
      // The pulled root is frozen: kind, fan-in, pull, and value intact.
      assert(opt.gates[r].live && opt.gates[r].kind == logic::GateKind::kAnd);
      assert(opt.gates[r].input_count == 2);
      assert(logic::pull_level_flat(opt, logic::tie_flat(r, 1)) == 1);
      assert(logic::block_input_count(opt) == 0);
      std::array<logic::LogicValue, 1> got = {kH};
      assert(logic::solve(opt, std::span<const logic::LogicValue>(), std::span<logic::LogicValue>(got.data(), 1))
                 .ok());
      assert(got[0] == want[0]);
      auto r2 = logic::optimize(opt, opts);
      assert(r2.ok());
      assert(r2.value == opt);
    }
    {
      // Shape B: V = AND(C1, pulled-idle-H) = C1, V.Y observed.
      logic::Netlist nl{};
      const auto src = logic::add_gate(nl, logic::GateKind::kNand, 2).value;
      const logic::PinRef s0{.slot = src, .is_output = false, .input = 0};
      const logic::PinRef s1{.slot = src, .is_output = false, .input = 1};
      assert(logic::ignore_pin(nl, s0).ok());
      assert(logic::ignore_pin(nl, s1).ok()); // NAND(L,L) = C1
      const auto v = logic::add_gate(nl, logic::GateKind::kAnd, 2).value;
      const auto sink = logic::add_gate(nl, logic::GateKind::kNot, 1).value;
      assert(logic::wire(nl, src, v, 0).ok());
      assert(logic::set_pull(nl, v, 1, 1).ok());
      assert(logic::wire(nl, v, sink, 0).ok());
      assert(logic::block_input_count(nl) == 0); // pulled input terminates
      std::array<logic::LogicValue, 1> want = {kL};
      assert(logic::solve(nl, std::span<const logic::LogicValue>(), std::span<logic::LogicValue>(want.data(), 1))
                 .ok());
      assert(want[0] == kL); // NOT(C1 & idle-H)
      auto res = logic::optimize(nl, opts);
      assert(res.ok());
      logic::Netlist opt = res.value;
      // The victim gate is frozen: no drop, no retag, pull intact.
      assert(opt.gates[v].live && opt.gates[v].kind == logic::GateKind::kAnd);
      assert(opt.gates[v].input_count == 2);
      assert(logic::pull_level_flat(opt, logic::tie_flat(v, 1)) == 1);
      assert(logic::block_input_count(opt) == 0);
      std::array<logic::LogicValue, 1> got = {kH};
      assert(logic::solve(opt, std::span<const logic::LogicValue>(), std::span<logic::LogicValue>(got.data(), 1))
                 .ok());
      assert(got[0] == want[0]);
      auto r2 = logic::optimize(opt, opts);
      assert(r2.ok());
      assert(r2.value == opt);
    }
    {
      // Shape C: BUF with a terminated (dangling pulled-up) input feeds
      // a NOT. Bypassing the BUF would rewire the NOT to the raw
      // unwired driver — value and interface change — so the BUF is
      // frozen and the idle flows through.
      logic::Netlist nl{};
      const auto b = logic::add_gate(nl, logic::GateKind::kBuf, 1).value;
      const auto sink = logic::add_gate(nl, logic::GateKind::kNot, 1).value;
      assert(logic::set_pull(nl, b, 0, 1).ok());
      assert(logic::wire(nl, b, sink, 0).ok());
      assert(logic::block_input_count(nl) == 0); // pulled input terminates
      std::array<logic::LogicValue, 1> want = {kL};
      assert(logic::solve(nl, std::span<const logic::LogicValue>(), std::span<logic::LogicValue>(want.data(), 1))
                 .ok());
      assert(want[0] == kL); // NOT(idle-H)
      auto res = logic::optimize(nl, opts);
      assert(res.ok());
      logic::Netlist opt = res.value;
      assert(opt.gates[b].live && opt.gates[b].kind == logic::GateKind::kBuf);
      assert(logic::pull_level_flat(opt, logic::tie_flat(b, 0)) == 1);
      assert(logic::block_input_count(opt) == 0);
      std::array<logic::LogicValue, 1> got = {kH};
      assert(logic::solve(opt, std::span<const logic::LogicValue>(), std::span<logic::LogicValue>(got.data(), 1))
                 .ok());
      assert(got[0] == want[0]);
      auto r2 = logic::optimize(opt, opts);
      assert(r2.ok());
      assert(r2.value == opt);
    }
  }
}

// All-Low per-slot power-on values (fresh-attach default, save seam).
std::array<logic::LogicValue, logic::kMaxGates> zero_inits() {
  std::array<logic::LogicValue, logic::kMaxGates> in{};
  in.fill(logic::LogicValue::kLow);
  return in;
}

// US4 (T024): gated bus with marks, attachments, and pulls round-trips
// save -> load -> save byte-identically with identical replay.
void test_yaml_round_trip() {
  logic::Netlist nl{};
  const GatedBus b = make_gated_bus(nl);
  // A second, terminated pull on a dangling input persists too.
  assert(logic::set_pull(nl, b.a, 0, 2).ok());
  std::string text;
  logic::yaml::save_text(nl, zero_inits(), text);
  // Canonical bytes: tbuf row, shared + pulls rows, canonical order.
  assert(text.find("  - {id: TBUF1, kind: tbuf, inputs: 2, ignore: [], ignore_output: false, delay_ps: 0}\n") !=
         std::string::npos);
  assert(text.find("shared:\n  - {net: OR1.1}\n") != std::string::npos);
  assert(text.find("pulls:\n  - {net: AND1.1, pull: down}\n  - {net: OR1.1, pull: up}\n") != std::string::npos);
  const std::size_t p_wires = text.find("wires:\n");
  const std::size_t p_shared = text.find("shared:\n");
  const std::size_t p_pulls = text.find("pulls:\n");
  const std::size_t p_ties = text.find("ties:\n");
  assert(p_wires < p_shared && p_shared < p_pulls && p_pulls < p_ties);
  logic::yaml::Workspace ws{};
  std::string err;
  assert(logic::yaml::load_text(text, ws, err));
  // Kinds, wirings, marks, attachments, and pulls all survive.
  assert(ws.netlist.gates[b.t].live && ws.netlist.gates[b.t].kind == logic::GateKind::kTbuf);
  assert(logic::is_shared_input(ws.netlist, b.s, 0));
  assert(logic::shared_attachment_count(ws.netlist, b.s, 0) == 2);
  assert(logic::pull_level_flat(ws.netlist, logic::tie_flat(b.s, 0)) == 1);
  assert(logic::pull_level_flat(ws.netlist, logic::tie_flat(b.a, 0)) == 2);
  std::string again;
  logic::yaml::save_text(ws.netlist, zero_inits(), again);
  assert(again == text);
  // Replay outputs identical across the whole input space (the extra
  // terminated pull shrinks the bus to AND1.2 + AND2.1 + AND2.2; the
  // output is constant Low: AND(idle-L, .) with a agreeing/Z-transparent
  // gated span beside it).
  for (int row = 0; row < 8; ++row) {
    const logic::LogicValue in[3] = {(row & 1) != 0 ? kH : kL, (row & 2) != 0 ? kH : kL,
                                     (row & 4) != 0 ? kH : kL};
    std::array<logic::LogicValue, 1> want = {kH};
    std::array<logic::LogicValue, 1> got = {kH};
    const bool wok = logic::solve(nl, std::span<const logic::LogicValue>(in, 3),
                                  std::span<logic::LogicValue>(want.data(), 1))
                         .ok();
    const bool gok = logic::solve(ws.netlist, std::span<const logic::LogicValue>(in, 3),
                                  std::span<logic::LogicValue>(got.data(), 1))
                         .ok();
    assert(wok && gok);
    assert(want[0] == kL && got[0] == kL);
  }
}

// --- US5 (one vocabulary) ----------------------------------------------------

// US5 (T027): O2 refuses loudly on buffer/pull netlists — input
// untouched, O0/O1 remain available.
void test_optimizer_o2_refusal() {
  {
    logic::Netlist nl{};
    (void)make_gated_bus(nl);
    const logic::Netlist before = nl;
    logic::OptimizeOptions opts{};
    opts.level = logic::OptLevel::kO2;
    auto res = logic::optimize(nl, opts);
    assert(!res.ok());
    assert(std::string_view(res.error).find("unsupported at O2") != std::string_view::npos);
    assert(nl == before); // pure: input never modified
  }
  {
    // Buffer-only netlist (no marks): the refusal names buffers/pulls.
    logic::Netlist nl{};
    const auto a = logic::add_gate(nl, logic::GateKind::kAnd, 2).value;
    const auto e = logic::add_gate(nl, logic::GateKind::kAnd, 2).value;
    const auto t = logic::add_gate(nl, logic::GateKind::kTbuf, 2).value;
    const auto s = logic::add_gate(nl, logic::GateKind::kNot, 1).value;
    assert(logic::wire(nl, a, t, 0).ok());
    assert(logic::wire(nl, e, t, 1).ok());
    assert(logic::wire(nl, t, s, 0).ok());
    const logic::Netlist before = nl;
    logic::OptimizeOptions opts{};
    opts.level = logic::OptLevel::kO2;
    auto res = logic::optimize(nl, opts);
    assert(!res.ok());
    assert(std::string_view(res.error).find("buffers/pulls") != std::string_view::npos);
    assert(nl == before);
  }
  {
    // Pull-only netlists refuse O2 too (fresh-pool emitter has no
    // flat-keyed pull vocabulary).
    logic::Netlist nl{};
    const auto a = logic::add_gate(nl, logic::GateKind::kAnd, 2).value;
    assert(logic::set_pull(nl, a, 0, 1).ok());
    const logic::Netlist before = nl;
    logic::OptimizeOptions opts{};
    opts.level = logic::OptLevel::kO2;
    auto res = logic::optimize(nl, opts);
    assert(!res.ok());
    assert(std::string_view(res.error).find("buffers/pulls") != std::string_view::npos);
    assert(nl == before);
  }
}

// Run one session line through the process-wide workspace Shell.
int run_line(const std::string &line, std::string &text) {
  wengine::cli::Shell sh;
  sh.set_history_file("");
  std::ostringstream out;
  const int rc = sh.execute(line, out);
  text = out.str();
  return rc;
}

// US5 (T027): word-path entry guards — `truth` refuses live-buffer
// netlists loudly (naming the construct, pointing at scalar solve)
// while pull-only netlists stream unchanged.
void test_word_path_refusals() {
  wengine::cli::logic::reset_logic_workspace();
  std::string text;
  assert(run_line("logic add TBUF", text) == 0);
  assert(run_line("logic add AND --inputs 2", text) == 0);
  assert(run_line("logic add NOT", text) == 0);
  assert(run_line("logic wire AND1.Y TBUF1.1", text) == 0);
  assert(run_line("logic wire TBUF1.Y NOT1.1", text) == 0);
  assert(run_line("logic truth", text) == 1);
  assert(text.find("enable-gated buffers need scalar solve") != std::string::npos);
  // The same netlist solves scalar (four values flow one lane at a time).
  assert(run_line("logic solve --in 111", text) == 0);
  wengine::cli::logic::reset_logic_workspace();
  assert(run_line("logic add AND --inputs 2", text) == 0);
  assert(run_line("logic pull AND1.1 up", text) == 0);
  // Pull-only truth streams unchanged (terminated input shrinks the
  // interface to one block input, two rows).
  assert(run_line("logic truth", text) == 0);
  assert(text.find("AND1.1") == std::string::npos); // terminated: no column
  assert(text.find("AND1.2") != std::string::npos);
  wengine::cli::logic::reset_logic_workspace();
}

// Run a whole session script on both front ends from identical states;
// every line's code and bytes must match (TUI parity by construction).
void check_tui_script(const std::vector<std::string> &lines) {
  std::vector<std::pair<int, std::string>> via_exec;
  {
    wengine::cli::logic::reset_logic_workspace();
    wengine::tui::TuiExecutor exec;
    for (const std::string &line : lines) {
      const wengine::tui::ExecResult r = exec.execute_line(line);
      via_exec.emplace_back(r.rc, r.text);
    }
  }
  std::vector<std::pair<int, std::string>> via_shell;
  {
    wengine::cli::logic::reset_logic_workspace();
    wengine::cli::Shell shell;
    shell.set_history_file("");
    for (const std::string &line : lines) {
      std::ostringstream out;
      const int rc = shell.execute(line, out);
      via_shell.emplace_back(rc, out.str());
    }
  }
  assert(via_exec.size() == via_shell.size());
  for (std::size_t i = 0; i < via_exec.size(); ++i) {
    assert(via_exec[i].first == via_shell[i].first);
    assert(via_exec[i].second == via_shell[i].second);
  }
  wengine::cli::logic::reset_logic_workspace();
}

// US5 (T027): the new vocabulary is byte-identical across the TUI
// executor and the Shell (both ride cli::Shell::execute verbatim).
void test_tui_parity() {
  check_tui_script({
      "logic reset",
      "logic add TBUF",
      "logic add AND --inputs 2",
      "logic add NOT",
      "logic wire AND1.Y TBUF1.1",
      "logic pull NOT1.1 up",
      "logic pull TBUF1.2 down",
      "logic list",
      "logic solve --in 10",
      "logic unpull NOT1.1",
      "logic list",
      "logic pull TBUF1.1 sideways",
      "logic unpull TBUF1.1",
      "logic add TBUF --inputs 1",
      "logic truth",
  });
}

} // namespace

int main() {
  test_tbuf_kind_identity();
  test_tbuf_eval_truth_table();
  test_tbuf_wire_validation();
  test_tbuf_enable_gating();
  test_tbuf_loud_input_seams();
  test_pull_mapping();
  test_pull_permutation_invariance();
  test_pull_parked_bus_solve();
  test_pull_termination();
  test_pull_disjointness();
  test_pull_lifecycle_storage();
  test_gated_span_exhaustive();
  test_optimizer_preserves_gated_bus();
  test_optimizer_skips_pulled_sink();
  test_yaml_round_trip();
  test_optimizer_o2_refusal();
  test_word_path_refusals();
  test_tui_parity();
  std::cout << "test_buffer: OK\n";
  return 0;
}
