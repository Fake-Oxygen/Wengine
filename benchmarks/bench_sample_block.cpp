// SampleBlock batching benchmark (PHYSI-44): bytes/sample and encode cost
// for batched consecutive ticks vs per-step compact frames. Deterministic
// pattern from argv (opaque to the optimizer); no timing thresholds.
//
// Repro:
//   bench_sample_block [ticks] [channels] [samples-per-block]

#include "wengine/link/compact_codec.hpp"
#include "wengine/link/frame.hpp"
#include "wengine/link/sample_block.hpp"
#include "wengine/world/state.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

wengine::world::Snapshot make_tick(std::uint64_t step, double time, std::size_t channels,
                                   std::uint64_t serial_base, double wobble) {
  wengine::world::Snapshot s;
  s.step_index = step;
  s.sim_time = time;
  s.sig_serial.resize(channels);
  s.sig_quantity.resize(channels);
  s.sig_value.resize(channels);
  for (std::size_t c = 0; c < channels; ++c) {
    s.sig_serial[c] = serial_base + c;
    s.sig_quantity[c] = static_cast<std::uint8_t>(c % 2);
    const double v = static_cast<double>((step * 131 + c * 17) % 1009) * 0.001 * wobble;
    s.sig_value[c] = v;
  }
  return s;
}

wengine::link::Frame frame_for_tick(const wengine::world::Snapshot &s) {
  return wengine::link::frame_from_signal_snapshot(s);
}

} // namespace

int main(int argc, char **argv) {
  std::size_t ticks = 640;
  std::size_t channels = 8;
  std::size_t per_block = 64;
  if (argc > 1) {
    ticks = static_cast<std::size_t>(std::strtoul(argv[1], nullptr, 10));
  }
  if (argc > 2) {
    channels = static_cast<std::size_t>(std::strtoul(argv[2], nullptr, 10));
  }
  if (argc > 3) {
    per_block = static_cast<std::size_t>(std::strtoul(argv[3], nullptr, 10));
  }
  if (ticks == 0 || channels == 0 || channels > wengine::link::sample::kBlockChannelsMax ||
      per_block == 0) {
    std::cerr << "usage: bench_sample_block [ticks] [channels] [samples-per-block]\n";
    return 2;
  }
  // Opaque wobble so values are not constant-foldable.
  volatile double wobble_src = 1.0;
  const double wobble = wobble_src + (argc > 4 ? 0.5 : 0.0);

  wengine::link::sample::SampleBlockConfig cfg;
  cfg.max_samples = per_block;
  wengine::link::sample::SampleBlockBuilder builder(cfg);

  std::vector<wengine::link::sample::SampleBlock> blocks;
  double time = 0.0;
  std::size_t frame_bytes_total = 0;
  const auto t0 = std::chrono::steady_clock::now();
  for (std::size_t step = 0; step < ticks; ++step) {
    time += 0.001;
    const auto snap = make_tick(step, time, channels, 0, wobble);
    frame_bytes_total += wengine::link::compact::encoded_size(frame_for_tick(snap));
    wengine::link::sample::SampleBlock sealed;
    bool has = false;
    if (!builder.push(snap, sealed, has).ok()) {
      std::cerr << "push failed\n";
      return 1;
    }
    if (has) {
      blocks.push_back(sealed);
    }
  }
  {
    wengine::link::sample::SampleBlock tail;
    bool has = false;
    if (!builder.flush(tail, has).ok()) {
      std::cerr << "flush failed\n";
      return 1;
    }
    if (has) {
      blocks.push_back(tail);
    }
  }
  const auto t1 = std::chrono::steady_clock::now();

  std::vector<unsigned char> wire(wengine::link::sample::kBlockMaxBytes);
  std::size_t block_bytes_total = 0;
  std::size_t encoded_blocks = 0;
  const auto e0 = std::chrono::steady_clock::now();
  for (std::size_t b = 0; b < blocks.size(); ++b) {
    std::size_t len = 0;
    if (!wengine::link::sample::encode_block(blocks[b], "signal", 1, b, wire.data(),
                                             wire.size(), len)
             .ok()) {
      std::cerr << "encode failed\n";
      return 1;
    }
    block_bytes_total += len;
    ++encoded_blocks;
  }
  const auto e1 = std::chrono::steady_clock::now();

  const double build_us =
      std::chrono::duration<double, std::micro>(t1 - t0).count();
  const double encode_us =
      std::chrono::duration<double, std::micro>(e1 - e0).count();
  const double block_bytes_per_sample =
      static_cast<double>(block_bytes_total) / static_cast<double>(ticks);
  const double frame_bytes_per_sample =
      static_cast<double>(frame_bytes_total) / static_cast<double>(ticks);
  const double block_bytes_per_value =
      block_bytes_per_sample / static_cast<double>(channels);

  std::cout << "ticks=" << ticks << " channels=" << channels
            << " per_block=" << per_block << " blocks=" << blocks.size() << "\n";
  if (frame_bytes_total == 0) {
    std::cout << "frames: over-bound (>64 LinkFrame channels, not encodable per step)\n";
  } else {
    std::cout << "frames: total_bytes=" << frame_bytes_total << " bytes/sample="
              << frame_bytes_per_sample << "\n";
  }
  std::cout << "blocks: total_bytes=" << block_bytes_total << " bytes/sample="
            << block_bytes_per_sample << " bytes/value=" << block_bytes_per_value
            << "\n";
  if (frame_bytes_total != 0) {
    std::cout << "ratio(frame/block)=" << frame_bytes_per_sample / block_bytes_per_sample
              << "\n";
  }
  std::cout << "build_us=" << build_us << " (" << build_us / static_cast<double>(ticks)
            << " us/tick) encode_us=" << encode_us << " blocks=" << encoded_blocks
            << "\n";
  // Owned storage note: every block owns its vectors; no views into builder
  // or snapshot lifetime survive push/flush (blocks.size() owned above).
  std::cout << "owned_blocks=" << blocks.size() << "\n";
  return 0;
}
