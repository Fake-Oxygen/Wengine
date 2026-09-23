#include "wengine/core/runner.hpp"
#include "wengine/signal/types.hpp"
#include "wengine/solvers/signal.hpp"
#include "wengine/world/state.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>

// Benchmark for the signal path: Runner + SignalSolver over N live sine
// sources for M fixed steps. Frequencies/phases come from a runtime
// xorshift PRNG (seeded from argc) so the compiler cannot fold the eval
// loop. Reports cost per source-step plus a checksum against DCE.

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
  std::size_t sources = 256;
  std::uint64_t steps = 2000;
  if (argc > 1) {
    sources = static_cast<std::size_t>(std::strtoull(argv[1], nullptr, 10));
    if (sources == 0) {
      sources = 256;
    }
  }
  if (argc > 2) {
    steps = std::strtoull(argv[2], nullptr, 10);
    if (steps == 0) {
      steps = 2000;
    }
  }

  std::uint32_t rng = 0x51ED270Bu ^ (static_cast<std::uint32_t>(argc) * 0x9E3779B9u +
                                     0xC2B2AE35u);
  wengine::core::Runner runner{{.dt = 0.001}};
  wengine::solvers::SignalSolver solver;
  runner.add_solver(solver);
  for (std::size_t k = 0; k < sources; ++k) {
    wengine::signal::SignalParams p;
    p.kind = wengine::signal::SignalKind::kSine;
    p.freq_hz = 10.0 + (xorshift(rng) % 10000) / 10.0;
    p.amp = 0.5 + (xorshift(rng) % 5000) / 1000.0;
    p.phase_rad = (xorshift(rng) % 6283) / 1000.0;
    wengine::world::Entity e = runner.state().spawn_entity();
    wengine::world::configure_source(e, runner.state().signals, p, 0.0);
  }

  auto t0 = std::chrono::high_resolution_clock::now();
  const wengine::core::Status st = runner.run(steps);
  auto t1 = std::chrono::high_resolution_clock::now();
  if (!st.ok()) {
    std::cout << "bench_signal: run failed\n";
    return 1;
  }

  const wengine::world::Snapshot snap = wengine::world::take_snapshot(runner.state());
  double checksum = 0.0;
  for (std::size_t k = 0; k < snap.signal_count(); ++k) {
    checksum += snap.sig_value[k];
  }
  do_not_optimize(checksum);

  const auto ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
  const double ssteps = static_cast<double>(sources) * static_cast<double>(steps);
  std::cout << "bench_signal: " << sources << " sources x " << steps << " steps, "
            << static_cast<double>(ns) / ssteps << " ns/source-step (checksum=" << checksum
            << ")\n";
  return 0;
}
