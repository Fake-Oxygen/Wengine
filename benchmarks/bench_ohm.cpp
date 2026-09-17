#include "wengine/solvers/ohm_law.hpp"

#include <chrono>
#include <iostream>

using namespace wengine::units::literals;

int main() {
  constexpr int kIters = 1'000'000;
  auto t0 = std::chrono::high_resolution_clock::now();
  double sink = 0.0;
  for (int i = 0; i < kIters; ++i) {
    const auto r = wengine::solvers::OhmLawSolver::solve_from_ui(12.4_V, 0.54_A);
    sink += r.resistance.get();
  }
  auto t1 = std::chrono::high_resolution_clock::now();
  const auto ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
  std::cout << "bench_ohm: " << kIters << " iters, "
            << static_cast<double>(ns) / kIters << " ns/iter (sink=" << sink
            << ")\n";
  return 0;
}
