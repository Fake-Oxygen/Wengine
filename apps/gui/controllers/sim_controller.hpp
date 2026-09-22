#pragma once

// Frontend-owned stepped-sim driver for apps/gui (Dear ImGui panel) and its
// unit test. Owns a core::Runner plus its solver, rebuilds both from a plain
// core::RunConfig on every accepted edit, and exposes snapshots/status for
// display. Transport: play (wall-clock advance), pause, exact single-step,
// reset to the current config.
//
// Determinism contract: rebuild + fixed-step driving reproduce
// solvers::run_headless() bit-for-bit for the same config (same Runner
// sequencing, same spawn order and initial values). Validation mirrors
// run_headless(): dt > 0, steps >= 1, known solver name,
// projectile-scoped gravity. A dedicated test guards this parity, so the
// small spawn block duplicated from solvers/run.hpp cannot drift silently.
//
// Engine-only includes: no ImGui, SDL, or Vulkan here, so unit tests link
// wengine_core alone. GUI owns all I/O; failures surface as Status plus an
// inquiryable message (frontend code may allocate — the hot path inside
// Runner::run/advance still does not).

#include "wengine/core/run_config.hpp"
#include "wengine/core/runner.hpp"
#include "wengine/solvers/solver_registry.hpp"
#include "wengine/world/state.hpp"

#include <cstdint>
#include <memory>
#include <string>

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
    if (!(cfg.loop.dt > 0.0)) {
      return reject("dt must be > 0");
    }
    if (cfg.steps < 1) {
      return reject("steps must be >= 1");
    }
    const solvers::SolverDef *sdef = solvers::SolverRegistry::find(cfg.solver);
    if (sdef == nullptr) {
      return reject(std::string("unknown solver '") + std::string(cfg.solver) +
                    "'");
    }
    const bool is_projectile = sdef->name == solvers::ProjectileSolver::kName;
    if (!is_projectile && cfg.has_gravity) {
      return reject("--gravity only applies to solver 'projectile'");
    }
    config_ = cfg;
    rebuild();
    error_.clear();
    last_status_ = core::Status::success();
    return last_status_;
  }

  // Rebuild from the current (already validated) config. Step index and
  // clock return to 0; transport keeps its play/pause state.
  void reset() {
    rebuild();
    error_.clear();
    last_status_ = core::Status::success();
  }

  // Advance exactly `steps` fixed steps (single-step = run_exact(1)).
  // Deterministic: identical call sequences give identical states.
  core::Status run_exact(std::uint64_t steps) {
    last_status_ = runner_.run(steps);
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
      const std::uint64_t done = runner_.state().step_index;
      if (done < config_.steps) {
        const std::uint64_t remaining = config_.steps - done;
        const double budget_dt =
            static_cast<double>(remaining) * config_.loop.dt;
        if (frame_dt_seconds > budget_dt) {
          frame_dt_seconds = budget_dt;
        }
      }
    }
    last_status_ = runner_.advance(frame_dt_seconds);
    if (!last_status_.ok()) {
      error_ = "simulation step failed";
      return last_status_;
    }
    if (runner_.state().step_index >= config_.steps) {
      playing_ = false;
    }
    return last_status_;
  }

  void set_playing(bool playing) noexcept { playing_ = playing; }
  [[nodiscard]] bool playing() const noexcept { return playing_; }

  [[nodiscard]] const core::RunConfig &config() const noexcept { return config_; }
  [[nodiscard]] const world::State &state() const noexcept {
    return runner_.state();
  }
  // Independent deep copy: safe to hold across rebuilds and frames.
  [[nodiscard]] world::Snapshot snapshot() const {
    return world::take_snapshot(runner_.state());
  }
  [[nodiscard]] bool ok() const noexcept { return last_status_.ok(); }
  [[nodiscard]] const std::string &error() const noexcept { return error_; }

 private:
  core::Status reject(const std::string &msg) {
    error_ = msg;
    last_status_ = core::Status::failure(core::StatusCode::kInvalidArgument);
    return last_status_;
  }

  void rebuild() {
    runner_ = core::Runner(config_.loop);
    // set_config() validated the name, so this resolve cannot fail.
    const solvers::SolverDef *sdef =
        solvers::SolverRegistry::find(config_.solver);
    solver_ = sdef->create();
    if (solver_->name() == solvers::ProjectileSolver::kName) {
      static_cast<solvers::ProjectileSolver *>(solver_.get())
          ->set_gravity(config_.gravity);
    }
    runner_.add_solver(*solver_);
    // Population identical to solvers::run_headless() (see solvers/run.hpp):
    // same spawn order, same initial values — batch parity is exact.
    for (std::uint64_t k = 0; k < config_.particles; ++k) {
      world::Entity e = runner_.state().spawn_entity();
      runner_.state().particles.attach(e.index);
      runner_.state().particles.pos_x[e.index] = config_.x0;
      runner_.state().particles.pos_y[e.index] = config_.y0;
      runner_.state().particles.vel_x[e.index] = config_.vx0;
      runner_.state().particles.vel_y[e.index] = config_.vy0;
    }
  }

  core::RunConfig config_{};
  // Declared before runner_: Runner only borrows the solver pointer.
  std::unique_ptr<core::ISolver> solver_;
  core::Runner runner_;
  bool playing_ = false;
  core::Status last_status_{core::StatusCode::kInvalidArgument};
  std::string error_{"not configured"};
};

} // namespace wengine::gui
