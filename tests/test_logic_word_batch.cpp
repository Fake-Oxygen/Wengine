// test_logic_word_batch: batch-vs-single parity corpus (017).
//
// Every word the batched entry emits must equal the single-slice
// path's word on the same slice, and both must agree with scalar
// `solve` row by row (contracts/parity-matrix.md). Bit-exact `==`
// throughout — no tolerances. Links wengine_core alone.
#include "wengine/logic/eval.hpp"
#include "wengine/logic/netlist.hpp"
#include "wengine/logic/topo.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <vector>

using namespace wengine;
using wengine::logic::GateKind;
using wengine::logic::LogicValue;
using wengine::logic::Netlist;
using wengine::logic::PinRef;
using wengine::logic::Slot;

namespace {

constexpr LogicValue kL = LogicValue::kLow;
constexpr LogicValue kH = LogicValue::kHigh;
constexpr std::size_t kW = logic::kWordBatchWidth;

Slot add_ok(Netlist &nl, GateKind kind, std::uint8_t fanin) {
  auto added = logic::add_gate(nl, kind, fanin);
  assert(added.ok());
  return added.value;
}

void wire_ok(Netlist &nl, Slot drv, Slot sink, std::uint8_t input) {
  assert(logic::wire(nl, drv, sink, input).ok());
}

PinRef pin(Slot s, std::uint8_t i) {
  return PinRef{.slot = s, .is_output = false, .input = i};
}

// Full-sweep agreement for one netlist: every full batch via
// eval_block_words, every slice via eval_block_word, every truth row
// via scalar solve — all three must agree, and a repeat batch pass
// must be bit-identical. Nets with fewer than W slices exercise the
// single-slice tail (the trailing partial batch must refuse loudly);
// the pool-backed overload is pinned on the first full batch.
void check_batch_parity(Netlist &nl) {
  const std::size_t nin = logic::block_input_count(nl);
  const std::size_t nout = logic::block_output_count(nl);
  const std::size_t n = logic::slot_count(nl);
  assert(nin <= 10); // full enumeration stays cheap (2^10 rows max)
  const std::size_t need = logic::bit_words_for_inputs(nin);
  assert(need == ((static_cast<std::size_t>(1ULL << nin) + 63) / 64));
  std::vector<std::uint64_t> scratch1(n == 0 ? 1 : n, 0);
  std::vector<std::uint64_t> single(nout == 0 ? 1 : nout, 0);
  std::vector<std::uint64_t> scratchW(n == 0 ? 1 : n * kW, 0);
  std::vector<std::uint64_t> batch(nout == 0 ? 1 : nout * kW, 0);
  const std::size_t nb = need / kW;
  const std::size_t tail = need % kW;
  for (std::size_t b = 0; b < nb; ++b) {
    const auto base = static_cast<std::uint64_t>(b * kW);
    assert(logic::eval_block_words(nl, base,
                                   std::span<std::uint64_t>(scratchW.data(), scratchW.size()),
                                   std::span<std::uint64_t>(batch.data(), nout * kW))
               .ok());
    if (b == 0) {
      // Pool-backed convenience overload agrees with caller scratch.
      std::vector<std::uint64_t> pooled(nout == 0 ? 1 : nout * kW, 0);
      assert(logic::eval_block_words(nl, base, std::span<std::uint64_t>(pooled.data(), nout * kW)).ok());
      for (std::size_t i = 0; i < nout * kW; ++i) {
        assert(pooled[i] == batch[i]);
      }
      // Repeat determinism: the same batch twice is bit-identical.
      std::vector<std::uint64_t> again(nout == 0 ? 1 : nout * kW, 0);
      assert(logic::eval_block_words(nl, base,
                                     std::span<std::uint64_t>(scratchW.data(), scratchW.size()),
                                     std::span<std::uint64_t>(again.data(), nout * kW))
                 .ok());
      for (std::size_t i = 0; i < nout * kW; ++i) {
        assert(again[i] == batch[i]);
      }
    }
    for (std::size_t w = 0; w < kW; ++w) {
      assert(logic::eval_block_word(nl, base + static_cast<std::uint64_t>(w),
                                    std::span<std::uint64_t>(scratch1.data(), scratch1.size()),
                                    std::span<std::uint64_t>(single.data(), nout))
                 .ok());
      for (std::size_t o = 0; o < nout; ++o) {
        assert(batch[o * kW + w] == single[o]);
      }
    }
  }
  // Tail slices (need is not a multiple of W, or need < W): singles
  // only — a trailing partial batch must refuse loudly (the range rule
  // applies to every lane).
  for (std::size_t w = 0; w < tail; ++w) {
    const auto idx = static_cast<std::uint64_t>(nb * kW + w);
    assert(logic::eval_block_word(nl, idx, std::span<std::uint64_t>(scratch1.data(), scratch1.size()),
                                  std::span<std::uint64_t>(single.data(), nout))
               .ok());
  }
  if (tail != 0) {
    assert(!logic::eval_block_words(nl, static_cast<std::uint64_t>(nb * kW),
                                    std::span<std::uint64_t>(scratchW.data(), scratchW.size()),
                                    std::span<std::uint64_t>(batch.data(), nout * kW))
                .ok());
  }
  // Scalar agreement on every truth row (batch lanes where covered,
  // single-slice tail lanes otherwise).
  const std::uint64_t rows = static_cast<std::uint64_t>(1ULL << nin);
  std::vector<LogicValue> scalar_in(nin, kL);
  std::vector<LogicValue> scalar_out(nout == 0 ? 1 : nout, kL);
  for (std::uint64_t r = 0; r < rows; ++r) {
    for (std::size_t j = 0; j < nin; ++j) {
      scalar_in[j] = ((((r >> j) & 1ULL) != 0ULL) ? kH : kL);
    }
    assert(logic::solve(nl, std::span<const LogicValue>(scalar_in.data(), nin),
                        std::span<LogicValue>(scalar_out.data(), nout))
               .ok());
    const std::size_t w = static_cast<std::size_t>(r / 64);
    const std::uint64_t b = r % 64;
    const std::size_t bidx = w / kW;
    const std::size_t lane = w % kW;
    for (std::size_t o = 0; o < nout; ++o) {
      std::uint64_t word = 0;
      if (bidx < nb) {
        // Re-evaluate the covering batch (scratch reuse is the point:
        // caller-owned buffers, no hidden state).
        assert(logic::eval_block_words(nl, static_cast<std::uint64_t>(bidx * kW),
                                       std::span<std::uint64_t>(scratchW.data(), scratchW.size()),
                                       std::span<std::uint64_t>(batch.data(), nout * kW))
                   .ok());
        word = batch[o * kW + lane];
      } else {
        assert(logic::eval_block_word(nl, static_cast<std::uint64_t>(w),
                                      std::span<std::uint64_t>(scratch1.data(), scratch1.size()),
                                      std::span<std::uint64_t>(single.data(), nout))
                   .ok());
        word = single[o];
      }
      const bool bit = (((word >> b) & 1ULL) != 0ULL);
      assert(bit == (scalar_out[o] == kH));
    }
  }
}

void test_all_kinds_fanin8() {
  // All 8 combinational kinds at fan-in 8 (256 rows, 4 words: exactly
  // one full batch) plus NOT/BUF single-input slices.
  const GateKind kinds[6] = {GateKind::kAnd, GateKind::kOr, GateKind::kNand,
                             GateKind::kNor, GateKind::kXor, GateKind::kXnor};
  for (std::size_t k = 0; k < 6; ++k) {
    Netlist nl;
    add_ok(nl, kinds[k], 8);
    check_batch_parity(nl);
  }
  Netlist nn;
  add_ok(nn, GateKind::kNot, 1);
  check_batch_parity(nn);
  Netlist nb;
  add_ok(nb, GateKind::kBuf, 1);
  check_batch_parity(nb);
}

void test_mixed_fanin_chain() {
  // Mixed fan-in chain AND2 -> OR3 -> XOR4 (7 block inputs, 2 words:
  // tail-only net — batch must refuse, singles + scalar agree).
  Netlist nl;
  const Slot a = add_ok(nl, GateKind::kAnd, 2);
  const Slot o = add_ok(nl, GateKind::kOr, 3);
  const Slot x = add_ok(nl, GateKind::kXor, 4);
  wire_ok(nl, a, o, 0);
  wire_ok(nl, o, x, 0);
  assert(logic::block_input_count(nl) == 7);
  check_batch_parity(nl);
}

void test_partial_word_masking() {
  // 1..7 block inputs: partial-word masking live on outputs. AND2 (2
  // inputs, 1 word), AND3 (3 inputs), and a 5-input cascade.
  {
    Netlist nl;
    add_ok(nl, GateKind::kAnd, 2);
    check_batch_parity(nl);
  }
  {
    Netlist nl;
    add_ok(nl, GateKind::kAnd, 3);
    check_batch_parity(nl);
  }
  {
    Netlist nl;
    const Slot a = add_ok(nl, GateKind::kAnd, 3);
    const Slot b = add_ok(nl, GateKind::kOr, 3);
    wire_ok(nl, a, b, 0);
    assert(logic::block_input_count(nl) == 5);
    check_batch_parity(nl);
  }
}

void test_multi_batch_and_tail() {
  // 8 inputs (4 words: exactly one batch), 9 inputs (8 words: two
  // batches), 10 inputs (16 words: four batches).
  {
    Netlist nl;
    add_ok(nl, GateKind::kAnd, 8);
    check_batch_parity(nl);
  }
  {
    Netlist nl;
    add_ok(nl, GateKind::kAnd, 8);
    add_ok(nl, GateKind::kNot, 1);
    assert(logic::block_input_count(nl) == 9);
    check_batch_parity(nl);
  }
  {
    Netlist nl;
    add_ok(nl, GateKind::kAnd, 8);
    add_ok(nl, GateKind::kBuf, 1);
    add_ok(nl, GateKind::kNot, 1);
    assert(logic::block_input_count(nl) == 10);
    check_batch_parity(nl);
  }
}

void test_ignored_and_ties() {
  // Ignored dangling inputs tie LOW in both paths.
  {
    Netlist nl;
    const Slot a = add_ok(nl, GateKind::kAnd, 3);
    const Slot b = add_ok(nl, GateKind::kBuf, 1);
    (void)b;
    assert(logic::ignore_pin(nl, pin(a, 1)).ok());
    assert(logic::ignore_pin(nl, pin(a, 2)).ok());
    assert(logic::block_input_count(nl) == 2);
    check_batch_parity(nl);
  }
  // Tied inputs share one leader column in both paths.
  {
    Netlist nl;
    const Slot a = add_ok(nl, GateKind::kAnd, 2);
    const Slot b = add_ok(nl, GateKind::kOr, 2);
    assert(logic::tie_pins(nl, pin(a, 0), pin(b, 0)).ok());
    assert(logic::block_input_count(nl) == 3);
    check_batch_parity(nl);
  }
}

void test_pulled_termination() {
  // Pull-up terminates to all-ones, pull-down to all-zero — in both
  // paths, without consuming block-input columns.
  {
    Netlist nl;
    const Slot a = add_ok(nl, GateKind::kAnd, 2);
    assert(logic::set_pull(nl, a, 0, 1).ok());
    assert(logic::block_input_count(nl) == 1);
    check_batch_parity(nl);
  }
  {
    Netlist nl;
    const Slot a = add_ok(nl, GateKind::kOr, 2);
    assert(logic::set_pull(nl, a, 0, 2).ok());
    assert(logic::block_input_count(nl) == 1);
    check_batch_parity(nl);
  }
}

void test_sequential_fallthrough() {
  // DFF present: word outputs follow the standing zero/NQ convention
  // in both paths (Q = 0 lanes, NQ exposed on explicit use).
  Netlist nl;
  const Slot d = add_ok(nl, GateKind::kDff, 2);
  const Slot n = add_ok(nl, GateKind::kNot, 1);
  wire_ok(nl, d, n, 0);
  check_batch_parity(nl);
}

void test_degenerate() {  // Empty netlist: the single slice evaluates; no full batch exists
  // (lanes 1..3 are out of range), so the batch entry refuses loudly.
  {
    Netlist nl;
    std::vector<std::uint64_t> scratch(1, 0);
    assert(logic::eval_block_word(nl, 0, std::span<std::uint64_t>(scratch.data(), scratch.size()),
                                  std::span<std::uint64_t>{})
               .ok());
    std::vector<std::uint64_t> scratchW(kW, 0);
    assert(!logic::eval_block_words(nl, 0, std::span<std::uint64_t>(scratchW.data(), scratchW.size()),
                                    std::span<std::uint64_t>{})
                .ok());
  }
  // No dangling outputs: outputs span is empty, batch succeeds (an
  // 8-input net enumerates exactly one full batch).
  {
    Netlist nl;
    const Slot a = add_ok(nl, GateKind::kAnd, 8);
    assert(logic::ignore_pin(nl, PinRef{.slot = a, .is_output = true, .input = 0}).ok());
    assert(logic::block_output_count(nl) == 0);
    const std::size_t nn = logic::slot_count(nl);
    std::vector<std::uint64_t> scratchW(nn * kW, 0);
    assert(logic::eval_block_words(nl, 0, std::span<std::uint64_t>(scratchW.data(), scratchW.size()),
                                   std::span<std::uint64_t>{})
               .ok());
  }
}

// Unbounded-slice net (32 inputs): no full truth exists; spot slices —
// including a large word_base — must agree batch-vs-single, and spot
// scalar rows must agree with both.
void check_spot_slice(Netlist &nl, std::uint64_t word_base) {
  const std::size_t n = logic::slot_count(nl);
  const std::size_t nout = logic::block_output_count(nl);
  std::vector<std::uint64_t> scratch1(n, 0);
  std::vector<std::uint64_t> single(nout, 0);
  std::vector<std::uint64_t> scratchW(n * kW, 0);
  std::vector<std::uint64_t> batch(nout * kW, 0);
  assert(logic::eval_block_words(nl, word_base, std::span<std::uint64_t>(scratchW.data(), scratchW.size()),
                                 std::span<std::uint64_t>(batch.data(), batch.size()))
             .ok());
  for (std::size_t w = 0; w < kW; ++w) {
    assert(logic::eval_block_word(nl, word_base + static_cast<std::uint64_t>(w),
                                  std::span<std::uint64_t>(scratch1.data(), scratch1.size()),
                                  std::span<std::uint64_t>(single.data(), single.size()))
               .ok());
    for (std::size_t o = 0; o < nout; ++o) {
      assert(batch[o * kW + w] == single[o]);
    }
  }
  // Repeat determinism on a large base.
  std::vector<std::uint64_t> again(nout * kW, 0);
  assert(logic::eval_block_words(nl, word_base, std::span<std::uint64_t>(scratchW.data(), scratchW.size()),
                                 std::span<std::uint64_t>(again.data(), again.size()))
             .ok());
  for (std::size_t i = 0; i < nout * kW; ++i) {
    assert(again[i] == batch[i]);
  }
  // Spot scalar rows inside the batch agree with the batch lanes.
  const std::size_t nin = logic::block_input_count(nl);
  std::vector<LogicValue> scalar_in(nin, kL);
  std::vector<LogicValue> scalar_out(nout, kL);
  for (std::uint64_t b = 0; b < 64; b += 21) {
    const std::uint64_t row = word_base * 64 + b;
    for (std::size_t j = 0; j < nin; ++j) {
      scalar_in[j] = ((((row >> j) & 1ULL) != 0ULL) ? kH : kL);
    }
    assert(logic::solve(nl, std::span<const LogicValue>(scalar_in.data(), nin),
                        std::span<LogicValue>(scalar_out.data(), nout))
               .ok());
    for (std::size_t o = 0; o < nout; ++o) {
      const bool bit = (((batch[o * kW] >> b) & 1ULL) != 0ULL);
      assert(bit == (scalar_out[o] == kH));
    }
  }
}

// Caller-pattern composition (common.cpp/bench shape): full batches
// followed by a single-slice tail on the same net. Full-truth slice
// counts never leave a remainder past a full batch (need is 1, 2, or a
// multiple of 4), so the corpus forces the composition explicitly:
// batches for slices [0, 12), singles for [12, 16) on a 10-input net,
// every word against the single-slice reference.
void test_batch_then_tail() {
  Netlist nl;
  add_ok(nl, GateKind::kAnd, 8);
  add_ok(nl, GateKind::kBuf, 1);
  add_ok(nl, GateKind::kNot, 1);
  assert(logic::block_input_count(nl) == 10);
  const std::size_t need = logic::bit_words_for_inputs(10);
  assert(need == 16);
  const std::size_t n = logic::slot_count(nl);
  const std::size_t nout = logic::block_output_count(nl);
  std::vector<std::uint64_t> scratch1(n, 0);
  std::vector<std::uint64_t> single(nout, 0);
  std::vector<std::uint64_t> scratchW(n * kW, 0);
  std::vector<std::uint64_t> batch(nout * kW, 0);
  for (std::uint64_t base = 0; base < 12; base += static_cast<std::uint64_t>(kW)) {
    assert(logic::eval_block_words(nl, base, std::span<std::uint64_t>(scratchW.data(), scratchW.size()),
                                   std::span<std::uint64_t>(batch.data(), batch.size()))
               .ok());
    for (std::size_t w = 0; w < kW; ++w) {
      assert(logic::eval_block_word(nl, base + static_cast<std::uint64_t>(w),
                                    std::span<std::uint64_t>(scratch1.data(), scratch1.size()),
                                    std::span<std::uint64_t>(single.data(), single.size()))
                 .ok());
      for (std::size_t o = 0; o < nout; ++o) {
        assert(batch[o * kW + w] == single[o]);
      }
    }
  }
  // Tail: slices past the last full batch go single-slice (the caller
  // pattern — any aligned batch covering them would also succeed, so
  // the pin here is an overrunning batch: lanes past the truth refuse
  // loudly even when the base itself is in range).
  for (std::uint64_t idx = 12; idx < static_cast<std::uint64_t>(need); ++idx) {
    assert(logic::eval_block_word(nl, idx, std::span<std::uint64_t>(scratch1.data(), scratch1.size()),
                                  std::span<std::uint64_t>(single.data(), single.size()))
               .ok());
  }
  assert(!logic::eval_block_words(nl, static_cast<std::uint64_t>(need - 1),
                                  std::span<std::uint64_t>(scratchW.data(), scratchW.size()),
                                  std::span<std::uint64_t>(batch.data(), batch.size()))
              .ok());
}

// Refusal-verdict parity (US3): every loud refusal the single-slice
// path raises, the batched path raises with the same code — guards
// (shared/TBUF at the verb entries), engine safety nets (live TBUF,
// cycles), arity (scratch/output sizes against each entry's own
// contract), and range (every lane past the truth).
void check_both_refuse(Netlist &nl, std::uint64_t word_base) {
  const std::size_t n = logic::slot_count(nl);
  const std::size_t nout = logic::block_output_count(nl);
  std::vector<std::uint64_t> scratch1(n == 0 ? 1 : n, 0);
  std::vector<std::uint64_t> single(nout == 0 ? 1 : nout, 0);
  std::vector<std::uint64_t> scratchW(n == 0 ? 1 : n * kW, 0);
  std::vector<std::uint64_t> batch(nout == 0 ? 1 : nout * kW, 0);
  const auto s1 = logic::eval_block_word(nl, word_base,
                                         std::span<std::uint64_t>(scratch1.data(), scratch1.size()),
                                         std::span<std::uint64_t>(single.data(), nout));
  const auto sW = logic::eval_block_words(nl, word_base,
                                          std::span<std::uint64_t>(scratchW.data(), scratchW.size()),
                                          std::span<std::uint64_t>(batch.data(), nout * kW));
  assert(!s1.ok() && !sW.ok());
  assert(s1.code == sW.code);
  assert(s1.code == wengine::core::StatusCode::kInvalidArgument);
}

void test_refusal_parity() {
  // Shared net with 2 attachments (17 block inputs, so a full batch
  // fits): verb entries refuse via shared_word_blocked (pinned here);
  // both word entries take the same degraded path and agree
  // word-for-word (verdict parity, not words).
  {
    Netlist nl;
    const Slot a1 = add_ok(nl, GateKind::kAnd, 8);
    const Slot a2 = add_ok(nl, GateKind::kAnd, 8);
    const Slot o = add_ok(nl, GateKind::kOr, 2);
    wire_ok(nl, a1, o, 0);
    assert(logic::mark_shared(nl, o, 0).ok());
    wire_ok(nl, a2, o, 0);
    assert(logic::shared_word_blocked(nl));
    const std::size_t n = logic::slot_count(nl);
    const std::size_t nout = logic::block_output_count(nl);
    std::vector<std::uint64_t> scratch1(n, 0);
    std::vector<std::uint64_t> single(nout, 0);
    std::vector<std::uint64_t> scratchW(n * kW, 0);
    std::vector<std::uint64_t> batch(nout * kW, 0);
    assert(logic::eval_block_words(nl, 0, std::span<std::uint64_t>(scratchW.data(), scratchW.size()),
                                   std::span<std::uint64_t>(batch.data(), batch.size()))
               .ok());
    assert(logic::eval_block_word(nl, 0, std::span<std::uint64_t>(scratch1.data(), scratch1.size()),
                                  std::span<std::uint64_t>(single.data(), single.size()))
               .ok());
    for (std::size_t oo = 0; oo < nout; ++oo) {
      for (std::size_t w = 0; w < kW; ++w) {
        assert(batch[oo * kW + w] == single[oo]);
      }
    }
  }
  // Live kTbuf: verb entries refuse via has_buffer_gates (pinned
  // here); both engine entries fail loudly too.
  {
    Netlist nl;
    const Slot t = add_ok(nl, GateKind::kTbuf, 2);
    (void)t;
    assert(logic::has_buffer_gates(nl));
    check_both_refuse(nl, 0);
  }
  // Combinational cycle: unconstructible via wire() (the incremental
  // cycle check refuses the closing edge and leaves the netlist
  // untouched), so there is no cyclic netlist to feed either entry —
  // both call the same topo::compile first, which is where the verdict
  // would originate. Structural parity, nothing to pin at runtime.
  // Out-of-range slice: 2-input net, lane past word 0.
  {
    Netlist nl;
    add_ok(nl, GateKind::kAnd, 2);
    check_both_refuse(nl, 1);
  }
  // Undersized scratch: n-1 words (both thresholds missed).
  {
    Netlist nl;
    add_ok(nl, GateKind::kAnd, 8);
    const std::size_t n = logic::slot_count(nl);
    assert(n > 1);
    std::vector<std::uint64_t> tiny(n - 1, 0);
    std::vector<std::uint64_t> out1(1, 0);
    std::vector<std::uint64_t> outW(kW, 0);
    const auto s1 = logic::eval_block_word(nl, 0, std::span<std::uint64_t>(tiny.data(), tiny.size()),
                                           std::span<std::uint64_t>(out1.data(), out1.size()));
    const auto sW = logic::eval_block_words(nl, 0, std::span<std::uint64_t>(tiny.data(), tiny.size()),
                                            std::span<std::uint64_t>(outW.data(), outW.size()));
    assert(!s1.ok() && !sW.ok());
    assert(s1.code == sW.code);
  }
  // Output-size mismatch: empty outputs on a one-output net.
  {
    Netlist nl;
    add_ok(nl, GateKind::kAnd, 8);
    const std::size_t n = logic::slot_count(nl);
    std::vector<std::uint64_t> scratch1(n, 0);
    std::vector<std::uint64_t> scratchW(n * kW, 0);
    const auto s1 = logic::eval_block_word(nl, 0, std::span<std::uint64_t>(scratch1.data(), scratch1.size()),
                                           std::span<std::uint64_t>{});
    const auto sW = logic::eval_block_words(nl, 0, std::span<std::uint64_t>(scratchW.data(), scratchW.size()),
                                            std::span<std::uint64_t>{});
    assert(!s1.ok() && !sW.ok());
    assert(s1.code == sW.code);
  }
}

void test_unbounded_slices() {
  // 4x AND8 feeding OR4: 32 dangling inputs, 1 output.
  Netlist nl;
  const Slot a0 = add_ok(nl, GateKind::kAnd, 8);
  const Slot a1 = add_ok(nl, GateKind::kAnd, 8);
  const Slot a2 = add_ok(nl, GateKind::kAnd, 8);
  const Slot a3 = add_ok(nl, GateKind::kAnd, 8);
  const Slot o = add_ok(nl, GateKind::kOr, 4);
  wire_ok(nl, a0, o, 0);
  wire_ok(nl, a1, o, 1);
  wire_ok(nl, a2, o, 2);
  wire_ok(nl, a3, o, 3);
  assert(logic::block_input_count(nl) == 32);
  assert(logic::bit_words_for_inputs(32) == 0);
  const std::uint64_t bases[5] = {0, 1, 5, 12345, 1048576};
  for (std::size_t i = 0; i < 5; ++i) {
    check_spot_slice(nl, bases[i]);
  }
}

void test_api_pins() {
  static_assert(logic::kWordBatchWidth == 4, "contract width");
  static_assert(noexcept(logic::batch_words(0)), "size helper is noexcept");
  assert(logic::batch_words(0) == 0);
  assert(logic::batch_words(1) == kW);
  assert(logic::batch_words(5) == 5 * kW);
}

} // namespace

int main() {
  test_api_pins();
  test_all_kinds_fanin8();
  test_mixed_fanin_chain();
  test_partial_word_masking();
  test_multi_batch_and_tail();
  test_ignored_and_ties();
  test_pulled_termination();
  test_sequential_fallthrough();
  test_batch_then_tail();
  test_refusal_parity();
  test_degenerate();
  test_unbounded_slices();
  std::cout << "test_logic_word_batch: OK\n";
  return 0;
}
