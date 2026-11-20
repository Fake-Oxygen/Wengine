// test_logic_shared: allowlisted multi-driver nets (011) — engine seam
// (mark lifecycle, span resolution, contention counting) through CLI verbs
// to YAML round-trip. Links wengine_cli + wengine_logic_yaml to prove the
// seam-to-verb-to-artifact chain.
#include "wengine/logic/block_interface.hpp"
#include "wengine/logic/eval.hpp"
#include "wengine/logic/netlist.hpp"
#include "wengine/logic/optimize.hpp"
#include "wengine/logic/seq_state.hpp"
#include "wengine/logic/seq_step.hpp"
#include "wengine/logic/types.hpp"
#include "wengine/logic/yaml_store.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>

using namespace wengine;

namespace {

// Foundational (011 Phase 2): fresh state is unmarked everywhere and the
// diagnostic channel starts at zero — the additive-storage no-op.
void test_fresh_state_unmarked() {
  logic::Netlist nl{};
  for (std::size_t f = 0; f < logic::kMaxGates * logic::kMaxInputs; ++f) {
    assert(!logic::is_shared_flat(nl, f));
    assert(logic::shared_extra_flat(nl, f) == 0);
  }
  logic::SolveDiag diag{};
  for (std::size_t f = 0; f < logic::kMaxGates * logic::kMaxInputs; ++f) {
    assert(diag.per_net[f] == 0);
  }
}

// US1 (011): unmarked occupied input refuses a second driver loudly with
// the netlist byte-identical — today's rule, never a silent overwrite.
void test_unmarked_second_driver_refused() {
  logic::Netlist nl{};
  const auto a1 = logic::add_gate(nl, logic::GateKind::kAnd, 2);
  const auto a2 = logic::add_gate(nl, logic::GateKind::kAnd, 2);
  const auto o = logic::add_gate(nl, logic::GateKind::kOr, 2);
  assert(a1.ok() && a2.ok() && o.ok());
  assert(logic::wire(nl, a1.value, o.value, 0).ok());
  const logic::Netlist before = nl;
  assert(!logic::is_shared_input(nl, o.value, 0));
  assert(!logic::wire(nl, a2.value, o.value, 0).ok());
  assert(nl == before);
  // Marking then wiring the same driver succeeds; count is 2.
  assert(logic::mark_shared(nl, o.value, 0).ok());
  assert(logic::is_shared_input(nl, o.value, 0));
  assert(logic::wire(nl, a2.value, o.value, 0).ok());
  assert(logic::shared_attachment_count(nl, o.value, 0) == 2);
}

// US1 (011): mark lifecycle — idempotent mark, pre-marking empty inputs,
// unshare refused while >= 2 attached, allowed at <= 1.
void test_mark_unshare_lifecycle() {
  logic::Netlist nl{};
  const auto a1 = logic::add_gate(nl, logic::GateKind::kAnd, 2);
  const auto a2 = logic::add_gate(nl, logic::GateKind::kAnd, 2);
  const auto o = logic::add_gate(nl, logic::GateKind::kOr, 2);
  assert(a1.ok() && a2.ok() && o.ok());
  // Pre-marking an unoccupied input is legal; zero attachments held.
  assert(logic::mark_shared(nl, o.value, 0).ok());
  assert(logic::shared_attachment_count(nl, o.value, 0) == 0);
  // Marking twice is idempotent.
  assert(logic::mark_shared(nl, o.value, 0).ok());
  assert(logic::wire(nl, a1.value, o.value, 0).ok());
  assert(logic::wire(nl, a2.value, o.value, 0).ok());
  // Unshare with two attachments fails loudly.
  assert(!logic::unshare_input(nl, o.value, 0).ok());
  assert(logic::is_shared_input(nl, o.value, 0));
  // Unwiring down to one releases the lifecycle; unshare then succeeds,
  // and the plain single-driver rule is back in force.
  assert(logic::unwire(nl, o.value, 0).ok());
  assert(logic::shared_attachment_count(nl, o.value, 0) == 1);
  assert(logic::unshare_input(nl, o.value, 0).ok());
  assert(!logic::is_shared_input(nl, o.value, 0));
  const logic::Netlist before = nl;
  assert(!logic::wire(nl, a2.value, o.value, 0).ok());
  assert(nl == before);
  // Unsharing an unmarked input fails loudly.
  assert(!logic::unshare_input(nl, o.value, 0).ok());
}

// US1 (011): the driver cap is loud, never silently truncated.
void test_shared_cap_loud() {
  logic::Netlist nl{};
  const auto o = logic::add_gate(nl, logic::GateKind::kOr, 2);
  assert(o.ok());
  assert(logic::mark_shared(nl, o.value, 0).ok());
  std::uint16_t src[5] = {};
  for (int i = 0; i < 5; ++i) {
    const auto b = logic::add_gate(nl, logic::GateKind::kBuf, 1);
    assert(b.ok());
    src[static_cast<std::size_t>(i)] = b.value;
  }
  for (int i = 0; i < 4; ++i) {
    assert(logic::wire(nl, src[static_cast<std::size_t>(i)], o.value, 0).ok());
  }
  assert(logic::shared_attachment_count(nl, o.value, 0) == 4);
  const logic::Netlist before = nl;
  assert(!logic::wire(nl, src[4], o.value, 0).ok());
  assert(nl == before);
  assert(logic::shared_attachment_count(nl, o.value, 0) == 4);
}

// US1 (011): marks never apply to tied inputs; ties never join a span.
void test_mark_refused_on_tied() {
  logic::Netlist nl{};
  const auto a = logic::add_gate(nl, logic::GateKind::kAnd, 2);
  const auto b = logic::add_gate(nl, logic::GateKind::kAnd, 2);
  assert(a.ok() && b.ok());
  assert(logic::tie_text(nl, "AND1.1", "AND2.1").ok());
  assert(!logic::mark_shared(nl, a.value, 0).ok());
  assert(!logic::is_shared_input(nl, a.value, 0));
}

// US3 (011) test rig: two NOTs share OR1.1, a BUF drives OR1.2.
struct BusRig {
  logic::Netlist nl{};
  std::uint16_t n1 = 0;
  std::uint16_t n2 = 0;
  std::uint16_t c = 0;
  std::uint16_t o = 0;
  std::size_t flat = 0;
};

BusRig make_bus() {
  BusRig r{};
  const auto n1 = logic::add_gate(r.nl, logic::GateKind::kNot, 1);
  const auto n2 = logic::add_gate(r.nl, logic::GateKind::kNot, 1);
  const auto c = logic::add_gate(r.nl, logic::GateKind::kBuf, 1);
  const auto o = logic::add_gate(r.nl, logic::GateKind::kOr, 2);
  assert(n1.ok() && n2.ok() && c.ok() && o.ok());
  assert(logic::wire(r.nl, n1.value, o.value, 0).ok());
  assert(logic::mark_shared(r.nl, o.value, 0).ok());
  assert(logic::wire(r.nl, n2.value, o.value, 0).ok());
  assert(logic::wire(r.nl, c.value, o.value, 1).ok());
  r.n1 = n1.value;
  r.n2 = n2.value;
  r.c = c.value;
  r.o = o.value;
  r.flat = logic::tie_flat(o.value, 0);
  return r;
}

// US3 (011): every low-vs-high contended resolution increments the
// per-net count by exactly one; agreeing resolutions leave it unchanged.
void test_contention_counted() {
  BusRig r = make_bus();
  logic::SolveDiag diag{};
  const logic::LogicValue agree[3] = {logic::LogicValue::kLow, logic::LogicValue::kLow,
                                      logic::LogicValue::kLow};
  logic::LogicValue out[1] = {logic::LogicValue::kLow};
  const std::span<const logic::LogicValue> ain(agree, 3);
  const std::span<logic::LogicValue> aout(out, 1);
  assert(logic::solve(r.nl, ain, aout, &diag).ok());
  assert(aout[0] == logic::LogicValue::kHigh); // {H,H} consensus
  assert(diag.per_net[r.flat] == 0);
  const logic::LogicValue fight[3] = {logic::LogicValue::kLow, logic::LogicValue::kHigh,
                                      logic::LogicValue::kLow};
  const std::span<const logic::LogicValue> fin(fight, 3);
  for (int k = 0; k < 3; ++k) {
    assert(!logic::solve(r.nl, fin, aout, &diag).ok()); // X at the consumer, loud
  }
  assert(diag.per_net[r.flat] == 3);
  assert(logic::solve(r.nl, ain, aout, &diag).ok());
  assert(diag.per_net[r.flat] == 3); // agreeing resolutions never increment
  // Null channel: identical value streams, no counting.
  assert(logic::solve(r.nl, ain, aout, nullptr).ok());
  assert(aout[0] == logic::LogicValue::kHigh);
}

// US3 (011): identical runs give identical count sequences; counts are
// session diagnostics — saves are byte-identical with or without
// contention history.
void test_contention_deterministic_and_unsaved() {
  BusRig r = make_bus();
  std::array<logic::LogicValue, logic::kMaxGates> inits{};
  inits.fill(logic::LogicValue::kLow);
  std::string before;
  logic::yaml::save_text(r.nl, inits, before);
  const logic::LogicValue fight[3] = {logic::LogicValue::kLow, logic::LogicValue::kHigh,
                                      logic::LogicValue::kLow};
  const std::span<const logic::LogicValue> fin(fight, 3);
  logic::LogicValue out[1] = {logic::LogicValue::kLow};
  const std::span<logic::LogicValue> aout(out, 1);
  logic::SolveDiag d1{};
  assert(!logic::solve(r.nl, fin, aout, &d1).ok());
  assert(!logic::solve(r.nl, fin, aout, &d1).ok());
  assert(d1.per_net[r.flat] == 2);
  std::string after;
  logic::yaml::save_text(r.nl, inits, after);
  assert(before == after);
  BusRig r2 = make_bus();
  logic::SolveDiag d2{};
  assert(!logic::solve(r2.nl, fin, aout, &d2).ok());
  assert(!logic::solve(r2.nl, fin, aout, &d2).ok());
  assert(d2.per_net[r2.flat] == d1.per_net[r.flat]);
}

// 011 US5: O0/O1 succeed with shared-feeding gates frozen (a bypassable
// BUF driving a shared input survives); resolution results unchanged;
// optimize-of-optimize idempotent.
void test_optimizer_freezes_shared() {
  logic::Netlist nl{};
  const auto n1 = logic::add_gate(nl, logic::GateKind::kNot, 1);
  const auto n2 = logic::add_gate(nl, logic::GateKind::kNot, 1);
  const auto b = logic::add_gate(nl, logic::GateKind::kBuf, 1);
  const auto o = logic::add_gate(nl, logic::GateKind::kOr, 2);
  assert(n1.ok() && n2.ok() && b.ok() && o.ok());
  assert(logic::wire(nl, n1.value, b.value, 0).ok());
  assert(logic::wire(nl, b.value, o.value, 0).ok());
  assert(logic::mark_shared(nl, o.value, 0).ok());
  assert(logic::wire(nl, n2.value, o.value, 0).ok());
  for (int level = 0; level <= 1; ++level) {
    logic::OptimizeOptions opts{};
    opts.level = (level == 0) ? logic::OptLevel::kO0 : logic::OptLevel::kO1;
    auto r = logic::optimize(nl, opts);
    assert(r.ok());
    logic::Netlist opt = r.value;
    // The shared-feeding BUF survives every pass (frozen, never
    // bypassed/rewired); marks and attachments intact.
    assert(opt.gates[b.value].live);
    assert(logic::is_shared_input(opt, o.value, 0));
    assert(logic::shared_attachment_count(opt, o.value, 0) == 2);
    // Observable resolution results never change: all 4 input combos
    // agree pre/post (ok-ness and outputs), and a second optimize is
    // idempotent.
    for (int row = 0; row < 4; ++row) {
      const logic::LogicValue in[2] = {(row & 1) != 0 ? logic::LogicValue::kHigh : logic::LogicValue::kLow,
                                       (row & 2) != 0 ? logic::LogicValue::kHigh : logic::LogicValue::kLow};
      logic::LogicValue want[1] = {logic::LogicValue::kLow};
      logic::LogicValue got[1] = {logic::LogicValue::kLow};
      const bool wok =
          logic::solve(nl, std::span<const logic::LogicValue>(in, 2), std::span<logic::LogicValue>(want, 1)).ok();
      const bool gok = logic::solve(opt, std::span<const logic::LogicValue>(in, 2),
                                    std::span<logic::LogicValue>(got, 1))
                           .ok();
      assert(wok == gok);
      if (wok) {
        assert(want[0] == got[0]);
      }
    }
    auto r2 = logic::optimize(opt, opts);
    assert(r2.ok());
    assert(r2.value == opt);
  }
}

// 011 US5: O2 refuses loudly on shared netlists, input untouched; the
// refusal names shared nets.
void test_optimizer_o2_refuses_shared() {
  BusRig r = make_bus();
  const logic::Netlist before = r.nl;
  logic::OptimizeOptions opts{};
  opts.level = logic::OptLevel::kO2;
  auto res = logic::optimize(r.nl, opts);
  assert(!res.ok());
  assert(res.error.find("shared") != std::string_view::npos);
  assert(r.nl == before); // pure: input never modified
}
// 011 US3 (T035): the step path attaches the same channel — phase-1 and
// snapshot resolutions count exactly like solve.
void test_step_counts_shared() {
  // (a) Comb-fed shared bus observed by a flop: contended bus fails the
  // step at phase-1 after counting once; agreeing steps count nothing.
  BusRig r = make_bus();
  const auto d = logic::add_gate(r.nl, logic::GateKind::kDff, 2);
  assert(d.ok());
  assert(logic::wire(r.nl, r.o, d.value, 0).ok()); // OR.Y -> DFF.D
  // Block inputs: N1.1, N2.1, C.1, DFF.CLK. Output: DFF.Q.
  assert(logic::block_input_count(r.nl) == 4);
  assert(logic::block_output_count(r.nl) == 1);
  logic::SeqState st{};
  logic::init_seq_state(st, r.nl);
  logic::SolveDiag diag{};
  const logic::LogicValue agree[4] = {logic::LogicValue::kLow, logic::LogicValue::kLow,
                                      logic::LogicValue::kLow, logic::LogicValue::kLow};
  logic::LogicValue out[1] = {logic::LogicValue::kLow};
  assert(logic::seq_step_filtered(r.nl, st, std::span<const logic::LogicValue>(agree, 4),
                                  std::span<logic::LogicValue>(out, 1), nullptr, 0, &diag)
             .ok());
  assert(diag.per_net[r.flat] == 0);
  const logic::LogicValue fight[4] = {logic::LogicValue::kLow, logic::LogicValue::kHigh,
                                      logic::LogicValue::kLow, logic::LogicValue::kLow};
  assert(!logic::seq_step_filtered(r.nl, st, std::span<const logic::LogicValue>(fight, 4),
                                    std::span<logic::LogicValue>(out, 1), nullptr, 0, &diag)
              .ok());
  assert(diag.per_net[r.flat] == 1); // phase-1 resolution counted, then loud
  assert(st.q[d.value] == logic::LogicValue::kHigh); // failed step: state untouched (H from the agreeing step)
  // (b) Shared input directly on a flop D pin: the snapshot site counts.
  logic::Netlist nl2{};
  const auto n1 = logic::add_gate(nl2, logic::GateKind::kNot, 1);
  const auto n2 = logic::add_gate(nl2, logic::GateKind::kNot, 1);
  const auto f = logic::add_gate(nl2, logic::GateKind::kDff, 2);
  assert(n1.ok() && n2.ok() && f.ok());
  assert(logic::mark_shared(nl2, f.value, 0).ok());
  assert(logic::wire(nl2, n1.value, f.value, 0).ok());
  assert(logic::wire(nl2, n2.value, f.value, 0).ok());
  assert(logic::block_input_count(nl2) == 3); // N1.1, N2.1, DFF.CLK
  logic::SeqState st2{};
  logic::init_seq_state(st2, nl2);
  logic::SolveDiag diag2{};
  const std::size_t dflat = logic::tie_flat(f.value, 0);
  const logic::LogicValue ok_in[3] = {logic::LogicValue::kLow, logic::LogicValue::kLow,
                                      logic::LogicValue::kLow};
  logic::LogicValue out2[1] = {logic::LogicValue::kLow};
  assert(logic::seq_step_filtered(nl2, st2, std::span<const logic::LogicValue>(ok_in, 3),
                                  std::span<logic::LogicValue>(out2, 1), nullptr, 0, &diag2)
             .ok());
  assert(diag2.per_net[dflat] == 0);
  assert(st2.q[f.value] == logic::LogicValue::kHigh); // D=H captured
  const logic::LogicValue bad_in[3] = {logic::LogicValue::kLow, logic::LogicValue::kHigh,
                                       logic::LogicValue::kLow};
  assert(!logic::seq_step_filtered(nl2, st2, std::span<const logic::LogicValue>(bad_in, 3),
                                    std::span<logic::LogicValue>(out2, 1), nullptr, 0, &diag2)
              .ok());
  assert(diag2.per_net[dflat] == 1); // snapshot resolution counted, then loud
}

// 011 US2 (T039): through-seam multisets up to the cap — 3- and
// 4-attachment spans resolve per the reference table, including mixed
// polarity; attachment order never affects values or counts.
void test_shared_span_wide() {
  logic::Netlist nl{};
  const auto a = logic::add_gate(nl, logic::GateKind::kNot, 1);
  const auto b = logic::add_gate(nl, logic::GateKind::kNot, 1);
  const auto c = logic::add_gate(nl, logic::GateKind::kNot, 1);
  const auto d = logic::add_gate(nl, logic::GateKind::kNot, 1);
  const auto o = logic::add_gate(nl, logic::GateKind::kOr, 2);
  assert(a.ok() && b.ok() && c.ok() && d.ok() && o.ok());
  assert(logic::wire(nl, a.value, o.value, 0).ok());
  assert(logic::mark_shared(nl, o.value, 0).ok());
  assert(logic::wire(nl, b.value, o.value, 0).ok());
  assert(logic::wire(nl, c.value, o.value, 0).ok());
  assert(logic::wire(nl, d.value, o.value, 0).ok());
  assert(logic::shared_attachment_count(nl, o.value, 0) == 4);
  logic::LogicValue gate_out[logic::kMaxGates];
  logic::LogicValue resolved[logic::kMaxGates * logic::kMaxInputs] = {};
  for (std::size_t s = 0; s < logic::kMaxGates; ++s) {
    gate_out[s] = logic::LogicValue::kLow;
  }
  logic::LogicValue out = logic::LogicValue::kLow;
  // {L,L,L,L} -> L; {H,H,H,H} -> H.
  assert(logic::resolve_input(nl, o.value, 0, gate_out, resolved, out) && out == logic::LogicValue::kLow);
  gate_out[a.value] = gate_out[b.value] = gate_out[c.value] = gate_out[d.value] = logic::LogicValue::kHigh;
  assert(logic::resolve_input(nl, o.value, 0, gate_out, resolved, out) && out == logic::LogicValue::kHigh);
  // Any low+high mix -> X, regardless of position.
  gate_out[c.value] = logic::LogicValue::kLow;
  assert(!logic::resolve_input(nl, o.value, 0, gate_out, resolved, out));
  gate_out[c.value] = logic::LogicValue::kHigh;
  gate_out[d.value] = logic::LogicValue::kLow;
  assert(!logic::resolve_input(nl, o.value, 0, gate_out, resolved, out));
  // Drop to 3 attachments: {H,H,H} -> H; {H,H,L} -> X.
  assert(logic::unwire(nl, o.value, 0).ok()); // promotes b; span {H,H,H}
  gate_out[b.value] = gate_out[c.value] = gate_out[d.value] = logic::LogicValue::kHigh;
  assert(logic::resolve_input(nl, o.value, 0, gate_out, resolved, out) && out == logic::LogicValue::kHigh);
  gate_out[d.value] = logic::LogicValue::kLow;
  assert(!logic::resolve_input(nl, o.value, 0, gate_out, resolved, out));
  // Counts are order-independent: same value multiset attached in
  // different orders contends identically.
  logic::Netlist fwd{};
  const auto f1 = logic::add_gate(fwd, logic::GateKind::kNot, 1);
  const auto f2 = logic::add_gate(fwd, logic::GateKind::kNot, 1);
  const auto f3 = logic::add_gate(fwd, logic::GateKind::kNot, 1);
  const auto fb = logic::add_gate(fwd, logic::GateKind::kBuf, 1);
  const auto fo = logic::add_gate(fwd, logic::GateKind::kOr, 2);
  assert(f1.ok() && f2.ok() && f3.ok() && fb.ok() && fo.ok());
  assert(logic::wire(fwd, f1.value, fo.value, 0).ok());
  assert(logic::mark_shared(fwd, fo.value, 0).ok());
  assert(logic::wire(fwd, f2.value, fo.value, 0).ok());
  assert(logic::wire(fwd, f3.value, fo.value, 0).ok());
  assert(logic::wire(fwd, fb.value, fo.value, 1).ok());
  logic::Netlist rev{};
  const auto r1 = logic::add_gate(rev, logic::GateKind::kNot, 1);
  const auto r2 = logic::add_gate(rev, logic::GateKind::kNot, 1);
  const auto r3 = logic::add_gate(rev, logic::GateKind::kNot, 1);
  const auto rb = logic::add_gate(rev, logic::GateKind::kBuf, 1);
  const auto ro = logic::add_gate(rev, logic::GateKind::kOr, 2);
  assert(r1.ok() && r2.ok() && r3.ok() && rb.ok() && ro.ok());
  assert(logic::wire(rev, r3.value, ro.value, 0).ok());
  assert(logic::mark_shared(rev, ro.value, 0).ok());
  assert(logic::wire(rev, r2.value, ro.value, 0).ok());
  assert(logic::wire(rev, r1.value, ro.value, 0).ok());
  assert(logic::wire(rev, rb.value, ro.value, 1).ok());
  // Same driver-value multiset {H, L, H} in both orders.
  const logic::LogicValue in[4] = {logic::LogicValue::kLow, logic::LogicValue::kHigh,
                                   logic::LogicValue::kLow, logic::LogicValue::kLow};
  logic::LogicValue trash[1] = {logic::LogicValue::kLow};
  logic::SolveDiag df{};
  logic::SolveDiag dr{};
  assert(!logic::solve(fwd, std::span<const logic::LogicValue>(in, 4),
                       std::span<logic::LogicValue>(trash, 1), &df)
              .ok());
  assert(!logic::solve(rev, std::span<const logic::LogicValue>(in, 4),
                       std::span<logic::LogicValue>(trash, 1), &dr)
              .ok());
  const std::size_t fflat = logic::tie_flat(fo.value, 0);
  const std::size_t rflat = logic::tie_flat(ro.value, 0);
  assert(df.per_net[fflat] == 1 && dr.per_net[rflat] == 1);
}

// US1 (011): a cycle closing through an extra attachment fails the same
// flop-aware check as any wire — extras are driver-graph edges.
void test_shared_cycle_rejected() {
  logic::Netlist nl{};
  const auto a = logic::add_gate(nl, logic::GateKind::kAnd, 2);
  const auto b = logic::add_gate(nl, logic::GateKind::kOr, 2);
  const auto c = logic::add_gate(nl, logic::GateKind::kAnd, 2);
  assert(a.ok() && b.ok() && c.ok());
  assert(logic::wire(nl, c.value, b.value, 0).ok());
  assert(logic::mark_shared(nl, b.value, 1).ok());
  assert(logic::wire(nl, a.value, b.value, 1).ok());
  // B resolves through A (extra edge A -> B.2); wiring B -> A.1 would
  // close a combinational cycle and must fail loudly, netlist untouched.
  const logic::Netlist before = nl;
  assert(!logic::wire(nl, b.value, a.value, 0).ok());
  assert(nl == before);
}
// extra removal compacts left, fanout releases polarity-aware.

// US1 (011): named-edge removal — primary unwire promotes extra 0,
void test_named_edge_removal() {
  logic::Netlist nl{};
  const auto o = logic::add_gate(nl, logic::GateKind::kOr, 2);
  assert(o.ok());
  std::uint16_t src[3] = {};
  for (int i = 0; i < 3; ++i) {
    const auto x = logic::add_gate(nl, logic::GateKind::kAnd, 2);
    assert(x.ok());
    src[static_cast<std::size_t>(i)] = x.value;
  }
  assert(logic::mark_shared(nl, o.value, 0).ok());
  for (int i = 0; i < 3; ++i) {
    assert(logic::wire(nl, src[static_cast<std::size_t>(i)], o.value, 0).ok());
  }
  assert(logic::shared_attachment_count(nl, o.value, 0) == 3);
  // Removing a middle extra compacts left; the remaining drivers resolve
  // unchanged (primary untouched).
  assert(logic::unwire_driver(nl, src[1], o.value, 0).ok());
  assert(logic::shared_attachment_count(nl, o.value, 0) == 2);
  assert(nl.gates[o.value].drivers[0] == src[0]);
  // Removing the primary promotes extra 0 into the primary slot.
  assert(logic::unwire(nl, o.value, 0).ok());
  assert(nl.gates[o.value].drivers[0] == src[2]);
  assert(logic::shared_attachment_count(nl, o.value, 0) == 1);
  assert(nl.gates[src[0]].output_fanout == 0);
  assert(nl.gates[src[2]].output_fanout == 1);
}

// US1 (011): removing a gate releases exactly its attachments wherever
// they sit (primary or extra) — no dangling entries survive.
void test_remove_gate_sweeps_extras() {
  logic::Netlist nl{};
  const auto o = logic::add_gate(nl, logic::GateKind::kOr, 2);
  assert(o.ok());
  const auto a1 = logic::add_gate(nl, logic::GateKind::kAnd, 2);
  const auto a2 = logic::add_gate(nl, logic::GateKind::kAnd, 2);
  assert(a1.ok() && a2.ok());
  assert(logic::mark_shared(nl, o.value, 0).ok());
  assert(logic::wire(nl, a1.value, o.value, 0).ok());
  assert(logic::wire(nl, a2.value, o.value, 0).ok());
  // Removing the extra driver compacts the span; removing the primary
  // afterwards leaves a clean marked-empty input.
  assert(logic::remove_gate(nl, a2.value).ok());
  assert(logic::shared_attachment_count(nl, o.value, 0) == 1);
  assert(nl.gates[o.value].drivers[0] == a1.value);
  assert(logic::remove_gate(nl, a1.value).ok());
  assert(logic::shared_attachment_count(nl, o.value, 0) == 0);
  assert(logic::is_shared_input(nl, o.value, 0));
  // The mark dies with its own gate's slot.
  assert(logic::remove_gate(nl, o.value).ok());
}

} // namespace

int main() {
  test_fresh_state_unmarked();
  test_unmarked_second_driver_refused();
  test_mark_unshare_lifecycle();
  test_shared_cap_loud();
  test_mark_refused_on_tied();
  test_shared_cycle_rejected();
  test_contention_counted();
  test_contention_deterministic_and_unsaved();
  test_step_counts_shared();
  test_shared_span_wide();
  test_optimizer_freezes_shared();
  test_optimizer_o2_refuses_shared();
  test_named_edge_removal();
  test_remove_gate_sweeps_extras();
  std::cout << "test_logic_shared: OK\n";
  return 0;
}
