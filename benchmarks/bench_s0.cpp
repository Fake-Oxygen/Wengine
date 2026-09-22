#include "wengine/solvers/s0_pulse.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

// Benchmark for the S0 pulse-output solver (heaviest entry: solve_full,
// ~10 flops + an infinity branch). Inputs are runtime-generated so the
// constexpr solver cannot be constant-folded.

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
  std::size_t iters = 1'000'000;
  if (argc > 1) {
    iters = static_cast<std::size_t>(std::strtoull(argv[1], nullptr, 10));
    if (iters == 0) {
      iters = 1'000'000;
    }
  }

  std::uint32_t rng = 0x51ED270Bu ^ (static_cast<std::uint32_t>(argc) * 0x85EBCA6Bu +
                                     0xC2B2AE35u);
  constexpr std::size_t kVals = 1024;
  constexpr double kConsts[] = {500.0, 1000.0, 2000.0, 3200.0};
  std::vector<double> power_w(kVals);
  std::vector<double> time_s(kVals);
  std::vector<double> imp_per_kwh(kVals);
  for (std::size_t k = 0; k < kVals; ++k) {
    power_w[k] = 200.0 + (xorshift(rng) % 280000) / 100.0; // 200..3000 W
    time_s[k] = 60.0 + (xorshift(rng) % 714000) / 100.0;   // 60..7200 s
    imp_per_kwh[k] = kConsts[xorshift(rng) % 4];
  }

  auto t0 = std::chrono::high_resolution_clock::now();
  double sink = 0.0;
  for (std::size_t n = 0; n < iters; ++n) {
    const std::size_t k = n % kVals;
    const auto r = wengine::solvers::S0PulseSolver::solve_full(power_w[k], time_s[k],
                                                              imp_per_kwh[k]);
    sink += r.pulses + r.freq_hz + r.energy_j;
  }
  do_not_optimize(sink);
  auto t1 = std::chrono::high_resolution_clock::now();
  const auto ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
  std::cout << "bench_s0: " << iters << " iters, "
            << static_cast<double>(ns) / static_cast<double>(iters) << " ns/iter (sink=" << sink
            << ")\n";
  return 0;
}
