// 014 four-state parity seam: the value tables in four_state.hpp vs the
// zero-delay 2-state evaluator in eval.hpp.
//
// D10 forbids unifying the two evaluators (the fast path never branches
// on mode, never reads a delay field, and eval.hpp gains zero code for
// this), so their agreement on clean Low/High nets is pinned here instead:
// every combinational kind, every valid fan-in, exhaustive bit patterns,
// plus the hand-derived 4-state goldens (dominant values, X/Z behavior,
// latch response) that have no 2-state counterpart. Reference: IEEE 1364
// gate-level semantics (dominant-value tables) plus the shipped funnel
// (Z transparent, X dominant/conflict).
//
// Include-weight pin: this TU includes only four_state.hpp + eval.hpp.
// four_state.hpp itself needs types.hpp alone — value-only consumers never
// drag in shared_net/tie_set/topo (previously forced via timing.hpp).

#include "wengine/logic/eval.hpp"
#include "wengine/logic/four_state.hpp"
#include "wengine/logic/types.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>

namespace {

using namespace wengine;
using wengine::logic::GateKind;
using wengine::logic::LogicValue;

constexpr LogicValue L = LogicValue::kLow;
constexpr LogicValue H = LogicValue::kHigh;
constexpr LogicValue X = LogicValue::kUnknown;
constexpr LogicValue Z = LogicValue::kHighZ;

// D10 seam: on clean Low/High nets the 4-state tables agree with the
// 2-state fast path exactly — every combinational kind, every valid
// fan-in, exhaustive patterns. TBUF included: asserted enable drives
// data, released drives Z, exactly like eval_gate.
void test_clean_parity_exhaustive() {
  constexpr GateKind kWide[] = {GateKind::kAnd, GateKind::kNand, GateKind::kOr,
                                GateKind::kNor, GateKind::kXor, GateKind::kXnor};
  LogicValue ins[logic::kMaxInputs];
  for (GateKind kind : kWide) {
    for (std::uint8_t n = 2; n <= logic::kMaxInputs; ++n) {
      const std::uint32_t rows = 1u << n;
      for (std::uint32_t r = 0; r < rows; ++r) {
        for (std::uint8_t i = 0; i < n; ++i) {
          ins[i] = ((r >> i) & 1u) != 0u ? H : L;
        }
        LogicValue got = L;
        assert(logic::timing_eval_gate(kind, ins, n, got));
        assert(got == logic::eval_gate(kind, ins, n));
      }
    }
  }
  for (GateKind kind : {GateKind::kNot, GateKind::kBuf}) {
    for (std::uint32_t r = 0; r < 2; ++r) {
      ins[0] = (r != 0u) ? H : L;
      LogicValue got = L;
      assert(logic::timing_eval_gate(kind, ins, 1, got));
      assert(got == logic::eval_gate(kind, ins, 1));
    }
  }
  for (std::uint32_t r = 0; r < 4; ++r) {
    ins[0] = ((r >> 0) & 1u) != 0u ? H : L;
    ins[1] = ((r >> 1) & 1u) != 0u ? H : L;
    LogicValue got = L;
    assert(logic::timing_eval_gate(GateKind::kTbuf, ins, 2, got));
    assert(got == logic::eval_gate(GateKind::kTbuf, ins, 2));
  }
}

// NQ read: agrees with apply_polarity on clean values; deliberately
// differs on X/HighZ (the trace shows a transient X on both pins while
// the zero-delay consumer folds everything non-High to Low).
void test_nq_read() {
  assert(logic::timing_nq_of(H) == L);
  assert(logic::timing_nq_of(L) == H);
  assert(logic::timing_nq_of(H) == logic::apply_polarity(H, 1));
  assert(logic::timing_nq_of(L) == logic::apply_polarity(L, 1));
  assert(logic::timing_nq_of(X) == X);
  assert(logic::timing_nq_of(Z) == Z);
}

// Hand-derived dominant-value goldens (IEEE 1364): the dominant input
// wins over X, X wins over everything else, and Z at a gate input reads
// as X. No 2-state counterpart — these pin the tables directly.
void test_dominant_tables() {
  LogicValue out = L;
  const LogicValue low_x[2] = {L, X};
  assert(logic::timing_eval_gate(GateKind::kAnd, low_x, 2, out) && out == L);
  assert(logic::timing_eval_gate(GateKind::kNand, low_x, 2, out) && out == H);
  const LogicValue high_x[2] = {H, X};
  assert(logic::timing_eval_gate(GateKind::kOr, high_x, 2, out) && out == H);
  assert(logic::timing_eval_gate(GateKind::kNor, high_x, 2, out) && out == L);
  const LogicValue high_xx[2] = {H, X};
  assert(logic::timing_eval_gate(GateKind::kAnd, high_xx, 2, out) && out == X);
  const LogicValue low_xx[2] = {L, X};
  assert(logic::timing_eval_gate(GateKind::kOr, low_xx, 2, out) && out == X);
  assert(logic::timing_eval_gate(GateKind::kXor, high_x, 2, out) && out == X);
  assert(logic::timing_eval_gate(GateKind::kXor, low_x, 2, out) && out == X);
  const LogicValue high_z[2] = {H, Z};
  assert(logic::timing_eval_gate(GateKind::kAnd, high_z, 2, out) && out == X);
  assert(logic::timing_eval_gate(GateKind::kXor, high_z, 2, out) && out == X);
  assert(logic::timing_eval_gate(GateKind::kNot, &high_z[1], 1, out) && out == X);
  assert(logic::timing_eval_gate(GateKind::kNot, &high_x[1], 1, out) && out == X);
  assert(logic::timing_eval_gate(GateKind::kBuf, &high_x[1], 1, out) && out == X);
  // TBUF: enabled passes X data through as X; unknown enable renders X;
  // released drives Z regardless of data.
  const LogicValue x_enabled[2] = {X, H};
  assert(logic::timing_eval_gate(GateKind::kTbuf, x_enabled, 2, out) && out == X);
  const LogicValue z_enabled[2] = {Z, H};
  assert(logic::timing_eval_gate(GateKind::kTbuf, z_enabled, 2, out) && out == X);
  const LogicValue data_xen[2] = {H, X};
  assert(logic::timing_eval_gate(GateKind::kTbuf, data_xen, 2, out) && out == X);
  const LogicValue data_released[2] = {H, L};
  assert(logic::timing_eval_gate(GateKind::kTbuf, data_released, 2, out) && out == Z);
  const LogicValue x_released[2] = {X, L};
  assert(logic::timing_eval_gate(GateKind::kTbuf, x_released, 2, out) && out == Z);
}

// Level-element response goldens (contracts/timing-semantics.md):
// DLATCH tracks while enabled (X passes through), holds otherwise;
// SRLATCH S=R=1 renders X, S drives 1, R drives 0, S=R=0 holds, any
// other unknown combination renders X.
void test_latch_tables() {
  LogicValue out = L;
  assert(logic::timing_eval_latch(GateKind::kDlatch, H, H, L, out) && out == H);
  assert(logic::timing_eval_latch(GateKind::kDlatch, L, H, H, out) && out == L);
  assert(logic::timing_eval_latch(GateKind::kDlatch, X, H, L, out) && out == X);
  assert(logic::timing_eval_latch(GateKind::kDlatch, H, L, H, out) && out == H);
  assert(logic::timing_eval_latch(GateKind::kDlatch, H, X, H, out) && out == H);
  assert(logic::timing_eval_latch(GateKind::kDlatch, H, Z, L, out) && out == L);
  assert(logic::timing_eval_latch(GateKind::kSrlatch, H, H, L, out) && out == X);
  assert(logic::timing_eval_latch(GateKind::kSrlatch, H, L, L, out) && out == H);
  assert(logic::timing_eval_latch(GateKind::kSrlatch, L, H, H, out) && out == L);
  assert(logic::timing_eval_latch(GateKind::kSrlatch, L, L, H, out) && out == H);
  assert(logic::timing_eval_latch(GateKind::kSrlatch, L, L, L, out) && out == L);
  assert(logic::timing_eval_latch(GateKind::kSrlatch, X, L, H, out) && out == X);
  assert(logic::timing_eval_latch(GateKind::kSrlatch, L, X, H, out) && out == X);
  assert(logic::timing_eval_latch(GateKind::kSrlatch, Z, Z, H, out) && out == X);
  assert(!logic::timing_eval_latch(GateKind::kAnd, H, H, L, out));
  assert(!logic::timing_eval_latch(GateKind::kDff, H, L, L, out));
}

// Refusal surface: sequential kinds never evaluate as gates, empty
// fan-in and null spans fail loudly (callers keep their value).
void test_refusals() {
  LogicValue out = H;
  const LogicValue ins[2] = {H, H};
  for (GateKind kind : {GateKind::kDff, GateKind::kDlatch, GateKind::kJk, GateKind::kT, GateKind::kSrlatch}) {
    assert(!logic::timing_eval_gate(kind, ins, 2, out));
    assert(out == H);
  }
  assert(!logic::timing_eval_gate(GateKind::kAnd, nullptr, 1, out));
  assert(!logic::timing_eval_gate(GateKind::kNot, ins, 0, out));
  assert(!logic::timing_eval_gate(GateKind::kBuf, ins, 0, out));
  assert(!logic::timing_eval_gate(GateKind::kTbuf, ins, 1, out));
}

} // namespace

int main() {
  test_clean_parity_exhaustive();
  std::cout << "test_logic_four_state: parity OK\n";
  test_nq_read();
  std::cout << "test_logic_four_state: nq OK\n";
  test_dominant_tables();
  std::cout << "test_logic_four_state: tables OK\n";
  test_latch_tables();
  std::cout << "test_logic_four_state: latch OK\n";
  test_refusals();
  std::cout << "test_logic_four_state: OK\n";
  return 0;
}
