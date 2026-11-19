#pragma once

/**
 * @file
 * Wall-clock-paced driver over solvers::ConfiguredRun (PHYSI-36): owns
 * only the waiting between step chunks so `sim run` / `sig run` can
 * offer paced mode next to the default fast batch mode. The engine
 * still advances fixed-dt steps with no wall-clock knowledge. Pacing
 * is injectable (Clock template) for deterministic tests. Depends on
 * engine types only — never CLI types; consumed by the paced CLI
 * commands and tests/test_paced_run.cpp.
 */

// Wall-clock-paced driver over solvers::ConfiguredRun (PHYSI-36).
//
// Application-edge pacing: the engine still advances fixed-dt steps with no
// wall-clock knowledge (see core::Runner / ConfiguredRun); this helper owns
// the wait between step chunks so `sim run` / `sig run` can offer an
// optional paced mode next to the default fast batch mode.
//
// Policy (PHYSI-28):
// - Never skips physics steps and never changes dt. Lateness only counts
//   (PacedStats::late_steps / max_late_s); the next chunk runs immediately.
// - Absolute epoch deadlines (epoch + done * period) so sleep jitter never
//   accumulates into drift.
// - Stop checks happen at every chunk boundary and inside every wait slice,
//   so Ctrl-C stays responsive while waiting.
// - The wait slice calls an idle hook (TCP service_pending at the CLI)
//   without generating dummy snapshots.
//
// Clock injection: the driver is templated on a Clock with
//   Clock::time_point, Clock::duration,
//   static time_point Clock::now(),
//   static void Clock::sleep_for(duration).
// SteadyClock below adapts std::chrono::steady_clock; tests inject a fake
// with the same shape for deterministic scheduling checks.

#include "wengine/core/status.hpp"
#include "wengine/solvers/configured_run.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <thread>
#include <utility>

namespace wengine::solvers::paced {

// Wall steps per second (> 0, finite). The CLI defaults paced mode to
// realtime (rate = 1/dt) and lets --rate override it as an absolute speed.
/**
 * @brief Pacing configuration for one paced drive.
 */
struct Config {
  /** @brief Wall steps per second (> 0, finite); realtime is 1/dt. */
  double rate_hz = 0.0;
  /** @brief Steps driven back-to-back per chunk (>= 1; stop granularity is one chunk). */
  std::uint64_t chunk_steps = 1;
};

/** @brief Outcome counters of one paced drive (the finished prefix). */
struct Stats {
  /** @brief Steps actually driven when the drive ended. */
  std::uint64_t steps_done = 0;
  std::uint64_t late_steps = 0; /**< @brief Steps run past their absolute deadline (counted, never skipped). */
  double max_late_s = 0.0;      /**< @brief Worst deadline overshoot, in seconds. */
};

// Rounded whole-nanosecond period for rate_hz; <= 0 when invalid.
/**
 * @brief Convert a pacing rate into its whole-nanosecond step period.
 *
 * @param[in] rate_hz wall steps per second; must be > 0 and finite.
 * @return Rounded period in nanoseconds; <= 0 when rate_hz is invalid
 *         or the period falls outside the 1 ns .. 9e18 ns clock range.
 */
[[nodiscard]] inline std::int64_t period_ns(double rate_hz) noexcept {
  if (!(rate_hz > 0.0) || !(rate_hz < HUGE_VAL)) {
    return 0;
  }
  const double period = 1.0e9 / rate_hz;
  if (!(period >= 1.0) || !(period <= 9.0e18)) {
    return 0;
  }
  return static_cast<std::int64_t>(std::llround(period));
}

/**
 * @brief Validate a paced-run configuration.
 *
 * @param[in] rate_hz wall steps per second (> 0, finite; see period_ns).
 * @param[in] chunk_steps steps per chunk (>= 1).
 * @return kOk when the pacing is realizable; kInvalidArgument when the
 *         period is invalid or chunk_steps is zero.
 */
[[nodiscard]] inline core::Status validate(double rate_hz,
                                           std::uint64_t chunk_steps) noexcept {
  if (period_ns(rate_hz) <= 0 || chunk_steps < 1) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  return core::Status::success();
}

// Production clock: steady (monotonic) time + blocking sleep.
/**
 * @brief Production clock: monotonic steady time plus blocking sleep.
 *
 * Tests inject a fake Clock with the same shape
 * (time_point/duration/now/sleep_for) for deterministic scheduling
 * checks.
 */
struct SteadyClock {
  /** @brief Underlying standard clock (std::chrono::steady_clock). */
  using clock = std::chrono::steady_clock;
  /** @brief Time point type used for absolute chunk deadlines. */
  using time_point = clock::time_point;
  /** @brief Duration type used for periods and wait slices. */
  using duration = clock::duration;
  /** @brief Current monotonic time. @return A steady time_point. */
  static time_point now() noexcept { return clock::now(); }
  /** @brief Block the driver thread for d. @param[in] d wait duration. */
  static void sleep_for(duration d) { std::this_thread::sleep_for(d); }
};

// Drive `run` for total_steps paced at rate_hz wall steps/s from epoch.
// Chunked: each chunk runs back-to-back, then waits for its absolute
// deadline (epoch + steps_done * period). A stop request ends the run with
// success (partial prefix, like Runner::run_until); solver failures abort
// loudly with the solver Status. Stats report the finished prefix.
//
// before_chunk(done) runs on the driver thread before every run.run(n)
// chunk (SIG-10: the control-plane drain lives there, so commands apply at
// exact step boundaries even in paced mode). It cannot fail the run;
// per-line verdicts stay in the control outbox.
/**
 * @brief Drive a ConfiguredRun for total_steps at a wall-clock pace.
 *
 * Each chunk runs back-to-back, then waits for its absolute deadline
 * (epoch + steps_done * period) — absolute deadlines mean sleep jitter
 * never accumulates into drift, and lateness is only counted
 * (late_steps / max_late_s), never skipped and never a dt change. A
 * stop request ends the drive with success (partial prefix, like
 * Runner::run_until); solver failures abort loudly with the solver
 * Status; stats always report the finished prefix.
 *
 * @tparam Clock pacing clock (SteadyClock in production, fakes in tests).
 * @tparam StopFn bool() poll for cooperative stop, checked at chunk
 *         boundaries and inside every wait slice.
 * @tparam IdleFn void() hook called while waiting (TCP service_pending
 *         at the CLI) — never generates dummy snapshots.
 * @tparam StepFn void(uint64_t) pre-chunk hook receiving steps done so far.
 * @param[in,out] run the shared configured run (state persists across calls).
 * @param[in] total_steps total steps to drive; the loop ends early on stop.
 * @param[in] rate_hz wall steps per second (> 0, finite).
 * @param[in] chunk_steps steps per chunk (>= 1; stop granularity).
 * @param[in] epoch pacing origin; deadlines are epoch + done * period.
 * @param[in] stop_requested stop poll (see StopFn).
 * @param[in] idle idle hook (see IdleFn).
 * @param[out] stats filled with the finished prefix's counters.
 * @param[in] before_chunk runs on the driver thread before every chunk
 *            (SIG-10 control-plane drain — commands apply at exact step
 *            boundaries even in paced mode); cannot fail the run.
 * @return kOk on completion or stop request; kInvalidArgument when
 *         (rate_hz, chunk_steps) fails validate; the solver/runner
 *         Status unchanged when a step fails.
 */
template <typename Clock, typename StopFn, typename IdleFn, typename StepFn>
core::Status run_paced(ConfiguredRun &run, std::uint64_t total_steps, double rate_hz,
                       std::uint64_t chunk_steps, typename Clock::time_point epoch,
                       StopFn &&stop_requested, IdleFn &&idle, Stats &stats,
                       StepFn &&before_chunk) {
  stats = Stats{};
  if (!validate(rate_hz, chunk_steps).ok()) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  const std::int64_t period = period_ns(rate_hz);
  const typename Clock::duration kSlice =
      std::chrono::duration_cast<typename Clock::duration>(std::chrono::milliseconds(1));

  std::uint64_t done = 0;
  typename Clock::time_point deadline = epoch;
  while (done < total_steps) {
    if (stop_requested()) {
      break;
    }
    std::uint64_t n = chunk_steps;
    if (n > total_steps - done) {
      n = total_steps - done;
    }
    before_chunk(done);
    // One fixed chunk: step-boundary stop granularity stays one chunk
    // (the CLI uses chunk 1, so every step stays interruptible).
    const core::Status st = run.run(n);
    if (!st.ok()) {
      stats.steps_done = done;
      return st;
    }
    done += n;
    stats.steps_done = done;
    // Absolute deadline for the finished prefix: advance from the epoch by
    // exactly one period per finished step (no now()+period re-anchoring,
    // so sleep jitter never becomes drift). Saturate at the clock maximum
    // instead of overflowing on unbounded (--forever) runs.
    const typename Clock::duration step_dur(period);
    const typename Clock::time_point max_tp = Clock::time_point::max();
    for (std::uint64_t i = 0; i < n; ++i) {
      if (deadline > max_tp - step_dur) {
        deadline = max_tp;
        break;
      }
      deadline += step_dur;
    }
    for (;;) {
      const typename Clock::time_point now = Clock::now();
      if (now >= deadline) {
        if (now > deadline) {
          stats.late_steps += n;
          const double late_s =
              std::chrono::duration<double>(now - deadline).count();
          if (late_s > stats.max_late_s) {
            stats.max_late_s = late_s;
          }
        }
        break;
      }
      if (stop_requested()) {
        return core::Status::success();
      }
      idle(); // output servicing during idle (TCP drain at the CLI)
      if (stop_requested()) {
        return core::Status::success();
      }
      typename Clock::duration rem = deadline - now;
      if (rem > kSlice) {
        rem = kSlice;
      }
      Clock::sleep_for(rem);
    }
  }
  return core::Status::success();
}

// Eight-argument form: no pre-chunk work (existing callers behave exactly
// as before — the hook defaults to a no-op).
/**
 * @brief Convenience overload without a pre-chunk hook.
 *
 * Behaves exactly like the nine-argument form with a no-op before_chunk
 * (existing callers keep their behavior).
 *
 * @tparam Clock pacing clock (SteadyClock in production).
 * @tparam StopFn bool() stop poll.
 * @tparam IdleFn void() idle hook.
 * @param[in,out] run the shared configured run.
 * @param[in] total_steps total steps to drive.
 * @param[in] rate_hz wall steps per second.
 * @param[in] chunk_steps steps per chunk.
 * @param[in] epoch pacing origin.
 * @param[in] stop_requested stop poll.
 * @param[in] idle idle hook.
 * @param[out] stats filled with the finished prefix's counters.
 * @return kOk on completion or stop; kInvalidArgument on invalid pacing;
 *         the solver Status unchanged on step failure.
 */
template <typename Clock, typename StopFn, typename IdleFn>
core::Status run_paced(ConfiguredRun &run, std::uint64_t total_steps, double rate_hz,
                       std::uint64_t chunk_steps, typename Clock::time_point epoch,
                       StopFn &&stop_requested, IdleFn &&idle, Stats &stats) {
  auto noop = [](std::uint64_t) {};
  return run_paced<Clock, StopFn, IdleFn, decltype(noop)>(
      run, total_steps, rate_hz, chunk_steps, epoch, std::forward<StopFn>(stop_requested),
      std::forward<IdleFn>(idle), stats, std::move(noop));
}

} // namespace wengine::solvers::paced
