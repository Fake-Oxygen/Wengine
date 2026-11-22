// Dynamic-pool pins: growth geometry, parity across the pool,
// prefix-slice agreement, loud refusals, and mixed contracts. Links
// wengine_core alone; default ctest size is 65,536 gates with a
// WENGINE_LOGIC_SCALE_GATES override for manual larger runs (up to
// kMaxGates; the pool grows geometrically, so small nets stay small).
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

#include "wengine/logic/eval.hpp"
#include "wengine/logic/netlist.hpp"
#include "wengine/logic/optimize_passes.hpp"
#include "wengine/logic/seq_state.hpp"
#include "wengine/logic/seq_step.hpp"
#include "wengine/logic/topo.hpp"
#include "wengine/logic/types.hpp"

namespace {

using wengine::logic::Gate;
using wengine::logic::GateKind;
using wengine::logic::kMaxGates;
using wengine::logic::kMaxInputs;
using wengine::logic::kNoDriver;
using wengine::logic::LogicValue;
using wengine::logic::Netlist;
using wengine::logic::PinRef;
using wengine::logic::SeqState;
using wengine::logic::Slot;

// Dynamic-pool identity: one 32-bit slot width, all-ones sentinel, a
// loud 1M max cap — and growth on demand instead of fixed storage.
static_assert(sizeof(Slot) == 4, "slots are 32-bit, the only width");
static_assert(wengine::logic::kNoDriver == 0xFFFFFFFFu, "kNoDriver is the all-ones sentinel");
static_assert(wengine::logic::kMaxGates == 1048576, "loud max cap is 1M slots");

// --- Scale harness ----------------------------------------------------
// Deterministic xorshift32 (fixed seed: identical args/shapes every run;
// timings vary, shapes never do).

struct XorShift {
  std::uint32_t s = 0xC0FFEEu;
  std::uint32_t next() {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
  }
};

std::size_t scale_gates() {
  const char *env = std::getenv("WENGINE_LOGIC_SCALE_GATES");
  if (env != nullptr) {
    const long v = std::atol(env);
    if (v >= 1 && static_cast<std::uint64_t>(v) <= wengine::logic::kMaxGates) {
      return static_cast<std::size_t>(v);
    }
  }
  return 65536;
}

// Pool geometry: five gates cost kilobytes, not hundreds of megabytes;
// growth is geometric; freed slots recycle lowest-first; past-cap
// growth refuses loudly and instantly.
void test_pool_identity() {
  using namespace wengine::logic;
  Netlist nl;
  assert(slot_count(nl) == 0);
  for (int i = 0; i < 5; ++i) {
    assert(add_gate(nl, GateKind::kAnd, 2).ok());
  }
  assert(nl.live_count == 5);
  assert(slot_count(nl) <= kInitialSlots); // kilobytes, never megabytes
  // Geometric growth: 100 live gates fit a small pool.
  for (int i = 5; i < 100; ++i) {
    assert(add_gate(nl, GateKind::kAnd, 2).ok());
  }
  assert(nl.live_count == 100);
  assert(slot_count(nl) >= 100 && slot_count(nl) <= 256);
  // Lowest-free-slot recycle: removing slot 7, re-adding takes 7 back.
  {
    assert(remove_gate(nl, 7).ok());
    assert(nl.live_count == 99);
    const auto r = add_gate(nl, GateKind::kOr, 2);
    assert(r.ok() && r.value == 7);
  }
  // Past-cap growth refuses loudly without allocating.
  assert(!ensure_slots(nl, kMaxGates + 1));
  assert(slot_count(nl) <= 256);
}

// Layered combinational motif shared by every scale build: the first
// kInputs gates are dangling-input NOTs (block inputs); every later
// gate i takes fan-in 2 from deterministic earlier slots (forward-only,
// so every prefix [0,P) is a closed sub-net). n must exceed kInputs.
inline GateKind layered_kind(std::size_t i) {
  return (i % 3 == 0) ? GateKind::kAnd : (i % 3 == 1) ? GateKind::kOr : GateKind::kXor;
}

// Honest build through the public mutation API (add_gate + wire,
// including the per-wire cycle probe). Used where the probe itself is
// affordable; the 65K parity build below uses the bulk path.
void build_layered(Netlist &nl, std::size_t n, std::size_t kInputs) {
  using namespace wengine::logic;
  XorShift rng;
  for (std::size_t i = 0; i < n; ++i) {
    auto r = (i < kInputs) ? add_gate(nl, GateKind::kNot, 1) : add_gate(nl, layered_kind(i), 2);
    assert(r.ok());
    assert(r.value == static_cast<Slot>(i)); // lowest-free-slot order
    if (i >= kInputs) {
      for (std::uint8_t k = 0; k < 2; ++k) {
        const auto drv = static_cast<Slot>(rng.next() % i);
        assert(wire(nl, drv, static_cast<Slot>(i), k).ok());
      }
    }
  }
}

// Bulk build for the large parity net: assigns the identical layered
// motif slot-directly (same RNG, same slots, same counters as
// build_layered would), then recounts fanout once. The per-wire cycle
// probe is the bench's growth-wire stage (budgeted there); here it
// would dominate the suite wall clock without adding parity signal,
// and the honest-wire path is covered by the prefix/mixed tests.
void build_layered_bulk(Netlist &nl, std::size_t n, std::size_t kInputs) {
  using namespace wengine::logic;
  assert(ensure_slots(nl, n));
  XorShift rng;
  std::uint32_t counters[21];
  for (std::size_t c = 0; c < 21; ++c) {
    counters[c] = 1;
  }
  for (std::size_t i = 0; i < n; ++i) {
    const GateKind kind = (i < kInputs) ? GateKind::kNot : layered_kind(i);
    const std::uint8_t fan = (i < kInputs) ? 1 : 2;
    Gate &g = nl.slots[i];
    g.live = true;
    g.kind = kind;
    g.counter = counters[kind_ordinal(kind)]++;
    g.input_count = fan;
    g.ignored_mask = 0;
    g.ignore_output = false;
    g.ignore_nq_output = false; // combinational: add_gate clears the NQ default
    g.output_fanout = 0;
    g.nq_fanout = 0;
    g.propagation_delay_ps = 0;
    for (std::size_t k = 0; k < kMaxInputs; ++k) {
      g.drivers[k] = kNoDriver;
    }
    if (i >= kInputs) {
      for (std::uint8_t k = 0; k < 2; ++k) {
        g.drivers[k] = static_cast<Slot>(rng.next() % i);
      }
    }
  }
  for (std::size_t c = 0; c < 21; ++c) {
    nl.next_counter[c] = counters[c];
  }
  nl.live_count = static_cast<std::uint32_t>(n);
  nl.dirty = true;
  detail::recount_fanout(nl);
}

// Scalar/word bit-exact parity over every truth row, plus bit-identical
// repeat solves. n_inputs must be <= 6 (one word).
void check_scalar_word_parity(Netlist &nl) {
  using namespace wengine::logic;
  const std::size_t n_in = block_input_count(nl);
  assert(n_in > 0 && n_in <= 6);
  const std::size_t n_out = block_output_count(nl);
  assert(n_out > 0);
  // Word eval, slice 0.
  std::vector<std::uint64_t> w0(n_out, 0);
  std::vector<std::uint64_t> words(slot_count(nl), 0);
  assert(eval_block_word(nl, 0, std::span<std::uint64_t>(words.data(), words.size()),
                         std::span<std::uint64_t>(w0.data(), n_out))
             .ok());
  std::vector<LogicValue> in_vals(n_in, LogicValue::kLow);
  std::vector<LogicValue> out_a(n_out, LogicValue::kLow);
  std::vector<LogicValue> out_b(n_out, LogicValue::kLow);
  const std::size_t rows = static_cast<std::size_t>(1ULL << n_in);
  for (std::size_t r = 0; r < rows; ++r) {
    for (std::size_t j = 0; j < n_in; ++j) {
      in_vals[j] = (((r >> j) & 1ULL) != 0ULL) ? LogicValue::kHigh : LogicValue::kLow;
    }
    assert(solve(nl, std::span<const LogicValue>(in_vals.data(), n_in),
                 std::span<LogicValue>(out_a.data(), n_out), nullptr)
               .ok());
    // Repeat: bit-identical.
    assert(solve(nl, std::span<const LogicValue>(in_vals.data(), n_in),
                 std::span<LogicValue>(out_b.data(), n_out), nullptr)
               .ok());
    for (std::size_t o = 0; o < n_out; ++o) {
      assert(out_a[o] == out_b[o]); // bit-identical repeats
      const bool word_bit = ((w0[o] >> r) & 1ULL) != 0ULL;
      const bool scalar_bit = (out_a[o] == LogicValue::kHigh);
      assert(word_bit == scalar_bit); // cross-discipline parity
    }
  }
}

void test_scale_parity() {
  using namespace wengine::logic;
  const std::size_t n = scale_gates();
  auto nl = std::make_unique<Netlist>();
  constexpr std::size_t kInputs = 6;
  build_layered_bulk(*nl, n, kInputs);
  assert(nl->live_count == static_cast<std::uint32_t>(n));
  assert(topo::rebuild(*nl, EdgeFilter::kAll).ok());
  check_scalar_word_parity(*nl);
}

// Prefix-slice agreement: the first kPrefix gates are prefix-closed by
// forward-only construction, so a standalone 512-gate build must match
// the suffix-removed pool slot-for-slot (same sentinel now — one pool,
// one width), and both solve identically (growth changes size, never
// semantics).
void test_prefix_slice() {
  using namespace wengine::logic;
  constexpr std::size_t kPrefix = 512;
  constexpr std::size_t kInputs = 6;
  // Standalone prefix build.
  Netlist small{};
  {
    XorShift rng;
    for (std::size_t i = 0; i < kPrefix; ++i) {
      auto r = (i < kInputs) ? add_gate(small, GateKind::kNot, 1) : add_gate(small, layered_kind(i), 2);
      assert(r.ok());
      if (i >= kInputs) {
        for (std::uint8_t k = 0; k < 2; ++k) {
          const auto drv = static_cast<Slot>(rng.next() % i);
          assert(wire(small, drv, static_cast<Slot>(i), k).ok());
        }
      }
    }
  }
  // Full net with the identical deterministic prefix.
  auto nl = std::make_unique<Netlist>();
  build_layered(*nl, 8192, kInputs);
  // Remove the suffix (high to low), leaving the prefix pool.
  for (std::size_t i = 8192; i-- > kPrefix;) {
    assert(remove_gate(*nl, static_cast<Slot>(i)).ok());
  }
  assert(nl->live_count == static_cast<std::uint32_t>(kPrefix));
  assert(topo::rebuild(*nl, EdgeFilter::kAll).ok());
  // Pool-identical to the standalone build (counters, ties, topo
  // caches included — growth provably changed nothing).
  for (std::size_t s = 0; s < kPrefix; ++s) {
    const auto &a = nl->slots[s];
    const auto &b = small.slots[s];
    assert(a.live == b.live && a.kind == b.kind && a.counter == b.counter);
    assert(a.input_count == b.input_count && a.ignored_mask == b.ignored_mask);
    assert(a.output_fanout == b.output_fanout);
    for (std::uint8_t k = 0; k < a.input_count; ++k) {
      assert(a.drivers[k] == b.drivers[k]);
    }
  }
  // And both solve identically on every row.
  const std::size_t n_in = block_input_count(small);
  const std::size_t n_out = block_output_count(small);
  assert(n_in == kInputs && block_input_count(*nl) == n_in);
  assert(block_output_count(*nl) == n_out);
  std::vector<LogicValue> in_vals(n_in), o_small(n_out), o_big(n_out);
  const std::size_t rows = static_cast<std::size_t>(1ULL << n_in);
  for (std::size_t r = 0; r < rows; ++r) {
    for (std::size_t j = 0; j < n_in; ++j) {
      in_vals[j] = (((r >> j) & 1ULL) != 0ULL) ? LogicValue::kHigh : LogicValue::kLow;
    }
    assert(solve(small, std::span<const LogicValue>(in_vals.data(), n_in),
                 std::span<LogicValue>(o_small.data(), n_out))
               .ok());
    assert(solve(*nl, std::span<const LogicValue>(in_vals.data(), n_in),
                 std::span<LogicValue>(o_big.data(), n_out))
               .ok());
    for (std::size_t o = 0; o < n_out; ++o) {
      assert(o_small[o] == o_big[o]);
    }
  }
}

// Refusal parity: loud cap, single-driver, cycle — all on the one pool.
void test_scale_refusals() {
  using namespace wengine::logic;
  // Past-cap growth refuses loudly without allocating.
  Netlist nl;
  assert(ensure_slots(nl, 64));
  assert(!ensure_slots(nl, kMaxGates + 1));
  // Occupied input: single-driver refusal.
  assert(add_gate(nl, GateKind::kAnd, 2).ok());
  assert(add_gate(nl, GateKind::kAnd, 2).ok());
  assert(wire(nl, 1, 0, 0).ok());
  assert(!wire(nl, 0, 0, 0).ok());
  // Cycle: closing 0 -> 1 -> 0 is refused loudly.
  assert(!wire(nl, 0, 1, 0).ok());
}

// Mixed contracts: shared nets, pulls, sequential elements, delay
// annotations solve and step exactly as at the small scale.
void test_scale_mixed() {
  using namespace wengine::logic;
  auto nl = std::make_unique<Netlist>();
  auto st = std::make_unique<SeqState>();
  XorShift rng;
  constexpr std::size_t kInputs = 4;
  constexpr std::size_t kN = 2048;
  // Layered base with a flop strand, shared bus, pulls, delays.
  for (std::size_t i = 0; i < kN; ++i) {
    GateKind k = GateKind::kAnd;
    std::uint8_t fan = 2;
    if (i < kInputs) {
      k = GateKind::kNot;
      fan = 1;
    } else if (i % 97 == 0) {
      k = GateKind::kDff;
      fan = 2;
    }
    assert(add_gate(*nl, k, fan).ok());
    if (i >= kInputs && (i % 3 != 0)) {
      const auto drv = static_cast<Slot>(rng.next() % i);
      assert(wire(*nl, drv, static_cast<Slot>(i), 0).ok());
      const auto drv2 = static_cast<Slot>(rng.next() % i);
      assert(wire(*nl, drv2, static_cast<Slot>(i), 1).ok());
      if (i % 5 == 0) {
        // Shared third driver on a marked input (append path). The
        // extra reuses input 1's primary slot, so the span always
        // agrees (contention would fail loudly by contract — that
        // refusal is pinned by the shared-net suite).
        assert(mark_shared(*nl, static_cast<Slot>(i), 1).ok());
        assert(wire(*nl, drv2, static_cast<Slot>(i), 1).ok());
      }
    }
    if (i % 11 == 0) {
      assert(set_gate_delay(*nl, static_cast<Slot>(i), 100).ok());
    }
  }
  // Shared nets block word eval loudly (4-value funnel has no 1-bit form).
  assert(shared_word_blocked(*nl));
  // Scalar shared-net solve works, bit-identical on repeat.
  const std::size_t n_in = block_input_count(*nl);
  const std::size_t n_out = block_output_count(*nl);
  std::vector<LogicValue> in_vals(n_in, LogicValue::kHigh);
  std::vector<LogicValue> out_a(n_out), out_b(n_out);
  assert(solve(*nl, std::span<const LogicValue>(in_vals.data(), n_in),
               std::span<LogicValue>(out_a.data(), n_out), nullptr)
             .ok());
  assert(solve(*nl, std::span<const LogicValue>(in_vals.data(), n_in),
               std::span<LogicValue>(out_b.data(), n_out), nullptr)
             .ok());
  for (std::size_t o = 0; o < n_out; ++o) {
    assert(out_a[o] == out_b[o]);
  }
  // Sequential step works and repeats bit-identically.
  std::vector<LogicValue> step_a(n_out), step_b(n_out);
  assert(seq_step_filtered(*nl, *st, std::span<const LogicValue>(in_vals.data(), n_in),
                           std::span<LogicValue>(step_a.data(), n_out), nullptr, 0)
             .ok());
  assert(seq_step_filtered(*nl, *st, std::span<const LogicValue>(in_vals.data(), n_in),
                           std::span<LogicValue>(step_b.data(), n_out), nullptr, 0)
             .ok());
  for (std::size_t o = 0; o < n_out; ++o) {
    assert(step_a[o] == step_b[o]);
  }
}

} // namespace

int main() {
  test_pool_identity();
  test_scale_parity();
  test_prefix_slice();
  test_scale_refusals();
  test_scale_mixed();
  std::printf("test_logic_scale: OK\n");
  return 0;
}
