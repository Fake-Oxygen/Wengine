#pragma once

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
struct Config {
  double rate_hz = 0.0;
  std::uint64_t chunk_steps = 1;
};

struct Stats {
  std::uint64_t steps_done = 0;
  std::uint64_t late_steps = 0; // steps run past their absolute deadline
  double max_late_s = 0.0;      // worst overshoot, seconds
};

// Rounded whole-nanosecond period for rate_hz; <= 0 when invalid.
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

[[nodiscard]] inline core::Status validate(double rate_hz,
                                           std::uint64_t chunk_steps) noexcept {
  if (period_ns(rate_hz) <= 0 || chunk_steps < 1) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  return core::Status::success();
}

// Production clock: steady (monotonic) time + blocking sleep.
struct SteadyClock {
  using clock = std::chrono::steady_clock;
  using time_point = clock::time_point;
  using duration = clock::duration;
  static time_point now() noexcept { return clock::now(); }
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
