#include "wengine/logic/eval.hpp"
#include "wengine/logic/netlist.hpp"
#include "wengine/logic/topo.hpp"
#include "wengine/logic/types.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

// Benchmark for the bit-parallel logic path: word-level truth eval over a
// live Netlist built from opaque depth/width args.
//
// Netlist: width gates per level x depth levels (<= kMaxGates). Kinds cycle
// through all 8 combinational ops via a runtime xorshift (seeded from
// argc, so the compiler cannot fold the build); fan-in is 1 for NOT/BUF
// else 2..8 opaque. Layers wire forward only (previous level -> current),
// so the graph is a DAG by construction and every wire succeeds.
// Layer 0 inputs dangle (block inputs); deeper layers wire each input to
// a previous-level gate (round-robin + opaque offset).
//
// Timed region: eval_block_word over `words` consecutive 64-row slices
// with caller-owned scratch (allocation-free steady state). Reports
// ns/gate-word plus a checksum against DCE.

template <typename T> inline void do_not_optimize(const T &v) {
#if defined(__GNUC__) || defined(__clang__)
  asm volatile("" : : "r,m"(v) : "memory");
#else
  volatile const T *p = &v;
  (void)p;
#endif
}

namespace {

std::uint32_t xorshift(std::uint32_t &s) noexcept {
  s ^= s << 13;
  s ^= s >> 17;
  s ^= s << 5;
  return s;
}

} // namespace

int main(int argc, char **argv) {
  std::size_t width = 16;
  std::size_t depth = 16;
  std::uint64_t words = 20000;
  if (argc > 1) {
    width = static_cast<std::size_t>(std::strtoull(argv[1], nullptr, 10));
    if (width == 0) {
      width = 16;
    }
  }
  if (argc > 2) {
    depth = static_cast<std::size_t>(std::strtoull(argv[2], nullptr, 10));
    if (depth == 0) {
      depth = 16;
    }
  }
  if (argc > 3) {
    words = std::strtoull(argv[3], nullptr, 10);
    if (words == 0) {
      words = 20000;
    }
  }

  using namespace wengine::logic;
  if (width == 0 || depth == 0 || width > kMaxGates || depth > kMaxGates ||
      width * depth > kMaxGates) {
    std::cout << "bench_logic: setup failed (width*depth must be 1.." << kMaxGates << ")\n";
    return 1;
  }

  std::uint32_t rng = 0xC10C1CBu ^ (static_cast<std::uint32_t>(argc) * 0x9E3779B9u + 0xC2B2AE35u);
  Netlist nl;
  // (017) Slot-width layer bases: the old uint16_t bases truncated past
  // 65535 slots, capping the harness below kMaxGates. Slot-width bases
  // keep setup valid up to the loud cap (needed for the 100k/1M points).
  std::vector<Slot> layer_base;
  layer_base.reserve(depth);
  bool setup_ok = true;
  for (std::size_t d = 0; d < depth && setup_ok; ++d) {
    layer_base.push_back(nl.live_count);
    for (std::size_t c = 0; c < width; ++c) {
      const GateKind kind = static_cast<GateKind>(xorshift(rng) % 8);
      std::uint8_t fanin = 1;
      if (kind != GateKind::kNot && kind != GateKind::kBuf) {
        fanin = static_cast<std::uint8_t>(2 + (xorshift(rng) % 7));
      }
      auto added = add_gate(nl, kind, fanin);
      if (!added.ok()) {
        setup_ok = false;
        break;
      }
      const Slot cur = added.value;
      if (d == 0) {
        continue; // layer 0 inputs dangle: block inputs
      }
      const Slot prev = layer_base[d - 1];
      const std::size_t off = static_cast<std::size_t>(xorshift(rng)) % width;
      for (std::uint8_t i = 0; i < fanin; ++i) {
        const Slot drv = prev + static_cast<Slot>((c + static_cast<std::size_t>(i) + off) % width);
        if (!wire(nl, drv, cur, i).ok()) {
          setup_ok = false;
          break;
        }
      }
    }
  }
  if (!setup_ok || !wengine::logic::topo::compile(nl, wengine::logic::EdgeFilter::kAll).ok()) {
    std::cout << "bench_logic: setup failed\n";
    return 1;
  }

  const std::size_t gates = static_cast<std::size_t>(nl.live_count);
  const std::size_t n_out = block_output_count(nl);
  std::vector<std::uint64_t> scratch(kMaxGates, 0);
  std::vector<std::uint64_t> out(n_out == 0 ? 1 : n_out, 0);
  std::span<std::uint64_t> out_span(out.data(), n_out);

  // 011: four-value nets refuse the word path loudly, once, outside the
  // timed region (bench nets never carry marks; the timed loop stays
  // branch-identical to v1).
  if (wengine::logic::shared_word_blocked(nl)) {
    std::cout << "bench_logic: shared nets need the scalar path\n";
    return 1;
  }

  // Warmup: one slice so topology/scratch are hot before timing.
  if (!eval_block_word(nl, 0, std::span<std::uint64_t>(scratch.data(), scratch.size()), out_span)
           .ok()) {
    std::cout << "bench_logic: setup failed (eval)\n";
    return 1;
  }

  auto t0 = std::chrono::high_resolution_clock::now();
  for (std::uint64_t w = 0; w < words; ++w) {
    const auto st = eval_block_word(nl, w, std::span<std::uint64_t>(scratch.data(), scratch.size()),
                                    out_span);
    if (!st.ok()) {
      // Word index out of range only when the net enumerates < `words`
      // slices (tiny widths): wrap so the timed loop stays valid. Huge
      // nets (n_inputs >= 31) accept every slice, so a failure there is
      // a real error, never a wrap (bit_words_for_inputs is 0 there).
      const std::size_t need = bit_words_for_inputs(block_input_count(nl));
      if (need == 0) {
        std::cout << "bench_logic: run failed\n";
        return 1;
      }
      const auto wrap =
          eval_block_word(nl, w % static_cast<std::uint64_t>(need),
                          std::span<std::uint64_t>(scratch.data(), scratch.size()), out_span);
      if (!wrap.ok()) {
        std::cout << "bench_logic: run failed\n";
        return 1;
      }
    }
  }
  auto t1 = std::chrono::high_resolution_clock::now();

  std::uint64_t checksum = 0;
  for (std::size_t i = 0; i < n_out; ++i) {
    checksum ^= out[i];
    checksum += out[i] * 0x9E3779B97F4A7C15ULL;
  }
  checksum ^= static_cast<std::uint64_t>(gates) * 0xBF58476D1CE4E5B9ULL;
  do_not_optimize(checksum);

  const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
  const double gate_words = static_cast<double>(gates) * static_cast<double>(words);
  std::cout << "bench_logic: " << gates << " gates (" << width << "w x " << depth << "d) x "
            << words << " words, " << static_cast<double>(ns) / gate_words
            << " ns/gate-word (checksum=" << checksum << ")\n";

  // (017) Batched twin of the word loop above: same net, same slices,
  // opaque xorshift wrap, checksummed outputs. Full batches share one
  // traversal via eval_block_words; the tail stays single-slice. The
  // before/after per-gate-word delta is the committed 017 record.
  {
    constexpr std::size_t kBW = wengine::logic::kWordBatchWidth;
    std::vector<std::uint64_t> bscratch(kMaxGates * kBW, 0);
    std::vector<std::uint64_t> bout(n_out == 0 ? kBW : n_out * kBW, 0);
    std::span<std::uint64_t> bscratch_span(bscratch.data(), bscratch.size());
    std::span<std::uint64_t> bout_span(bout.data(), n_out * kBW);
    const std::uint64_t nbatches = words / static_cast<std::uint64_t>(kBW);
    std::uint64_t bchecksum = 0;
    auto b0 = std::chrono::high_resolution_clock::now();
    for (std::uint64_t b = 0; b < nbatches; ++b) {
      const auto st = eval_block_words(nl, b * static_cast<std::uint64_t>(kBW), bscratch_span, bout_span);
      if (st.ok()) {
        continue;
      }
      // Same wrap rule as the single-slice loop above, lane by lane;
      // single-slice outputs fold into the checksum as they land.
      for (std::size_t w = 0; w < kBW; ++w) {
        const std::uint64_t slice = b * static_cast<std::uint64_t>(kBW) + static_cast<std::uint64_t>(w);
        const auto sst = eval_block_word(nl, slice, std::span<std::uint64_t>(scratch.data(), scratch.size()),
                                         out_span);
        if (sst.ok()) {
          for (std::size_t i = 0; i < n_out; ++i) {
            bchecksum ^= out[i] + static_cast<std::uint64_t>(w);
            bchecksum += out[i] * 0x9E3779B97F4A7C15ULL;
          }
          continue;
        }
        const std::size_t need = bit_words_for_inputs(block_input_count(nl));
        if (need == 0) {
          std::cout << "bench_logic_batch: run failed\n";
          return 1;
        }
        const auto wrap =
            eval_block_word(nl, slice % static_cast<std::uint64_t>(need),
                            std::span<std::uint64_t>(scratch.data(), scratch.size()), out_span);
        if (!wrap.ok()) {
          std::cout << "bench_logic_batch: run failed\n";
          return 1;
        }
        for (std::size_t i = 0; i < n_out; ++i) {
          bchecksum ^= out[i] + static_cast<std::uint64_t>(w);
          bchecksum += out[i] * 0x9E3779B97F4A7C15ULL;
        }
      }
    }
    for (std::uint64_t w = nbatches * static_cast<std::uint64_t>(kBW); w < words; ++w) {
      const auto st = eval_block_word(nl, w, std::span<std::uint64_t>(scratch.data(), scratch.size()),
                                      out_span);
      if (!st.ok()) {
        std::cout << "bench_logic_batch: tail failed\n";
        return 1;
      }
      for (std::size_t i = 0; i < n_out; ++i) {
        bchecksum ^= out[i] + w;
        bchecksum += out[i] * 0x9E3779B97F4A7C15ULL;
      }
    }
    auto b1 = std::chrono::high_resolution_clock::now();

    for (std::size_t i = 0; i < n_out * kBW; ++i) {
      bchecksum ^= bout[i];
      bchecksum += bout[i] * 0x9E3779B97F4A7C15ULL;
    }
    bchecksum ^= static_cast<std::uint64_t>(gates) * 0xBF58476D1CE4E5B9ULL;
    do_not_optimize(bchecksum);

    const auto bns = std::chrono::duration_cast<std::chrono::nanoseconds>(b1 - b0).count();
    std::cout << "bench_logic_batch: " << gates << " gates (" << width << "w x " << depth << "d) x "
              << words << " words, " << static_cast<double>(bns) / gate_words
              << " ns/gate-word (checksum=" << bchecksum << ")\n";
  }

  // Scalar shared-path bench (VAL2 measurement baseline): marked nets with
  // mixed attachments + pulls, opaque xorshift inputs like the word bench.
  // Motifs (all consensus/deterministic so every solve succeeds):
  //  - 32 consensus buses: 3 BUFs with tied inputs (one block input each)
  //    share one marked OR input (primary + 2 extras, Q polarity); the OR
  //    second input is pull-terminated (alternating up/down), exercising
  //    fill_resolved termination beside the span.
  //  - 1 inverted bus: 2 DFF NQ sources share one marked OR input (primary
  //    inverted + 1 inverted extra) with a pull (driven+pull inert path);
  //    DFF data/clock inputs are pull-terminated so the block vector stays
  //    the 32 bus leaders.
  //  - 1 floating bus: marked OR input with no drivers + pull (all-floating
  //    span maps through the pull to idle).
  // The timed solve loop passes a SolveDiag channel, so contention counting
  // runs every shared resolution; the timed rebuild loop (dirty forced)
  // baselines the extra-edge Kahn compile. Neither path may optimize
  // before this measurement.
  {
    Netlist snl;
    bool shared_ok = true;
    constexpr std::size_t kBuses = 32;
    for (std::size_t b = 0; b < kBuses && shared_ok; ++b) {
      auto bufa = add_gate(snl, GateKind::kBuf, 1);
      auto bufb = add_gate(snl, GateKind::kBuf, 1);
      auto bufc = add_gate(snl, GateKind::kBuf, 1);
      auto sink = add_gate(snl, GateKind::kOr, 2);
      if (!bufa.ok() || !bufb.ok() || !bufc.ok() || !sink.ok()) {
        shared_ok = false;
        break;
      }
      const PinRef pa{bufa.value, false, 0};
      const PinRef pb{bufb.value, false, 0};
      const PinRef pc{bufc.value, false, 0};
      if (!tie_pins(snl, pa, pb).ok() || !tie_pins(snl, pb, pc).ok()) {
        shared_ok = false;
        break;
      }
      if (!wire(snl, bufa.value, sink.value, 0).ok() ||
          !mark_shared(snl, sink.value, 0).ok() ||
          !wire(snl, bufb.value, sink.value, 0).ok() ||
          !wire(snl, bufc.value, sink.value, 0).ok()) {
        shared_ok = false;
        break;
      }
      // Alternating idle: even buses force High (constant output), odd
      // buses pass the opaque input through (varying checksum).
      const std::uint8_t level = ((b % 2) == 0) ? 1 : 2;
      if (!set_pull(snl, sink.value, 1, level).ok()) {
        shared_ok = false;
        break;
      }
    }
    // Inverted bus (extra_inv path): two constant-High NQ sources agree.
    std::uint16_t inv_sink = 0;
    if (shared_ok) {
      auto dff1 = add_gate(snl, GateKind::kDff, 2);
      auto dff2 = add_gate(snl, GateKind::kDff, 2);
      auto sink = add_gate(snl, GateKind::kOr, 2);
      if (!dff1.ok() || !dff2.ok() || !sink.ok()) {
        shared_ok = false;
      } else {
        inv_sink = sink.value;
        const PinRef nq1{dff1.value, true, 0, true};
        const PinRef nq2{dff2.value, true, 0, true};
        const PinRef dst{sink.value, false, 0};
        if (!wire_pins(snl, nq1, dst).ok() || !mark_shared(snl, sink.value, 0).ok() ||
            !wire_pins(snl, nq2, dst).ok()) {
          shared_ok = false;
        }
        // Driven + pull is inert (exercises the check on a driven span);
        // the second input + all four flop pins terminate.
        if (shared_ok &&
            (!set_pull(snl, sink.value, 0, 1).ok() || !set_pull(snl, sink.value, 1, 2).ok() ||
             !set_pull(snl, dff1.value, 0, 2).ok() || !set_pull(snl, dff1.value, 1, 1).ok() ||
             !set_pull(snl, dff2.value, 0, 2).ok() || !set_pull(snl, dff2.value, 1, 1).ok())) {
          shared_ok = false;
        }
      }
    }
    // Floating bus (pull map path): marked input, no drivers, pull-up.
    if (shared_ok) {
      auto sink = add_gate(snl, GateKind::kOr, 2);
      if (!sink.ok() || !mark_shared(snl, sink.value, 0).ok() ||
          !set_pull(snl, sink.value, 0, 1).ok() || !set_pull(snl, sink.value, 1, 2).ok()) {
        shared_ok = false;
      }
    }
    const std::size_t n_in = block_input_count(snl);
    const std::size_t n_out = block_output_count(snl);
    if (!shared_ok || n_in != kBuses || n_out == 0 ||
        !wengine::logic::topo::compile(snl, wengine::logic::EdgeFilter::kAll).ok()) {
      std::cout << "bench_logic_shared: setup failed\n";
      return 1;
    }
    // The inverted bus must actually hold the inverted extra (else the
    // bench silently times the wrong path).
    if (shared_attachment_count(snl, inv_sink, 0) != 2 ||
        shared_extra_inv(snl, tie_flat(inv_sink, 0), 0) == 0) {
      std::cout << "bench_logic_shared: setup failed (inverted extra missing)\n";
      return 1;
    }

    std::vector<LogicValue> bin(n_in, LogicValue::kLow);
    std::vector<LogicValue> bout(n_out, LogicValue::kLow);
    SolveDiag diag{};
    // Warmup: one solve so topology/scratch are hot before timing.
    for (std::size_t j = 0; j < n_in; ++j) {
      bin[j] = (((xorshift(rng) & 1u) != 0u) ? LogicValue::kHigh : LogicValue::kLow);
    }
    if (!solve(snl, std::span<const LogicValue>(bin.data(), n_in),
               std::span<LogicValue>(bout.data(), n_out), &diag)
             .ok()) {
      std::cout << "bench_logic_shared: setup failed (solve)\n";
      return 1;
    }

    auto s0 = std::chrono::high_resolution_clock::now();
    for (std::uint64_t it = 0; it < words; ++it) {
      for (std::size_t j = 0; j < n_in; ++j) {
        bin[j] = (((xorshift(rng) & 1u) != 0u) ? LogicValue::kHigh : LogicValue::kLow);
      }
      if (!solve(snl, std::span<const LogicValue>(bin.data(), n_in),
                 std::span<LogicValue>(bout.data(), n_out), &diag)
               .ok()) {
        std::cout << "bench_logic_shared: run failed\n";
        return 1;
      }
    }
    auto s1 = std::chrono::high_resolution_clock::now();

    std::uint64_t schecksum = 0;
    for (std::size_t i = 0; i < n_out; ++i) {
      schecksum ^= static_cast<std::uint64_t>(bout[i]) * 0x9E3779B97F4A7C15ULL + i;
    }
    // (017) Bound by the live channel size: solve() sizes per_net to
    // the pool (n*kMaxInputs), not kMaxGates*kMaxInputs — the old bound
    // read out-of-bounds past the pool on every run (defect predates
    // this feature; fixed here because the batch record needs a clean
    // full-sweep run). Zeros beyond the pool would contribute nothing.
    for (std::size_t f = 0; f < diag.per_net.size(); ++f) {
      schecksum += static_cast<std::uint64_t>(diag.per_net[f]);
    }
    schecksum ^= static_cast<std::uint64_t>(n_in) * 0xBF58476D1CE4E5B9ULL;
    do_not_optimize(schecksum);

    const auto sns = std::chrono::duration_cast<std::chrono::nanoseconds>(s1 - s0).count();
    const double sgates = static_cast<double>(snl.live_count);
    std::cout << "bench_logic_shared: " << snl.live_count << " gates (" << kBuses
              << " buses + inv + float) x " << words << " solves, "
              << static_cast<double>(sns) / static_cast<double>(words) << " ns/solve ("
              << static_cast<double>(sns) / (sgates * static_cast<double>(words))
              << " ns/gate-solve, checksum=" << schecksum << ")\n";

    // Extra-edge Kahn baseline: force a full recompile every iteration.
    constexpr std::uint64_t kRebuildIters = 2000;
    auto r0 = std::chrono::high_resolution_clock::now();
    for (std::uint64_t it = 0; it < kRebuildIters; ++it) {
      snl.dirty = true;
      if (!wengine::logic::topo::compile(snl, wengine::logic::EdgeFilter::kAll).ok()) {
        std::cout << "bench_logic_shared_rebuild: run failed\n";
        return 1;
      }
    }
    auto r1 = std::chrono::high_resolution_clock::now();
    const auto rns = std::chrono::duration_cast<std::chrono::nanoseconds>(r1 - r0).count();
    std::cout << "bench_logic_shared_rebuild: " << snl.live_count << " gates x " << kRebuildIters
              << " rebuilds, " << static_cast<double>(rns) / static_cast<double>(kRebuildIters)
              << " ns/rebuild\n";
  }
  return 0;
}
