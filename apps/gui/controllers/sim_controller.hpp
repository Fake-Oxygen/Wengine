#pragma once

// Frontend-owned stepped-sim driver for apps/gui (Dear ImGui panel) and its
// unit test. Owns a solvers::ConfiguredRun (shared validation/population
// with solvers::run_headless() — see solvers/configured_run.hpp), rebuilds
// it from a plain core::RunConfig on every accepted edit, and exposes
// snapshots/status for display. Transport: play (wall-clock advance),
// pause, exact single-step, reset to the current config.
//
// Determinism contract: rebuild + fixed-step driving reproduce
// solvers::run_headless() bit-for-bit for the same config (same Runner
// sequencing, same spawn order and initial values) because both frontends
// use the one shared builder — there is no duplicated spawn block left to
// drift.
//
// Engine-only includes: no ImGui, SDL, or Vulkan here, so unit tests link
// wengine_core alone. GUI owns all I/O; failures surface as Status plus an
// inquiryable message (frontend code may allocate — the hot path inside
// Runner::run/advance still does not). The controller provides no Lua
// scripting runtime (wengine_core only), so kLua configs are rejected by
// the shared builder with a message (SIG-9 panel follow-up).

#include "wengine/core/run_config.hpp"
#include "wengine/emit/sink.hpp"
#include "wengine/solvers/configured_run.hpp"
#include "wengine/world/state.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace wengine::gui {

class SimController {
 public:
  SimController() {
    core::RunConfig cfg;
    cfg.loop.dt = 0.01;
    cfg.loop.emit_every = 1;
    cfg.steps = 100;
    cfg.solver = solvers::ProjectileSolver::kName;
    cfg.gravity = 9.81;
    cfg.has_gravity = false;
    cfg.particles = 1;
    cfg.x0 = 0.0;
    cfg.y0 = 0.0;
    cfg.vx0 = 10.0;
    cfg.vy0 = 10.0;
    last_status_ = set_config(cfg);
  }

  // Validate and adopt a new config, rebuilding the run from step 0.
  // Rejected edits leave the live run untouched; the message explains why.
  core::Status set_config(const core::RunConfig &cfg) {
    solvers::ConfiguredRun next;
    std::string message;
    const core::Status st = solvers::ConfiguredRun::build(
        cfg, std::span<core::ISolver *>{}, /*has_lua_runtime=*/false, next, &message);
    if (!st.ok()) {
      error_ = message.empty() ? "invalid config" : message;
      last_status_ = st;
      return last_status_;
    }
    config_ = cfg;
    run_ = std::move(next);
    for (emit::ISink *sink : observers_) {
      run_.add_sink(*sink);
    }
    error_.clear();
    last_status_ = core::Status::success();
    return last_status_;
  }

  // Rebuild from the current (already validated) config. Step index and
  // clock return to 0; transport keeps its play/pause state.
  void reset() {
    solvers::ConfiguredRun next;
    std::string message;
    const core::Status st = solvers::ConfiguredRun::build(
        config_, std::span<core::ISolver *>{}, /*has_lua_runtime=*/false, next, &message);
    if (!st.ok()) {
      error_ = message.empty() ? "signal setup failed" : message;
      last_status_ = st;
      return; // unreachable post-validation; live run left intact
    }
    run_ = std::move(next);
    for (emit::ISink *sink : observers_) {
      run_.add_sink(*sink);
    }
    error_.clear();
    last_status_ = core::Status::success();
  }

  // Advance exactly `steps` fixed steps (single-step = run_exact(1)).
  // Deterministic: identical call sequences give identical states.
  core::Status run_exact(std::uint64_t steps) {
    last_status_ = run_.run(steps);
    if (!last_status_.ok()) {
      error_ = "simulation step failed";
    }
    return last_status_;
  }

  // Fold wall-clock time into fixed steps (play mode). Capped per call by
  // RunnerConfig::max_substeps, so stalls cannot spiral. While playing, the
  // frame is additionally clamped to the remaining step budget, so play
  // parks exactly at `steps` and Reset + Play reproduces a headless batch
  // bit-for-bit regardless of frame rate. Reaching the budget auto-pauses.
  core::Status advance_frame(double frame_dt_seconds) {
    if (playing_) {
      const std::uint64_t done = run_.state().step_index;
      if (done < config_.steps) {
        const std::uint64_t remaining = config_.steps - done;
        const double budget_dt =
            static_cast<double>(remaining) * config_.loop.dt;
        if (frame_dt_seconds > budget_dt) {
          frame_dt_seconds = budget_dt;
        }
      }
    }
    last_status_ = run_.runner().advance(frame_dt_seconds);
    if (!last_status_.ok()) {
      error_ = "simulation step failed";
      return last_status_;
    }
    if (run_.state().step_index >= config_.steps) {
      playing_ = false;
    }
    return last_status_;
  }

  void set_playing(bool playing) noexcept { playing_ = playing; }
  [[nodiscard]] bool playing() const noexcept { return playing_; }

  // Attach a best-effort snapshot observer (graphs, future views). The
  // controller never owns sinks: callers keep them alive for as long as
  // the controller runs. Re-attached automatically on every rebuild, so
  // config edits and resets never silently detach observers.
  void add_sink(emit::ISink &sink) {
    observers_.push_back(&sink);
    run_.add_sink(sink);
  }

  [[nodiscard]] const core::RunConfig &config() const noexcept { return config_; }
  [[nodiscard]] const world::State &state() const noexcept {
    return run_.state();
  }
  // Independent deep copy: safe to hold across rebuilds and frames.
  [[nodiscard]] world::Snapshot snapshot() const {
    return world::take_snapshot(run_.state());
  }
  [[nodiscard]] bool ok() const noexcept { return last_status_.ok(); }
  [[nodiscard]] const std::string &error() const noexcept { return error_; }

 private:
  core::RunConfig config_{};
  // Shared builder output: owns the primary solver + initialized Runner.
  // Extras are unsupported here (no scripting runtime in wengine_core).
  solvers::ConfiguredRun run_;
  // Non-owning observers, re-attached on every rebuild (see add_sink).
  std::vector<emit::ISink *> observers_;
  bool playing_ = false;
  core::Status last_status_{core::StatusCode::kInvalidArgument};
  std::string error_{"not configured"};
};

} // namespace wengine::gui
