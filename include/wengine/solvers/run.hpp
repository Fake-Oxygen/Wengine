#pragma once

// Headless batch driver: the engine side of the core::RunConfig seam.
// Builds the configured run without any frontend types: resolves
// cfg.solver through SolverRegistry, applies projectile-scoped gravity,
// spawns cfg.particles, attaches caller-owned sinks, advances exactly
// cfg.steps fixed steps, and copies the final state out.
//
// Contract: sinks are created and owned by the caller (CLI file sink, GUI
// views) and must be non-null; the driver never opens files or touches
// frontend I/O. Config problems (bad dt/steps, unknown solver, gravity
// with a non-projectile solver, signal opts with a non-signal solver,
// bad signal params) return kInvalidArgument; step failures propagate
// the solver's Status unchanged.

#include "wengine/core/run_config.hpp"
#include "wengine/core/runner.hpp"
#include "wengine/emit/sink.hpp"
#include "wengine/signal/types.hpp"
#include "wengine/solvers/solver_registry.hpp"
#include "wengine/world/state.hpp"

#include <cstdint>
#include <memory>
#include <span>

namespace wengine::solvers {

[[nodiscard]] inline core::Status run_headless(const core::RunConfig &cfg,
                                              std::span<emit::ISink *> sinks,
                                              world::State &out_state) {
  if (!(cfg.loop.dt > 0.0) || cfg.steps < 1) {
    return {core::StatusCode::kInvalidArgument};
  }
  const SolverDef *sdef = SolverRegistry::find(cfg.solver);
  if (sdef == nullptr) {
    return {core::StatusCode::kInvalidArgument};
  }
  std::unique_ptr<core::ISolver> solver = sdef->create();
  // --gravity configures the projectile solver specifically; particle
  // population is generic setup handled below.
  const bool is_projectile = solver->name() == ProjectileSolver::kName;
  if (is_projectile) {
    // Registry lookup guarantees the factory built a ProjectileSolver.
    static_cast<ProjectileSolver *>(solver.get())->set_gravity(cfg.gravity);
  } else if (cfg.has_gravity) {
    return {core::StatusCode::kInvalidArgument};
  }
  // Signal-scoped params mirror the gravity pattern above.
  const bool is_signal = solver->name() == SignalSolver::kName;
  signal::SignalParams sig_params;
  if (is_signal) {
    const auto kind = signal::parse_kind(cfg.sig_kind);
    const auto qty = signal::parse_quantity(cfg.sig_quantity);
    if (!kind.ok() || !qty.ok()) {
      return {core::StatusCode::kInvalidArgument};
    }
    sig_params.kind = kind.value;
    sig_params.quantity = qty.value;
    sig_params.freq_hz = cfg.sig_freq;
    sig_params.amp = cfg.sig_amp;
    sig_params.offset = cfg.sig_offset;
    sig_params.phase_rad = cfg.sig_phase;
    sig_params.duty = cfg.sig_duty;
    sig_params.seed = cfg.sig_seed;
    if (!signal::validate_params(sig_params).ok()) {
      return {core::StatusCode::kInvalidArgument};
    }
  } else if (cfg.has_signal_opts) {
    return {core::StatusCode::kInvalidArgument};
  }

  core::Runner runner(cfg.loop);
  runner.add_solver(*solver);
  for (std::uint64_t k = 0; k < cfg.particles; ++k) {
    world::Entity e = runner.state().spawn_entity();
    runner.state().particles.attach(e.index);
    runner.state().particles.pos_x[e.index] = cfg.x0;
    runner.state().particles.pos_y[e.index] = cfg.y0;
    runner.state().particles.vel_x[e.index] = cfg.vx0;
    runner.state().particles.vel_y[e.index] = cfg.vy0;
  }
  if (is_signal) {
    // Batch sources share params; per-source seeds derive deterministically
    // (seed + index) so N noise sources never alias each other.
    for (std::uint64_t k = 0; k < cfg.signals; ++k) {
      world::Entity e = runner.state().spawn_entity();
      signal::SignalParams p = sig_params;
      p.seed = sig_params.seed + k;
      world::configure_source(e, runner.state().signals, p, 0.0);
    }
  }
  for (emit::ISink *sink : sinks) {
    runner.add_sink(*sink);
  }
  const core::Status st = runner.run(cfg.steps);
  if (!st.ok()) {
    return st;
  }
  out_state = runner.state();
  return core::Status::success();
}

} // namespace wengine::solvers
