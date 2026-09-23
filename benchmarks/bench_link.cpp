// Benchmark for the wengine-link SHM bus: publish throughput of a live
// Runner + ProjectileSolver flood (every step emitted) and lag metrics of
// a reader that attaches only after the flood (worst-case slow reader).
// Initial conditions come from a runtime xorshift PRNG (seeded from argc)
// so the compiler cannot fold the stepping; the checksum pins the whole
// chain (step -> snapshot -> adapt -> encode -> seqlock -> read -> decode)
// against dead-code elimination. Reports frames/s + ns/frame for the
// flood and published/received/missed/max_gap for the slow reader.

#include "wengine/core/runner.hpp"
#include "wengine/link/shm_publisher.hpp"
#include "wengine/link/shm_reader.hpp"
#include "wengine/solvers/projectile.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <unistd.h>

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
  std::size_t particles = 64;
  std::uint64_t steps = 20000;
  if (argc > 1) {
    particles = static_cast<std::size_t>(std::strtoull(argv[1], nullptr, 10));
    if (particles == 0 || particles > wengine::link::kMaxItems) {
      particles = 64;
    }
  }
  if (argc > 2) {
    steps = std::strtoull(argv[2], nullptr, 10);
    if (steps == 0) {
      steps = 20000;
    }
  }

  std::uint32_t rng = 0x85EBCA6Bu ^ (static_cast<std::uint32_t>(argc) * 0x9E3779B9u +
                                     0xC2B2AE35u);
  const std::string seg =
      "/wengine-bench-link-" + std::to_string(::getpid());
  wengine::link::ShmPublisher pub(seg, "projectile");
  if (!pub.ok()) {
    std::cout << "bench_link: publisher open failed\n";
    return 1;
  }

  wengine::core::Runner runner{{.dt = 0.01, .emit_every = 1}};
  wengine::solvers::ProjectileSolver solver;
  runner.add_solver(solver);
  for (std::size_t k = 0; k < particles; ++k) {
    wengine::world::Entity e = runner.state().spawn_entity();
    runner.state().particles.attach(e.index);
    runner.state().particles.pos_x[e.index] =
        -10.0 + (xorshift(rng) % 20000) / 1000.0;
    runner.state().particles.pos_y[e.index] =
        -10.0 + (xorshift(rng) % 20000) / 1000.0;
    runner.state().particles.vel_x[e.index] =
        -10.0 + (xorshift(rng) % 20000) / 1000.0;
    runner.state().particles.vel_y[e.index] =
        -10.0 + (xorshift(rng) % 20000) / 1000.0;
  }
  runner.add_sink(pub);

  // Scenario A: flood — every step published, no reader attached.
  auto t0 = std::chrono::high_resolution_clock::now();
  const wengine::core::Status st = runner.run(steps);
  auto t1 = std::chrono::high_resolution_clock::now();
  if (!st.ok() || !pub.ok() || pub.stats().published != steps) {
    std::cout << "bench_link: flood failed\n";
    return 1;
  }
  const auto flood_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
  const double ns_per_frame =
      static_cast<double>(flood_ns) / static_cast<double>(steps);
  const double frames_per_s = 1e9 / ns_per_frame;
  const double bus_mb_per_s = frames_per_s * sizeof(wengine::link::LinkFrame) / 1e6;

  // Scenario B: slow reader — attaches after the flood, takes latest only.
  wengine::link::ShmReader reader(seg);
  if (!reader.ok()) {
    std::cout << "bench_link: reader attach failed\n";
    return 1;
  }
  wengine::link::LagStats lag;
  wengine::link::LinkFrame wire{};
  double checksum = 0.0;
  if (reader.read_latest(wire)) {
    (void)lag_observe(lag, wire.frame_seq);
    wengine::link::Frame frame;
    if (!wengine::link::decode_frame(wire, frame).ok()) {
      std::cout << "bench_link: decode failed\n";
      return 1;
    }
    for (const auto &it : frame.items) {
      checksum += it.x + it.y + it.z + it.vx + it.vy + it.vz;
    }
    for (const auto &c : frame.channels) {
      checksum += c.value;
    }
  } else {
    std::cout << "bench_link: reader saw nothing\n";
    return 1;
  }
  do_not_optimize(checksum);

  std::cout << "bench_link: " << particles << " items x " << steps << " frames, "
            << ns_per_frame << " ns/frame (" << frames_per_s << " frames/s, "
            << bus_mb_per_s << " bus-MB/s, checksum=" << checksum << ")\n";
  std::cout << "bench_link: slow reader: published=" << steps
            << " received=" << lag.received << " missed=" << lag.missed
            << " max_gap=" << lag.max_gap << "\n";
  return 0;
}
