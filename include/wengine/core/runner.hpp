#pragma once

// Fixed-step simulation runner: owns world::State, advances sim_time and
// step_index, calls registered ISolver steps in order, and emits snapshots
// to attached ISink lists at a decimated rate.
//
// Two entry points:
//   run(steps)            exact N steps; deterministic batch/headless/tests.
//   advance(frame_dt)     real-time accumulator: folds variable frame time
//                         into fixed-dt chunks, capped per call (spiral-of-
//                         death guard); leftover fraction is kept.
// A failed solver step aborts the call and propagates its Status; the
// failed step advances neither time nor index and emits nothing.
// Sinks are best-effort observers: they must not throw.

#include "wengine/core/solver.hpp"
#include "wengine/emit/sink.hpp"
#include "wengine/world/state.hpp"

#include <cstdint>
#include <vector>

namespace wengine::core {

struct RunnerConfig {
  double dt = 0.01; // fixed step [s], must be > 0
  std::uint64_t emit_every = 1; // snapshot every N steps, 0 = never
  std::uint64_t max_substeps = 8; // per advance() call (stall guard)
};

class Runner {
public:
  explicit Runner(RunnerConfig cfg = {}) : config_(cfg) {}

  void add_solver(ISolver &solver) { solvers_.push_back(&solver); }
  void add_sink(emit::ISink &sink) { sinks_.push_back(&sink); }

  [[nodiscard]] const RunnerConfig &config() const noexcept { return config_; }
  [[nodiscard]] world::State &state() noexcept { return state_; }
  [[nodiscard]] const world::State &state() const noexcept { return state_; }

  // Advance exactly `steps` fixed steps (0 is a no-op success).
  Status run(std::uint64_t steps) {
    return run_until(steps, nullptr);
  }

  // Run until `steps` are complete or the optional callback requests a stop.
  // The callback is checked at fixed-step boundaries, before the next solver
  // invocation, so interruption never leaves a partially advanced step.
  Status run_until(std::uint64_t steps,
                   bool (*stop_requested)() noexcept) {
    if (!(config_.dt > 0.0)) {
      return {StatusCode::kInvalidArgument};
    }
    for (std::uint64_t k = 0; k < steps; ++k) {
      if (stop_requested != nullptr && stop_requested()) {
        break;
      }
      const Status st = step_once();
      if (!st.ok()) {
        return st;
      }
    }
    return Status::success();
  }

  // Fold `frame_dt_seconds` of wall time into fixed steps. Keeps the
  // fractional remainder for the next call; drops backlog beyond
  // max_substeps worth after stalls.
  Status advance(double frame_dt_seconds) {
    if (!(config_.dt > 0.0)) {
      return {StatusCode::kInvalidArgument};
    }
    if (!(frame_dt_seconds >= 0.0)) {
      return {StatusCode::kInvalidArgument};
    }
    acc_ += frame_dt_seconds;
    const double cap = config_.dt * static_cast<double>(config_.max_substeps);
    if (acc_ > cap) {
      acc_ = cap;
    }
    while (acc_ >= config_.dt) {
      acc_ -= config_.dt;
      const Status st = step_once();
      if (!st.ok()) {
        acc_ = 0.0;
        return st;
      }
    }
    return Status::success();
  }

private:
  Status step_once() {
    for (ISolver *s : solvers_) {
      const Status st = s->step(state_, config_.dt);
      if (!st.ok()) {
        return st;
      }
    }
    state_.sim_time += config_.dt;
    state_.step_index += 1;
    if (config_.emit_every != 0 && state_.step_index % config_.emit_every == 0) {
      const world::Snapshot snap = world::take_snapshot(state_);
      for (emit::ISink *sink : sinks_) {
        sink->on_snapshot(snap);
      }
    }
    return Status::success();
  }

  RunnerConfig config_{};
  world::State state_{};
  std::vector<ISolver *> solvers_;
  std::vector<emit::ISink *> sinks_;
  double acc_{0.0};
};

} // namespace wengine::core
