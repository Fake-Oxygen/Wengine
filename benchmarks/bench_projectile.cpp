#include "wengine/core/runner.hpp"
#include "wengine/solvers/projectile.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>

// Benchmark for the stepped path: Runner + ProjectileSolver over N live
// particles for M fixed steps. Initial conditions come from a runtime
// xorshift PRNG (seeded from argc) so the compiler cannot fold the
// integration loop. Reports cost per particle-step plus a checksum that
// pins the sink against dead-code elimination.

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
  std::size_t particles = 1024;
  std::uint64_t steps = 500;
  if (argc > 1) {
    particles = static_cast<std::size_t>(std::strtoull(argv[1], nullptr, 10));
    if (particles == 0) {
      particles = 1024;
    }
  }
  if (argc > 2) {
    steps = std::strtoull(argv[2], nullptr, 10);
    if (steps == 0) {
      steps = 500;
    }
  }

  std::uint32_t rng = 0x85EBCA6Bu ^ (static_cast<std::uint32_t>(argc) * 0x9E3779B9u +
                                     0xC2B2AE35u);
  wengine::core::Runner runner{{.dt = 0.01}};
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

  auto t0 = std::chrono::high_resolution_clock::now();
  const wengine::core::Status st = runner.run(steps);
  auto t1 = std::chrono::high_resolution_clock::now();
  if (!st.ok()) {
    std::cout << "bench_projectile: run failed\n";
    return 1;
  }

  const wengine::world::Snapshot snap = wengine::world::take_snapshot(runner.state());
  double checksum = 0.0;
  for (std::size_t k = 0; k < snap.particle_count(); ++k) {
    checksum += snap.pos_x[k] + snap.pos_y[k] + snap.vel_x[k] + snap.vel_y[k];
  }
  do_not_optimize(checksum);

  const auto ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
  const double psteps = static_cast<double>(particles) * static_cast<double>(steps);
  std::cout << "bench_projectile: " << particles << " particles x " << steps << " steps, "
            << static_cast<double>(ns) / psteps << " ns/particle-step (checksum=" << checksum
            << ")\n";
  return 0;
}
