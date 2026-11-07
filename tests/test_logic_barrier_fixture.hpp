#pragma once

// Shared flop/latch opaque-barrier fixture for test_logic (SEQ11).
// Extracted from test_logic.cpp so barrier builders/oracles live in one
// place instead of bloating the test TU. Header-only, assert-based like
// the test itself.

#include "wengine/logic/netlist.hpp"
#include "wengine/logic/topo.hpp"
#include "wengine/logic/seq_state.hpp"
#include "wengine/logic/seq_step.hpp"
#include "wengine/logic/types.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// ---- SEQ11 flop/latch opaque barrier (007-flop-latch-barrier) -----------
// Helpers shared by every barrier test (T002). Builders hand-place
// sequential gates (add_gate stages sequential-kind creation, so tests use
// the same white-box placement as test_logic_seq_step.cpp). Per-kind
// counters are assigned in slot order so O2's fresh build reproduces the
// same display names.
void barrier_place_seq(wengine::logic::Netlist &nl, std::uint16_t slot, wengine::logic::GateKind kind, std::uint32_t counter, std::uint8_t inputs = 2) {
  assert(slot < wengine::logic::kMaxGates);
  assert(wengine::logic::is_sequential_kind(kind));
  assert(wengine::logic::valid_fanin(kind, inputs));
  wengine::logic::Gate &g = nl.gates[slot];
  assert(!g.live);
  g.live = true;
  g.kind = kind;
  g.counter = counter;
  g.input_count = inputs;
  g.ignored_mask = 0;
  g.ignore_output = false;
  g.ignore_nq_output = true;
  g.output_fanout = 0;
  g.nq_fanout = 0;
  g.propagation_delay_ps = 0;
  for (std::size_t i = 0; i < wengine::logic::kMaxInputs; ++i) {
    g.drivers[i] = wengine::logic::kNoDriver;
  }
  ++nl.live_count;
  nl.dirty = true;
  nl.topo_size = 0;
}

void barrier_ignore(wengine::logic::Netlist &nl, std::uint16_t slot, std::uint8_t input) {
  wengine::logic::PinRef pin{};
  pin.slot = slot;
  pin.is_output = false;
  pin.input = input;
  assert(wengine::logic::ignore_pin(nl, pin).ok());
}

// Closed 2-bit counter, no block I/O (DCE trap: zero fanout-0 sinks).
// Same excitation as test_logic_seq_step.cpp: Q0 toggles, Q1 carries.
wengine::logic::Netlist barrier_closed_counter() {
  wengine::logic::Netlist nl;
  barrier_place_seq(nl, 0, wengine::logic::GateKind::kDff, 1); // DFF1 = Q0 (LSB)
  barrier_place_seq(nl, 1, wengine::logic::GateKind::kDff, 2); // DFF2 = Q1 (MSB)
  const std::uint16_t n = wengine::logic::add_gate(nl, wengine::logic::GateKind::kNot, 1).value;
  const std::uint16_t x = wengine::logic::add_gate(nl, wengine::logic::GateKind::kXor, 2).value;
  assert(wengine::logic::wire(nl, n, 0, 0).ok()); // NOT.Y -> DFF1.D
  assert(wengine::logic::wire(nl, 0, n, 0).ok()); // Q0 -> NOT.1
  assert(wengine::logic::wire(nl, x, 1, 0).ok()); // XOR.Y -> DFF2.D
  assert(wengine::logic::wire(nl, 0, x, 0).ok()); // Q0 -> XOR.1
  assert(wengine::logic::wire(nl, 1, x, 1).ok()); // Q1 -> XOR.2
  barrier_ignore(nl, 0, 1);              // DFF1.CLK ignored (edge assumed)
  barrier_ignore(nl, 1, 1);              // DFF2.CLK ignored
  assert(wengine::logic::topo::validate(nl, wengine::logic::EdgeFilter::kCombOnly).ok());
  assert(wengine::logic::block_input_count(nl) == 0);
  assert(wengine::logic::block_output_count(nl) == 0);
  return nl;
}

// Counter plus observable glue: AND3(Q0,Q1,EXT) block output plus a DLATCH
// sampling Q0 with EN frozen (ignored). One block input, two outputs.
wengine::logic::Netlist barrier_counter_glue() {
  wengine::logic::Netlist nl;
  barrier_place_seq(nl, 0, wengine::logic::GateKind::kDff, 1);   // DFF1 = Q0
  barrier_place_seq(nl, 1, wengine::logic::GateKind::kDff, 2);   // DFF2 = Q1
  const std::uint16_t n = wengine::logic::add_gate(nl, wengine::logic::GateKind::kNot, 1).value;
  const std::uint16_t x = wengine::logic::add_gate(nl, wengine::logic::GateKind::kXor, 2).value;
  const std::uint16_t a = wengine::logic::add_gate(nl, wengine::logic::GateKind::kAnd, 3).value;
  barrier_place_seq(nl, 5, wengine::logic::GateKind::kDlatch, 1); // DLATCH1
  assert(wengine::logic::wire(nl, n, 0, 0).ok());          // NOT.Y -> DFF1.D
  assert(wengine::logic::wire(nl, 0, n, 0).ok());          // Q0 -> NOT.1
  assert(wengine::logic::wire(nl, x, 1, 0).ok());          // XOR.Y -> DFF2.D
  assert(wengine::logic::wire(nl, 0, x, 0).ok());          // Q0 -> XOR.1
  assert(wengine::logic::wire(nl, 1, x, 1).ok());          // Q1 -> XOR.2
  assert(wengine::logic::wire(nl, 0, a, 0).ok());          // Q0 -> AND.1
  assert(wengine::logic::wire(nl, 1, a, 1).ok());          // Q1 -> AND.2 (AND.3 = EXT)
  assert(wengine::logic::wire(nl, 0, 5, 0).ok());          // Q0 -> DLATCH1.D
  barrier_ignore(nl, 0, 1);                       // CLKs ignored
  barrier_ignore(nl, 1, 1);
  barrier_ignore(nl, 5, 1); // DLATCH1.EN ignored: frozen (hold row)
  assert(wengine::logic::topo::validate(nl, wengine::logic::EdgeFilter::kCombOnly).ok());
  assert(wengine::logic::block_input_count(nl) == 1);  // EXT
  assert(wengine::logic::block_output_count(nl) == 2); // AND1.Y, DLATCH1.Q
  return nl;
}

// Acyclic pipeline SER -> BUF -> DFF -> DFF -> OUT. The exact-simplify
// trap: solve() reads flop outputs as kLow, so a truth-table pass sees
// "constant" state. One block input, one output.
wengine::logic::Netlist barrier_pipeline() {
  wengine::logic::Netlist nl;
  const std::uint16_t b = wengine::logic::add_gate(nl, wengine::logic::GateKind::kBuf, 1).value; // BUF1 @0
  barrier_place_seq(nl, 1, wengine::logic::GateKind::kDff, 1);                          // DFF1 @1
  barrier_place_seq(nl, 2, wengine::logic::GateKind::kDff, 2);                          // DFF2 @2
  assert(wengine::logic::wire(nl, b, 1, 0).ok()); // BUF.Y -> DFF1.D
  assert(wengine::logic::wire(nl, 1, 2, 0).ok()); // Q1 -> DFF2.D
  barrier_ignore(nl, 1, 1);              // CLKs ignored
  barrier_ignore(nl, 2, 1);
  assert(wengine::logic::topo::validate(nl, wengine::logic::EdgeFilter::kCombOnly).ok());
  assert(wengine::logic::block_input_count(nl) == 1);  // SER (BUF.1 dangling)
  assert(wengine::logic::block_output_count(nl) == 1); // DFF2.Q
  return nl;
}

// JK flop with a NOT(Q) consumer feeding an output AND, plus an NQ-fed
// output AND: the O1/O2 polarity trap. J/K are the only block inputs (both
// land on verbatim sequential pins, so O2 never duplicates them); the ANDs
// close over Q/NQ so converted glue carries no dangling pins of its own.
wengine::logic::Netlist barrier_jk_nq() {
  wengine::logic::Netlist nl;
  barrier_place_seq(nl, 0, wengine::logic::GateKind::kJk, 1, 3); // JK1: J=0 K=1 CLK=2
  const std::uint16_t n = wengine::logic::add_gate(nl, wengine::logic::GateKind::kNot, 1).value;
  const std::uint16_t a = wengine::logic::add_gate(nl, wengine::logic::GateKind::kAnd, 2).value;
  const std::uint16_t q = wengine::logic::add_gate(nl, wengine::logic::GateKind::kAnd, 2).value;
  assert(wengine::logic::wire(nl, 0, n, 0).ok());  // Q -> NOT.1
  assert(wengine::logic::wire(nl, n, a, 0).ok());  // NOT.Y -> AND1.1
  assert(wengine::logic::wire(nl, 0, a, 1).ok());  // Q -> AND1.2 (AND1 = Q & ~Q)
  wengine::logic::PinRef nq{};                     // NQ -> AND2.1
  nq.slot = 0;
  nq.is_output = true;
  nq.negated = true;
  wengine::logic::PinRef dst{};
  dst.slot = q;
  dst.is_output = false;
  dst.input = 0;
  assert(wengine::logic::wire_pins(nl, nq, dst).ok());
  assert(wengine::logic::wire(nl, 0, q, 1).ok());  // Q -> AND2.2 (AND2 = ~Q & Q)
  barrier_ignore(nl, 0, 2); // JK1.CLK ignored (edge assumed)
  assert(wengine::logic::topo::validate(nl, wengine::logic::EdgeFilter::kCombOnly).ok());
  assert(wengine::logic::block_input_count(nl) == 2);  // J K
  assert(wengine::logic::block_output_count(nl) == 2); // AND1.Y AND2.Y
  return nl;
}

// Full-control 5-input DFF (D+CLK+RST+SET+EN) with every control pin
// ignored (inactive): no control pin may be dropped as dead.
wengine::logic::Netlist barrier_full_dff() {
  wengine::logic::Netlist nl;
  barrier_place_seq(nl, 0, wengine::logic::GateKind::kDff, 1, 5); // DFF1: D=0 rest ignored
  barrier_ignore(nl, 0, 1);                       // CLK
  barrier_ignore(nl, 0, 2);                       // RST
  barrier_ignore(nl, 0, 3);                       // SET
  barrier_ignore(nl, 0, 4);                       // EN
  assert(wengine::logic::topo::validate(nl, wengine::logic::EdgeFilter::kCombOnly).ok());
  assert(wengine::logic::block_input_count(nl) == 1);  // D
  assert(wengine::logic::block_output_count(nl) == 1); // Q
  return nl;
}

std::string barrier_gate_name(const wengine::logic::Netlist &nl, std::uint16_t s) {
  const wengine::logic::Gate &g = nl.gates[s];
  std::string n(wengine::logic::gate_kind_name(g.kind));
  n += std::to_string(g.counter);
  return n;
}

// Oracle core: every sequential gate in `before` resolves by display name
// in `after` with identical kind, pin count, flags, and per-pin shape
// (wired-vs-dangling, same Q/NQ polarity). With kStrict, wired pins
// must also read the same source gate by name — valid for the in-place
// O0/O1 levels; O2 converts upstream glue, so O2 tests use the shape-only
// form and rely on run parity for functional equivalence.
enum class SeqDriverCheck : std::uint8_t { kStrict = 0, kShapeOnly = 1 };

void assert_seq_equal_with_check(const wengine::logic::Netlist &before, const wengine::logic::Netlist &after, SeqDriverCheck check) {
  std::size_t n_before = 0;
  std::size_t n_after = 0;
  for (std::uint16_t s = 0; s < wengine::logic::kMaxGates; ++s) {
    if (after.gates[s].live && wengine::logic::is_sequential_kind(after.gates[s].kind)) {
      ++n_after;
    }
  }
  for (std::uint16_t s = 0; s < wengine::logic::kMaxGates; ++s) {
    const wengine::logic::Gate &g = before.gates[s];
    if (!g.live || !wengine::logic::is_sequential_kind(g.kind)) {
      continue;
    }
    ++n_before;
    auto r = wengine::logic::resolve_gate(after, barrier_gate_name(before, s));
    assert(r.ok());
    const wengine::logic::Gate &h = after.gates[r.value];
    assert(h.live && h.kind == g.kind);
    assert(h.input_count == g.input_count);
    assert(h.ignored_mask == g.ignored_mask);
    assert(h.ignore_output == g.ignore_output);
    assert(h.ignore_nq_output == g.ignore_nq_output);
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      const std::uint16_t d0 = g.drivers[i];
      const std::uint16_t d1 = h.drivers[i];
      const std::size_t f0 = static_cast<std::size_t>(s) * wengine::logic::kMaxInputs + i;
      const std::size_t f1 = static_cast<std::size_t>(r.value) * wengine::logic::kMaxInputs + i;
      if (d0 == wengine::logic::kNoDriver) {
        assert(d1 == wengine::logic::kNoDriver);
        continue;
      }
      assert(d1 != wengine::logic::kNoDriver);
      assert(after.gates[d1].live);
      if (check == SeqDriverCheck::kStrict) {
        assert(barrier_gate_name(after, d1) == barrier_gate_name(before, d0));
      }
      assert(after.driver_inv[f1] == before.driver_inv[f0]); // Q/NQ polarity
    }
  }
  assert(n_before > 0);
  assert(n_after == n_before);
}

// Oracle: every sequential gate in `before` resolves by display name in
// `after` with identical kind, pin count, flags, and logical per-pin
// drivers (same source gate by name, same Q/NQ polarity); no sequential
// gate appears or disappears.
void assert_seq_sets_equal(const wengine::logic::Netlist &before, const wengine::logic::Netlist &after) {
  assert_seq_equal_with_check(before, after, SeqDriverCheck::kStrict);
}

// Oracle: same as above but without the driver-name check, for O2's fresh
// build where upstream combinational glue is converted and renamed.
void assert_seq_shape_equal(const wengine::logic::Netlist &before, const wengine::logic::Netlist &after) {
  assert_seq_equal_with_check(before, after, SeqDriverCheck::kShapeOnly);
}

// Oracle: for each sequential output pin, the same number of consumer pins
// read it with the same polarity before and after. Name-agnostic (holds
// for O2's fresh build where combinational consumers are renamed); the
// 10-step run-parity check is the functional backstop.
void assert_seq_fanout_equal(const wengine::logic::Netlist &before, const wengine::logic::Netlist &after) {
  for (std::uint16_t s = 0; s < wengine::logic::kMaxGates; ++s) {
    const wengine::logic::Gate &g = before.gates[s];
    if (!g.live || !wengine::logic::is_sequential_kind(g.kind)) {
      continue;
    }
    auto r = wengine::logic::resolve_gate(after, barrier_gate_name(before, s));
    assert(r.ok());
    for (std::uint8_t pol = 0; pol <= 1; ++pol) {
      std::size_t c0 = 0;
      std::size_t c1 = 0;
      for (std::uint16_t u = 0; u < wengine::logic::kMaxGates; ++u) {
        const wengine::logic::Gate &gu = before.gates[u];
        if (!gu.live) {
          continue;
        }
        for (std::uint8_t i = 0; i < gu.input_count; ++i) {
          if (gu.drivers[i] == s &&
              before.driver_inv[static_cast<std::size_t>(u) * wengine::logic::kMaxInputs + i] == pol) {
            ++c0;
          }
        }
        const wengine::logic::Gate &hu = after.gates[u];
        if (!hu.live) {
          continue;
        }
        for (std::uint8_t i = 0; i < hu.input_count; ++i) {
          // Map the after-side driver slot back through its display name:
          // sequential names are stable, combinational slots are not.
          if (hu.drivers[i] >= wengine::logic::kMaxGates || !after.gates[hu.drivers[i]].live) {
            continue;
          }
          if (barrier_gate_name(after, hu.drivers[i]) != barrier_gate_name(before, s)) {
            continue;
          }
          if (after.driver_inv[static_cast<std::size_t>(u) * wengine::logic::kMaxInputs + i] == pol) {
            ++c1;
          }
        }
      }
      assert(c0 == c1);
    }
  }
}

// Oracle: fixed-step runs of two nets from power-on agree on every stored
// flop value at every step. `stim` holds steps*nin_a row-major block inputs
// for `a`; `proj` maps each block input of `b` onto the block input of `a`
// it replicates (O2 duplicates dangling pins for inversion, so `b` may
// expose more pins than `a` — the same projection idea as check_equiv_proj).
void assert_seq_run_parity_proj(const wengine::logic::Netlist &a, const wengine::logic::Netlist &b, std::uint16_t steps,
                               const std::vector<wengine::logic::LogicValue> &stim, const std::vector<std::size_t> &proj) {
  const std::size_t nin_a = wengine::logic::block_input_count(a);
  const std::size_t nin_b = wengine::logic::block_input_count(b);
  assert(proj.size() == nin_b);
  assert(stim.size() == static_cast<std::size_t>(steps) * nin_a);
  for (std::size_t j = 0; j < nin_b; ++j) {
    assert(proj[j] < nin_a);
  }
  wengine::logic::Netlist ca = a;
  wengine::logic::Netlist cb = b;
  wengine::logic::SeqState sa;
  wengine::logic::SeqState sb;
  std::vector<wengine::logic::LogicValue> row_a(nin_a == 0 ? 1 : nin_a, wengine::logic::LogicValue::kLow);
  std::vector<wengine::logic::LogicValue> row_b(nin_b == 0 ? 1 : nin_b, wengine::logic::LogicValue::kLow);
  for (std::uint16_t step = 0; step < steps; ++step) {
    for (std::size_t j = 0; j < nin_a; ++j) {
      row_a[j] = stim[static_cast<std::size_t>(step) * nin_a + j];
    }
    for (std::size_t j = 0; j < nin_b; ++j) {
      row_b[j] = row_a[proj[j]];
    }
    assert(wengine::logic::seq_step(ca, sa, std::span<const wengine::logic::LogicValue>(row_a.data(), nin_a)).ok());
    assert(wengine::logic::seq_step(cb, sb, std::span<const wengine::logic::LogicValue>(row_b.data(), nin_b)).ok());
    for (std::uint16_t s = 0; s < wengine::logic::kMaxGates; ++s) {
      const wengine::logic::Gate &g = a.gates[s];
      if (!g.live || !wengine::logic::is_sequential_kind(g.kind)) {
        continue;
      }
      auto r = wengine::logic::resolve_gate(cb, barrier_gate_name(a, s));
      assert(r.ok());
      assert(sa.q[s] == sb.q[r.value]);
    }
  }
}

void assert_seq_run_parity(const wengine::logic::Netlist &a, const wengine::logic::Netlist &b, std::uint16_t steps, const std::vector<wengine::logic::LogicValue> &stim) {
  const std::size_t nin = wengine::logic::block_input_count(a);
  assert(wengine::logic::block_input_count(b) == nin);
  std::vector<std::size_t> ident(nin, 0);
  for (std::size_t j = 0; j < nin; ++j) {
    ident[j] = j;
  }
  assert_seq_run_parity_proj(a, b, steps, stim, ident);
}

// All-zero projection: every block input of `b` replicates input 0 of `a`
// (single-source nets at O2, where duplication only clones one signal).
std::vector<std::size_t> barrier_proj_all_zero(const wengine::logic::Netlist &b) {
  const std::size_t nin_b = wengine::logic::block_input_count(b);
  return std::vector<std::size_t>(nin_b, 0);
}

