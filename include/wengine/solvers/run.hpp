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
// with a non-projectile solver, set signal section with a non-signal
// solver, invalid signal params, kSum in a batch section, kTable without
// points, cross-kind leftovers) return kInvalidArgument; step failures
// propagate the solver's Status unchanged.

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
  // Signal-scoped section mirrors the gravity pattern above, but with
  // parsed values: frontends fill cfg.signal, the driver only validates.
  // Batch spawns identical sources, so kSum is rejected here — sums need
  // distinct per-source operands (SIG-6 family configures those directly).
  // Custom sources (SIG-4a): kExpression needs its program in params, kTable
  // needs its points in cfg.signal.table (appended once, shared by all N
  // batch sources); cross-kind leftovers (expr program with a sine kind,
  // table points with a non-table kind) are rejected, never ignored.
  const bool is_signal = solver->name() == SignalSolver::kName;
  if (is_signal) {
    if (!signal::validate_params(cfg.signal.params).ok() ||
        cfg.signal.params.kind == signal::SignalKind::kSum) {
      return {core::StatusCode::kInvalidArgument};
    }
    const bool is_expr = cfg.signal.params.kind == signal::SignalKind::kExpression;
    const bool is_table = cfg.signal.params.kind == signal::SignalKind::kTable;
    if (is_expr && !cfg.signal.table.empty()) {
      return {core::StatusCode::kInvalidArgument};
    }
    if (!is_expr && cfg.signal.params.expr.size != 0) {
      return {core::StatusCode::kInvalidArgument};
    }
    if (is_table) {
      if (!signal::validate_table_points(std::span(cfg.signal.table.data(),
                                                   cfg.signal.table.size()))
               .ok()) {
        return {core::StatusCode::kInvalidArgument};
      }
    } else if (!cfg.signal.table.empty()) {
      return {core::StatusCode::kInvalidArgument};
    }
  } else if (cfg.signal.set) {
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
    // (seed + index) so N noise sources never alias each other. Sums are
    // rejected above, so configure cannot fail here (defense in depth).
    // Table batches share one appended range (see append_table_points).
    const bool is_table = cfg.signal.params.kind == signal::SignalKind::kTable;
    std::uint64_t table_off = 0;
    std::uint64_t table_len = 0;
    if (is_table) {
      if (!world::append_table_points(
               runner.state().signals,
               std::span(cfg.signal.table.data(), cfg.signal.table.size()), table_off)
               .ok()) {
        return {core::StatusCode::kInvalidArgument};
      }
      table_len = static_cast<std::uint64_t>(cfg.signal.table.size());
    }
    for (std::uint64_t k = 0; k < cfg.signal.count; ++k) {
      world::Entity e = runner.state().spawn_entity();
      if (is_table) {
        const core::Status cst =
            world::attach_table_source(e, runner.state().signals, table_off, table_len,
                                       cfg.signal.params.quantity, 0.0);
        if (!cst.ok()) {
          return cst;
        }
        continue;
      }
      signal::SignalParams p = cfg.signal.params;
      p.seed = cfg.signal.params.seed + k;
      const core::Status cst =
          world::configure_source(e, runner.state().signals, p, 0.0);
      if (!cst.ok()) {
        return cst;
      }
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
