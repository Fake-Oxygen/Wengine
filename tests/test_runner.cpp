#include "wengine/core/runner.hpp"
#include "wengine/signal/types.hpp"
#include "wengine/solvers/signal.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
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

// Mutates live state by an exact delta and counts invocations, so partial
// application of a failed step is observable without FP subtlety.
struct Mutator : core::ISolver {
  int calls{0};
  std::uint32_t slot{0};

  [[nodiscard]] std::string_view name() const noexcept override { return "mut"; }

  core::Status step(world::State &s, double) noexcept override {
    ++calls;
    s.particles.pos_x[slot] += 1.0;
    return core::Status::success();
  }
};

// Fails every step call (registered after the mutator: a later solver).
struct FailNow : core::ISolver {
  int calls{0};

  [[nodiscard]] std::string_view name() const noexcept override { return "failnow"; }

  core::Status step(world::State &, double) noexcept override {
    ++calls;
    return {core::StatusCode::kInternal};
  }
};

// Counts invocations without touching state (proves the latch invokes
// nothing on retries).
struct Counter : core::ISolver {
  int calls{0};

  [[nodiscard]] std::string_view name() const noexcept override { return "counter"; }

  core::Status step(world::State &, double) noexcept override {
    ++calls;
    return core::Status::success();
  }
};

bool stop_after_three() noexcept {
  static int checks = 0;
  return ++checks > 3;
}

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

void test_failure_latch() {
  // A later solver fails: the failed step freezes time/index but keeps the
  // earlier solver's mutation (partial state, not a completed tick).
  // Retries return the same failure without invoking solvers or emitting;
  // only a rebuilt Runner steps again.
  core::Runner runner{{.dt = 0.01}};
  Mutator mut;
  FailNow fail;
  RecSink sink;
  runner.add_solver(mut);
  runner.add_solver(fail);
  runner.add_sink(sink);
  const world::Entity e = runner.state().spawn_entity();
  runner.state().particles.attach(e.index);
  mut.slot = e.index;

  const core::Status first = runner.run(1);
  assert(!first.ok() && first.code == core::StatusCode::kInternal);
  assert(runner.state().step_index == 0 && runner.state().sim_time == 0.0);
  assert(mut.calls == 1 && fail.calls == 1);
  assert(runner.state().particles.pos_x[e.index] == 1.0); // partial kept
  assert(sink.snaps.empty()); // failed step emits nothing

  for (int k = 0; k < 3; ++k) {
    const core::Status retry = runner.run(1);
    assert(!retry.ok() && retry.code == first.code);
  }
  assert(mut.calls == 1 && fail.calls == 1); // nothing invoked again
  assert(runner.state().particles.pos_x[e.index] == 1.0); // frozen
  assert(sink.snaps.empty());
  assert(!runner.run(0).ok()); // latch covers even no-op calls
  const core::Status adv = runner.advance(1.0);
  assert(!adv.ok() && adv.code == first.code);
  assert(mut.calls == 1 && fail.calls == 1);

  // Recovery is a rebuild: a fresh identical runner replays the failure
  // deterministically (latch is per-Runner, cleared by replacement).
  core::Runner fresh{{.dt = 0.01}};
  Mutator mut2;
  FailNow fail2;
  fresh.add_solver(mut2);
  fresh.add_solver(fail2);
  const world::Entity e2 = fresh.state().spawn_entity();
  fresh.state().particles.attach(e2.index);
  mut2.slot = e2.index;
  const core::Status fresh_first = fresh.run(1);
  assert(!fresh_first.ok() && fresh_first.code == first.code);
  assert(mut2.calls == 1); // solvers run again after rebuild
  assert(fresh.state() == runner.state());
}

void test_latch_noise_then_failing_expression() {
  // Noise (slot 0) advances before the later expression (slot 1) fails, so
  // the failed step advances the noise RNG exactly once while the clock,
  // the expression value, solvers, and emission stay frozen past it.
  core::Runner runner{{.dt = 0.01}};
  Counter counter;
  solvers::SignalSolver signal;
  runner.add_solver(counter);
  runner.add_solver(signal);
  signal::SignalParams noise;
  noise.kind = signal::SignalKind::kNoise;
  const world::Entity n = runner.state().spawn_entity();
  assert(world::configure_source(n, runner.state().signals, noise, 0.0).ok());
  const auto parsed = signal::parse_expression("sqrt(0.5-t)");
  assert(parsed.ok());
  signal::SignalParams expr;
  expr.kind = signal::SignalKind::kExpression;
  expr.expr = parsed.value;
  const world::Entity x = runner.state().spawn_entity();
  assert(world::configure_source(x, runner.state().signals, expr, 0.0).ok());
  RecSink sink;
  runner.add_sink(sink);

  // Drive one exact step at a time to the deterministic failure point.
  core::Status st = core::Status::success();
  std::uint64_t prev_rng = 0;
  double prev_expr = 0.0;
  for (int k = 0; k < 100 && st.ok(); ++k) {
    prev_rng = runner.state().signals.rng[n.index];
    prev_expr = runner.state().signals.value[x.index];
    st = runner.run(1);
  }
  assert(!st.ok() && st.code == core::StatusCode::kInvalidArgument);
  const std::uint64_t failed_step = runner.state().step_index + 1;
  assert(failed_step > 1);
  assert(sink.snaps.size() == runner.state().step_index); // nothing emitted
  assert(runner.state().signals.rng[n.index] == signal::rng_next(prev_rng));
  assert(runner.state().signals.value[x.index] == prev_expr);
  const std::uint64_t rng_at_failure = runner.state().signals.rng[n.index];
  const int calls_at_failure = counter.calls;

  for (int k = 0; k < 3; ++k) {
    const core::Status retry = runner.run(1);
    assert(!retry.ok() && retry.code == st.code);
  }
  assert(runner.state().signals.rng[n.index] == rng_at_failure); // frozen
  assert(counter.calls == calls_at_failure);
  assert(sink.snaps.size() == runner.state().step_index);

  // Rebuild replays the successful prefix bit-identically, then fails at
  // the same step with the same partial mutation.
  core::Runner replay{{.dt = 0.01}};
  Counter counter2;
  solvers::SignalSolver signal2;
  replay.add_solver(counter2);
  replay.add_solver(signal2);
  const world::Entity n2 = replay.state().spawn_entity();
  assert(world::configure_source(n2, replay.state().signals, noise, 0.0).ok());
  const world::Entity x2 = replay.state().spawn_entity();
  assert(world::configure_source(x2, replay.state().signals, expr, 0.0).ok());
  RecSink sink2;
  replay.add_sink(sink2);
  assert(replay.run(failed_step - 1).ok());
  assert(sink2.snaps == sink.snaps); // bit-identical successful prefix
  const core::Status st2 = replay.run(1);
  assert(!st2.ok() && st2.code == st.code);
  assert(replay.state().signals.rng[n2.index] == rng_at_failure);

  // A rebuilt run with a non-failing expression completes past the point.
  core::Runner fixed{{.dt = 0.01}};
  solvers::SignalSolver signal3;
  fixed.add_solver(signal3);
  const world::Entity n3 = fixed.state().spawn_entity();
  assert(world::configure_source(n3, fixed.state().signals, noise, 0.0).ok());
  const auto parsed_ok = signal::parse_expression("t+1");
  assert(parsed_ok.ok());
  signal::SignalParams expr_ok;
  expr_ok.kind = signal::SignalKind::kExpression;
  expr_ok.expr = parsed_ok.value;
  const world::Entity x3 = fixed.state().spawn_entity();
  assert(world::configure_source(x3, fixed.state().signals, expr_ok, 0.0).ok());
  assert(fixed.run(failed_step).ok());
  assert(fixed.state().step_index == failed_step);
}

void test_nonfinite_dt_rejected() {
  // NaN/infinite dt and wall-frame durations reject with kInvalidArgument
  // before any solver runs; State stays bit-identical.
  const double inf = std::numeric_limits<double>::infinity();
  const double nan = std::numeric_limits<double>::quiet_NaN();
  for (const double bad_dt : {inf, -inf, nan}) {
    core::Runner runner{{.dt = bad_dt}};
    Counter solver;
    runner.add_solver(solver);
    const world::State before = runner.state();
    const core::Status st = runner.run(1);
    assert(!st.ok() && st.code == core::StatusCode::kInvalidArgument);
    assert(solver.calls == 0 && runner.state() == before);
    assert(!runner.advance(0.01).ok()); // same config, same rejection
    assert(solver.calls == 0 && runner.state() == before);
  }
  core::Runner runner{{.dt = 0.01}};
  Counter solver;
  runner.add_solver(solver);
  const world::State before = runner.state();
  assert(!runner.advance(inf).ok());
  assert(!runner.advance(nan).ok());
  assert(solver.calls == 0 && runner.state() == before);
  assert(runner.run(1).ok()); // valid calls still step after rejections
  assert(solver.calls == 1);
}

void test_clock_exhaustion_rejected() {
  // Step-index overflow, non-finite next time, and dt-below-resolution
  // reject with kOutOfRange before solvers run or sinks emit. The checks
  // are pure pre-checks (not latched): restoring the clock steps again.
  const double max_d = std::numeric_limits<double>::max();
  const double tiny = std::numeric_limits<double>::denorm_min();
  const std::uint64_t max_u = std::numeric_limits<std::uint64_t>::max();

  core::Runner runner{{.dt = 0.01}};
  Counter solver;
  RecSink sink;
  runner.add_solver(solver);
  runner.add_sink(sink);

  runner.state().step_index = max_u; // index range exhausted
  const world::State overflowed = runner.state();
  const core::Status of = runner.run(1);
  assert(!of.ok() && of.code == core::StatusCode::kOutOfRange);
  assert(solver.calls == 0 && sink.snaps.empty());
  assert(runner.state() == overflowed);

  runner.state().step_index = 7;
  runner.state().sim_time = max_d; // max + dt is non-finite
  const world::State saturated = runner.state();
  const core::Status st = runner.run(1);
  assert(!st.ok() && st.code == core::StatusCode::kOutOfRange);
  assert(solver.calls == 0 && sink.snaps.empty());
  assert(runner.state() == saturated);

  // Not latched: a restored clock steps again on the same Runner.
  runner.state().sim_time = 0.07;
  assert(runner.run(1).ok());
  assert(solver.calls == 1 && runner.state().step_index == 8);
  assert(sink.snaps.size() == 1);

  // Positive dt below clock resolution at the current time.
  core::Runner stuck{{.dt = tiny}};
  Counter solver2;
  stuck.add_solver(solver2);
  stuck.state().sim_time = 1.0; // 1.0 + tiny == 1.0
  const world::State frozen = stuck.state();
  const core::Status sr = stuck.run(3);
  assert(!sr.ok() && sr.code == core::StatusCode::kOutOfRange);
  assert(solver2.calls == 0 && stuck.state() == frozen);

  // Valid extremes still step exactly as before.
  core::Runner huge{{.dt = 1e308}};
  Counter solver3;
  huge.add_solver(solver3);
  assert(huge.run(1).ok());
  assert(huge.state().sim_time == 1e308 && huge.state().step_index == 1);
  core::Runner denorm{{.dt = tiny}}; // advances from a zero clock
  Counter solver4;
  denorm.add_solver(solver4);
  assert(denorm.run(1).ok());
  assert(denorm.state().sim_time == tiny && denorm.state().step_index == 1);
}

void test_stop_callback() {
  core::Runner runner{{.dt = 0.01}};
  CountingSolver solver;
  runner.add_solver(solver);
  assert(runner.run_until(100, stop_after_three).ok());
  assert(solver.calls == 3);
  assert(runner.state().step_index == 3);
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
  test_nonfinite_dt_rejected();
  test_clock_exhaustion_rejected();
  test_failure_latch();
  test_latch_noise_then_failing_expression();
  test_stop_callback();
  test_error_propagation();
  test_accumulator();
  test_budgets();
  test_bad_config();
  std::cout << "test_runner: OK\n";
  return 0;
}
