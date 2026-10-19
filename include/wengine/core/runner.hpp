#pragma once

// Fixed-step simulation runner: owns world::State, advances sim_time and
// step_index, calls registered ISolver steps in order, and emits snapshots
// to attached ISink lists at a decimated rate.
//
// Emission reuses one Snapshot buffer (no per-emit allocation after
// warmup): each emit step refills it via world::fill_snapshot and fans the
// reference out to every sink synchronously. The observer contract in
// emit/sink.hpp applies — sinks copy or enqueue-owned before returning and
// never reenter this Runner from inside on_snapshot.
//
// Two entry points:
//   run(steps)            exact N steps; deterministic batch/headless/tests.
//   advance(frame_dt)     real-time accumulator: folds variable frame time
//                         into fixed-dt chunks, capped per call (spiral-of-
//                         death guard); leftover fraction is kept.
// A failed solver step aborts the call and propagates its Status; the
// failed step advances neither time nor index and emits nothing. The
// failure is latched: every later run()/advance() returns the stored
// Status immediately (even run(0)) without invoking solvers or emitting.
// Only replacing the Runner (rebuilding/restoring the configured run)
// clears it — there is deliberately no rollback and no resume.
// Clock-range exhaustion (step-index overflow, non-finite next time, or a
// positive dt too small to advance the clock) rejects the step with
// kOutOfRange before any solver mutation, leaving State bit-identical;
// these pure pre-checks re-derive deterministically and are not latched.
// PARTIAL STATE: freezing time/index does not undo solver work. Solvers
// before the failing one in registration order have already mutated the
// pools for the failed step (particle positions, noise RNG, ...), so the
// live State is not a completed simulation tick and must never be read as
// one. Retrying the same Runner cannot complete the tick; rebuild from a
// restored state instead. Strong transactional stepping (atomic
// accept/reject per step) is a separate design for domains that need
// recoverable rejection — this latch only prevents compounding a partial
// step with more steps.
// Sinks are best-effort observers: they must not throw.

#include "wengine/core/solver.hpp"
#include "wengine/emit/sink.hpp"
#include "wengine/world/state.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace wengine::core {

struct RunnerConfig {
  double dt = 0.01; // fixed step [s], must be finite and > 0
  std::uint64_t emit_every = 1; // snapshot every N steps, 0 = never
  std::uint64_t max_substeps = 8; // per advance() call (stall guard)
};

// Shared loop-config edge: the Runner entries and the configured-run
// builder (PHYSI-35, all frontends) validate through this one function so
// C++, CLI, and GUI reject identical inputs identically.
[[nodiscard]] inline Status validate_runner_config(const RunnerConfig &cfg) noexcept {
  if (!(cfg.dt > 0.0) || !std::isfinite(cfg.dt)) {
    return {StatusCode::kInvalidArgument}; // NaN, <= 0, or infinite step
  }
  return Status::success();
}

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
  // A latched solver failure (see header comment) returns immediately.
  Status run_until(std::uint64_t steps, bool (*stop_requested)() noexcept) {
    if (!failure_.ok()) {
      return failure_;
    }
    const Status cfg_st = validate_runner_config(config_);
    if (!cfg_st.ok()) {
      return cfg_st;
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
  // max_substeps worth after stalls. A latched solver failure (see header
  // comment) returns immediately without consuming wall time.
  Status advance(double frame_dt_seconds) {
    if (!failure_.ok()) {
      return failure_;
    }
    const Status cfg_st = validate_runner_config(config_);
    if (!cfg_st.ok()) {
      return cfg_st;
    }
    if (!(frame_dt_seconds >= 0.0) || !std::isfinite(frame_dt_seconds)) {
      return {StatusCode::kInvalidArgument}; // NaN or infinite wall time
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
    // Clock-range guard, evaluated before any solver mutation so a
    // rejected step leaves State bit-identical. Pure pre-checks in
    // (sim_time, step_index, dt): deterministic to re-derive, hence not
    // latched like solver failures (see header comment).
    if (state_.step_index == std::numeric_limits<std::uint64_t>::max()) {
      return {StatusCode::kOutOfRange}; // index range exhausted
    }
    const double next_time = state_.sim_time + config_.dt;
    if (!std::isfinite(next_time)) {
      return {StatusCode::kOutOfRange}; // clock range exhausted
    }
    if (next_time == state_.sim_time) {
      // Positive dt below clock resolution: stepping would spin forever
      // without advancing time.
      return {StatusCode::kOutOfRange};
    }
    for (ISolver *s : solvers_) {
      const Status st = s->step(state_, config_.dt);
      if (!st.ok()) {
        failure_ = st; // latch: no retry, no rollback (see header comment)
        return st;
      }
    }
    state_.sim_time = next_time;
    state_.step_index += 1;
    if (config_.emit_every != 0 && !sinks_.empty() && state_.step_index % config_.emit_every == 0) {
      world::fill_snapshot(state_, emit_snap_);
      for (emit::ISink *sink : sinks_) {
        sink->on_snapshot(emit_snap_);
      }
    }
    return Status::success();
  }

  RunnerConfig config_{};
  world::State state_{};
  Status failure_{}; // latched solver-step failure (kOk until the first one)
  world::Snapshot emit_snap_; // reused emission buffer (see header comment)
  std::vector<ISolver *> solvers_;
  std::vector<emit::ISink *> sinks_;
  double acc_{0.0};
};

} // namespace wengine::core
