#include "wengine/solvers/ohm_law.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

using namespace wengine::units::literals;

// Prevents the optimizer from constant-folding or discarding results.
// Inputs below are derived from argc (a runtime value), so the compiler
// must emit the real divide + multiply every iteration.
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

  // Opaque inputs: runtime-generated U in [1, 24] V, I in [0.1, 5] A.
  // Seeded from argc so the values are unknown at compile time.
  std::uint32_t rng = 0x9E3779B9u ^ (static_cast<std::uint32_t>(argc) * 0x85EBCA6Bu +
                                     0xC2B2AE35u);
  constexpr std::size_t kVals = 1024;
  std::vector<double> u_v(kVals);
  std::vector<double> i_a(kVals);
  for (std::size_t k = 0; k < kVals; ++k) {
    u_v[k] = 1.0 + (xorshift(rng) % 23000) / 1000.0;        // 1..24 V
    i_a[k] = 0.1 + (xorshift(rng) % 4900) / 1000.0;         // 0.1..5 A
  }

  auto t0 = std::chrono::high_resolution_clock::now();
  double sink = 0.0;
  for (std::size_t n = 0; n < iters; ++n) {
    const std::size_t k = n % kVals;
    const auto r = wengine::solvers::OhmLawSolver::solve_from_ui(
        wengine::units::Value<wengine::units::ElectricPotential>(u_v[k]),
        wengine::units::Value<wengine::units::ElectricCurrent>(i_a[k]));
    sink += r.resistance.get() + r.power.get();
  }
  do_not_optimize(sink);
  auto t1 = std::chrono::high_resolution_clock::now();
  const auto ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
  std::cout << "bench_ohm: " << iters << " iters, "
            << static_cast<double>(ns) / static_cast<double>(iters) << " ns/iter (sink=" << sink
            << ")\n";
  return 0;
}
