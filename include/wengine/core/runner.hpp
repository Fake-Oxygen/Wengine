#pragma once

/**
 * @file
 * The fixed-step simulation engine loop: owns one world::State, advances
 * sim_time/step_index by a validated dt, invokes registered ISolver
 * steps in registration order, and emits snapshots to attached sinks at
 * a decimated rate. Driven headless (CLI `sim run`), by the GUI frame
 * loop, and by tests. The Runner owns no solver and no sink — both are
 * non-owning references the app edge supplies and must re-attach after
 * every rebuild; this header must never grow I/O, wall-clock input, or
 * frontend types.
 */

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

/**
 * @brief Fixed-step loop tuning: dt, snapshot cadence, and the
 *        real-time stall guard.
 *
 * @note Validated through validate_runner_config so every frontend
 *       (C++ API, CLI, GUI) rejects the same inputs identically.
 */
struct RunnerConfig {
  /**
   * @brief Fixed step size in seconds (_s); must be finite and > 0.
   */
  double dt = 0.01; // fixed step [s], must be finite and > 0
  /**
   * @brief Emit a snapshot every N completed steps; 0 = never.
   *
   * @note Emission cadence is one of the four independent clocks
   *       (simulation dt, emitted sample rate, wall-clock packet rate,
   *       UI refresh rate) — it is not the waveform frequency.
   */
  std::uint64_t emit_every = 1; // snapshot every N steps, 0 = never
  /**
   * @brief Maximum fixed steps consumed by one advance() call
   *        (spiral-of-death guard for real-time driving).
   */
  std::uint64_t max_substeps = 8; // per advance() call (stall guard)
};

/**
 * @brief Shared loop-config edge: the Runner entries and the
 *        configured-run builder (PHYSI-35, all frontends) validate
 *        through this one function so C++, CLI, and GUI reject
 *        identical inputs identically.
 *
 * @param[in] cfg loop configuration to validate.
 * @return kOk when dt is finite and > 0; kInvalidArgument when dt is
 *         NaN, <= 0, or infinite (state unchanged).
 */
[[nodiscard]] inline Status validate_runner_config(const RunnerConfig &cfg) noexcept {
  if (!(cfg.dt > 0.0) || !std::isfinite(cfg.dt)) {
    return {StatusCode::kInvalidArgument}; // NaN, <= 0, or infinite step
  }
  return Status::success();
}

/**
 * @brief Fixed-step simulation runner: clock, solvers, and snapshot
 *        emission over one owned world::State.
 *
 * @note Solvers run in registration order and sinks are fanned out in
 *       attach order every emit step — both orders are part of the
 *       deterministic-output contract. The Runner performs no I/O and
 *       no wall-clock reads inside run(); failure behavior (latched
 *       solver failures, clock-range guards) is specified in the file
 *       header above.
 */
class Runner {
public:
  /**
   * @brief Construct with the loop configuration.
   *
   * @param[in] cfg loop tuning (dt, emit_every, max_substeps); validated
   *            on every run()/advance() entry.
   */
  explicit Runner(RunnerConfig cfg = {}) : config_(cfg) {}

  /**
   * @brief Register a solver to be stepped once per fixed step.
   *
   * @param[in] solver solver to register; called in registration order.
   * @note Non-owning: the caller owns the solver and must keep it alive
   *       for the Runner's lifetime and re-attach it after any rebuild
   *       of the configured run.
   */
  void add_solver(ISolver &solver) { solvers_.push_back(&solver); }
  /**
   * @brief Register a sink to receive snapshots at the emit cadence.
   *
   * @param[in] sink best-effort observer; must not throw.
   * @note Non-owning: the app edge owns the sink and re-attaches it
   *       after every rebuild — sinks stay unaware of rebuilds by
   *       construction.
   */
  void add_sink(emit::ISink &sink) { sinks_.push_back(&sink); }

  /**
   * @brief The loop configuration supplied at construction.
   *
   * @return const reference to the stored RunnerConfig.
   */
  [[nodiscard]] const RunnerConfig &config() const noexcept { return config_; }
  /**
   * @brief Live simulation state (mutable access for setup and solvers).
   *
   * @return reference to the owned world::State.
   */
  [[nodiscard]] world::State &state() noexcept { return state_; }
  /**
   * @brief Live simulation state (read-only view for observers).
   *
   * @return const reference to the owned world::State.
   */
  [[nodiscard]] const world::State &state() const noexcept { return state_; }

  /**
   * @brief Advance exactly `steps` fixed steps (0 is a no-op success).
   *
   * @param[in] steps exact number of fixed steps to complete.
   * @return kOk when all steps completed; kInvalidArgument when the
   *         stored config fails validation; kOutOfRange on a
   *         clock-range guard (step-index or sim-time exhaustion,
   *         rejected before any solver mutation); the latched solver
   *         failure Status from this or an earlier call otherwise.
   * @note Deterministic: identical config and steps produce identical
   *       state trajectories and snapshots; no wall-clock input.
   */
  Status run(std::uint64_t steps) {
    return run_until(steps, nullptr);
  }

  /**
   * @brief Run until `steps` are complete or the optional callback
   *        requests a stop.
   *
   * @param[in] steps maximum number of fixed steps to complete.
   * @param[in] stop_requested optional predicate polled at fixed-step
   *            boundaries, before the next solver invocation; null
   *            runs all steps. Requesting a stop returns kOk for the
   *            partial run (interruption is not an error).
   * @return kOk when the run completed or was stopped; kInvalidArgument
   *         on invalid config; kOutOfRange on a clock-range guard;
   *         otherwise the latched solver failure Status.
   * @note The callback is checked at fixed-step boundaries so
   *       interruption never leaves a partially advanced step. A
   *       latched solver failure (see file header) returns immediately.
   */
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

  /**
   * @brief Fold `frame_dt_seconds` of wall time into fixed steps.
   *
   * @param[in] frame_dt_seconds elapsed wall-clock time in seconds (_s);
   *            must be finite and >= 0. This is frame time, not the
   *            simulation dt and not the emit cadence (independent
   *            clocks, see RunnerConfig::emit_every).
   * @return kOk when the fold completed; kInvalidArgument when the
   *         config is invalid or the wall time is NaN/negative
   *         (nothing consumed); kOutOfRange on a clock-range guard;
   *         otherwise the latched solver failure Status (the remainder
   *         accumulator is reset so a rebuilt run starts clean).
   * @note Keeps the fractional remainder for the next call and drops
   *       backlog beyond max_substeps worth after stalls
   *       (spiral-of-death guard). Non-deterministic by design: it
   *       converts wall time into the deterministic fixed-step
   *       sequence. A latched solver failure (see file header) returns
   *       immediately without consuming wall time.
   */
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
