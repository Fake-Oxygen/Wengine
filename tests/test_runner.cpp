#include "wengine/core/runner.hpp"

#include <cassert>
#include <cmath>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

using namespace wengine;

namespace {

constexpr bool near(double a, double b, double eps = 1e-12) {
  return std::fabs(a - b) <= eps * (1.0 + std::fabs(b));
}

// Dummy stepped solver: counts calls, records order, spawns one particle
// per step so shared-state mutation is visible to all solvers.
struct CountingSolver : core::ISolver {
  int calls{0};
  std::vector<std::string> *order{nullptr};

  [[nodiscard]] std::string_view name() const noexcept override { return "count"; }

  core::Status step(world::State &s, double dt) noexcept override {
    ++calls;
    if (order != nullptr) {
      order->emplace_back(name());
    }
    const world::Entity e = s.spawn_entity();
    s.particles.attach(e.index);
    s.particles.pos_x[e.index] = dt;
    return core::Status::success();
  }
};

struct FailingSolver : core::ISolver {
  int calls{0};
  int fail_on{3};

  [[nodiscard]] std::string_view name() const noexcept override { return "fail"; }

  core::Status step(world::State &, double) noexcept override {
    ++calls;
    if (calls >= fail_on) {
      return {core::StatusCode::kInternal};
    }
    return core::Status::success();
  }
};

struct RecSink : emit::ISink {
  std::vector<world::Snapshot> snaps;
  void on_snapshot(const world::Snapshot &s) override { snaps.push_back(s); }
};

void test_exact_stepping() {
  core::Runner runner{{.dt = 0.01}};
  CountingSolver a;
  CountingSolver b;
  std::vector<std::string> order;
  a.order = &order;
  b.order = &order;
  runner.add_solver(a);
  runner.add_solver(b);

  const core::Status st = runner.run(10);
  assert(st.ok());
  assert(a.calls == 10 && b.calls == 10);
  assert(runner.state().step_index == 10);
  assert(near(runner.state().sim_time, 0.1));
  // Both solvers see shared state: 20 live particles with dt stamped.
  assert(runner.state().entities.alive_count() == 20);
  assert(runner.state().particles.live_count == 20);
  assert(order.size() == 20);
  for (std::size_t k = 0; k < order.size(); k += 2) {
    assert(order[k] == "count" && order[k + 1] == "count"); // registration order
  }
  // run(0) is a no-op success.
  assert(runner.run(0).ok());
  assert(a.calls == 10);
}

void test_determinism() {
  auto drive = [] {
    core::Runner r{{.dt = 0.005}};
    CountingSolver s;
    r.add_solver(s);
    assert(r.run(7).ok());
    return r;
  };
  const core::Runner r1 = drive();
  const core::Runner r2 = drive();
  assert(r1.state() == r2.state()); // full state equality: clock + entities + pools
}

void test_emit_decimation() {
  core::Runner runner{{.dt = 0.01, .emit_every = 2}};
  CountingSolver s;
  RecSink sink;
  runner.add_solver(s);
  runner.add_sink(sink);
  assert(runner.run(10).ok());
  assert(sink.snaps.size() == 5);
  for (std::size_t k = 0; k < sink.snaps.size(); ++k) {
    assert(sink.snaps[k].step_index == 2 * (k + 1));
    assert(near(sink.snaps[k].sim_time, 0.01 * 2 * (k + 1)));
  }
  // emit_every = 0 disables emission; default 1 emits every step.
  core::Runner quiet{{.dt = 0.01, .emit_every = 0}};
  RecSink none;
  CountingSolver qs;
  quiet.add_solver(qs);
  quiet.add_sink(none);
  assert(quiet.run(3).ok() && none.snaps.empty());

  core::Runner every{{.dt = 0.01}};
  RecSink all;
  CountingSolver es;
  every.add_solver(es);
  every.add_sink(all);
  assert(every.run(3).ok() && all.snaps.size() == 3);
}

void test_error_propagation() {
  core::Runner runner{{.dt = 0.01}};
  FailingSolver f;
  RecSink sink;
  runner.add_solver(f);
  runner.add_sink(sink);
  const core::Status st = runner.run(10);
  assert(!st.ok() && st.code == core::StatusCode::kInternal);
  assert(f.calls == 3);
  // Failed step advances neither time/index nor emission.
  assert(runner.state().step_index == 2);
  assert(near(runner.state().sim_time, 0.02));
  assert(sink.snaps.size() == 2);
}

void test_accumulator() {
  core::Runner runner{{.dt = 0.01}};
  CountingSolver s;
  runner.add_solver(s);
  assert(runner.advance(0.025).ok()); // 2 steps, 0.005 kept
  assert(s.calls == 2 && runner.state().step_index == 2);
  assert(runner.advance(0.005).ok()); // remainder completes 3rd step
  assert(s.calls == 3 && near(runner.state().sim_time, 0.03));
  assert(runner.advance(0.0).ok() && s.calls == 3);
  assert(!runner.advance(-0.001).ok()); // negative frame time rejected
}

void test_budgets() {
  core::Runner runner{{.dt = 0.01, .emit_every = 1, .max_substeps = 2}};
  CountingSolver s;
  runner.add_solver(s);
  assert(runner.advance(1.0).ok()); // capped: exactly 2 substeps, rest dropped
  assert(s.calls == 2 && near(runner.state().sim_time, 0.02));
}

void test_bad_config() {
  core::Runner runner{{.dt = 0.0}};
  CountingSolver s;
  runner.add_solver(s);
  const core::Status a = runner.run(1);
  assert(!a.ok() && a.code == core::StatusCode::kInvalidArgument);
  const core::Status b = runner.advance(0.016);
  assert(!b.ok() && b.code == core::StatusCode::kInvalidArgument);
  assert(s.calls == 0);
}

} // namespace

int main() {
  test_exact_stepping();
  test_determinism();
  test_emit_decimation();
  test_error_propagation();
  test_accumulator();
  test_budgets();
  test_bad_config();
  std::cout << "test_runner: OK\n";
  return 0;
}
