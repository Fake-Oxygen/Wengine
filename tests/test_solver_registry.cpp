#include "wengine/core/runner.hpp"
#include "wengine/solvers/solver_registry.hpp"

#include <cassert>
#include <cmath>
#include <iostream>

using namespace wengine;

namespace {

constexpr bool near(double a, double b, double eps = 1e-9) {
  return std::fabs(a - b) <= eps * (1.0 + std::fabs(b));
}

void test_registry_compile_time() {
  static_assert(solvers::SolverRegistry::find("projectile") != nullptr);
  static_assert(solvers::SolverRegistry::find("nope") == nullptr);
  static_assert(solvers::SolverRegistry::all().size() == 1);
  static_assert(!solvers::SolverRegistry::all()[0].description.empty());
  static_assert(solvers::SolverRegistry::find("projectile")->create != nullptr);
}

void test_factory_and_runner_integration() {
  // Registry -> factory -> runner: the catalog path drives simulation.
  const solvers::SolverDef *def = solvers::SolverRegistry::find("projectile");
  assert(def != nullptr);
  std::unique_ptr<core::ISolver> solver = def->create();
  assert(solver && solver->name() == "projectile");

  core::Runner runner{{.dt = 0.01}};
  runner.add_solver(*solver);
  world::Entity e = runner.state().spawn_entity();
  runner.state().particles.attach(e.index);
  runner.state().particles.pos_x[e.index] = 0.0;
  runner.state().particles.vel_x[e.index] = 10.0;
  assert(runner.run(100).ok());
  assert(runner.state().step_index == 100);
  // Default gravity, constant vx: x = 100 * 10 * 0.01.
  assert(near(runner.state().particles.pos_x[e.index], 10.0, 1e-6));
}

} // namespace

int main() {
  test_registry_compile_time();
  test_factory_and_runner_integration();
  std::cout << "test_solver_registry: OK\n";
  return 0;
}
