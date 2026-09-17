#include "wengine/solvers/ohm_law.hpp"

#include <cassert>
#include <cmath>
#include <iostream>

using namespace wengine::units::literals;

int main() {
  // solve_from_ui: R = U/I, P = U*I
  {
    const auto r = wengine::solvers::OhmLawSolver::solve_from_ui(12.0_V, 3.0_A);
    assert(std::fabs(r.resistance.get() - 4.0) < 1e-12);
    assert(std::fabs(r.power.get() - 36.0) < 1e-12);
  }
  // solve_from_ir / solve_from_ur round-trip
  {
    const auto r = wengine::solvers::OhmLawSolver::solve_from_ir(2.0_A, 5.0_Ohm);
    assert(std::fabs(r.voltage.get() - 10.0) < 1e-12);
  }
  // constexpr check (compile-time solver, key for perf)
  static_assert(wengine::solvers::OhmLawSolver::solve_from_ui(12.0_V, 3.0_A)
                    .resistance.get() == 4.0);

  std::cout << "test_ohm_law: OK\n";
  return 0;
}
