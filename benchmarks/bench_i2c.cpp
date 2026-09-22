#include "wengine/solvers/i2c.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

// Benchmark for the I2C bus solver: solve_pullup + solve_series over
// runtime-generated bus points. e12_nearest carries data-dependent decade
// loops, so varying Rp exercises the real branchy path.

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
  std::size_t iters = 200'000;
  if (argc > 1) {
    iters = static_cast<std::size_t>(std::strtoull(argv[1], nullptr, 10));
    if (iters == 0) {
      iters = 200'000;
    }
  }

  using wengine::solvers::I2cMode;
  std::uint32_t rng = 0x1B873593u ^ (static_cast<std::uint32_t>(argc) * 0x85EBCA6Bu +
                                     0xC2B2AE35u);
  constexpr std::size_t kVals = 1024;
  constexpr double kVdd[] = {1.8, 3.3, 5.0};
  std::vector<double> vdd_v(kVals);
  std::vector<double> cb_f(kVals);
  std::vector<I2cMode> modes(kVals);
  for (std::size_t k = 0; k < kVals; ++k) {
    vdd_v[k] = kVdd[xorshift(rng) % 3];
    cb_f[k] = (50.0 + (xorshift(rng) % 5000) / 10.0) * 1e-12; // 50..550 pF
    modes[k] = static_cast<I2cMode>(xorshift(rng) % 3);
  }

  auto t0 = std::chrono::high_resolution_clock::now();
  double sink = 0.0;
  for (std::size_t n = 0; n < iters; ++n) {
    const std::size_t k = n % kVals;
    const auto lim = wengine::solvers::i2c_mode_limits(modes[k]);
    const double vol = wengine::solvers::i2c_default_vol_v(vdd_v[k]);
    const double iol = wengine::solvers::i2c_default_iol_a(vdd_v[k], modes[k]);
    const auto pu = wengine::solvers::I2cBusSolver::solve_pullup(
        vdd_v[k], cb_f[k], lim.tr_max_s, vol, iol);
    const auto se = wengine::solvers::I2cBusSolver::solve_series(
        vdd_v[k], vol, iol, pu.rp_used_ohm, cb_f[k], lim.tr_max_s, lim.tf_max_s);
    sink += pu.rp_used_ohm + pu.actual_tr_s + se.tr_with_rs_s + se.tf_est_s;
  }
  do_not_optimize(sink);
  auto t1 = std::chrono::high_resolution_clock::now();
  const auto ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
  std::cout << "bench_i2c: " << iters << " iters, "
            << static_cast<double>(ns) / static_cast<double>(iters) << " ns/iter (sink=" << sink
            << ")\n";
  return 0;
}
