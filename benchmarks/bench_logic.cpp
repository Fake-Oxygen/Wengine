#include "wengine/logic/eval.hpp"
#include "wengine/logic/netlist.hpp"
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
  std::vector<std::uint16_t> layer_base;
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
      const std::uint16_t cur = added.value;
      if (d == 0) {
        continue; // layer 0 inputs dangle: block inputs
      }
      const std::uint16_t prev = layer_base[d - 1];
      const std::uint32_t off = xorshift(rng) % static_cast<std::uint32_t>(width);
      for (std::uint8_t i = 0; i < fanin; ++i) {
        const std::uint16_t drv =
            static_cast<std::uint16_t>(prev + ((c + static_cast<std::size_t>(i) + off) % width));
        if (!wire(nl, drv, cur, i).ok()) {
          setup_ok = false;
          break;
        }
      }
    }
  }
  if (!setup_ok || !ensure_topology(nl).ok()) {
    std::cout << "bench_logic: setup failed\n";
    return 1;
  }

  const std::size_t gates = static_cast<std::size_t>(nl.live_count);
  const std::size_t n_out = block_output_count(nl);
  std::vector<std::uint64_t> scratch(kMaxGates, 0);
  std::vector<std::uint64_t> out(n_out == 0 ? 1 : n_out, 0);
  std::span<std::uint64_t> out_span(out.data(), n_out);

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
  return 0;
}
