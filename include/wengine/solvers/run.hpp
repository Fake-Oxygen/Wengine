#pragma once

/**
 * @file
 * Headless batch driver: the engine side of the core::RunConfig seam.
 * Owns the one-shot build-then-step flow over ConfiguredRun — it builds
 * the shared validation/population, attaches caller-owned sinks, advances
 * exactly cfg.steps fixed steps, and copies the final state out. Never
 * opens files or touches frontend I/O, and depends on engine types only
 * (never CLI types). Consumed by the `sim run` / `sig run` CLI commands
 * and tests.
 */

// Thin one-shot wrapper over solvers::ConfiguredRun (see configured_run.hpp):
// builds the shared validation/population (solver resolution,
// projectile-scoped gravity, particle + signal spawns with deterministic
// seeds), attaches caller-owned sinks, advances exactly cfg.steps fixed
// steps, and copies the final state out.
//
// Contract: sinks are created and owned by the caller (CLI file sink, GUI
// views) and must be non-null; the driver never opens files or touches
// frontend I/O. Config problems return kInvalidArgument; step failures
// propagate the solver's Status unchanged.
//
// Extra solvers (SIG-4b): caller-owned ISolvers stepped BEFORE the primary
// solver, in span order — the custom-source evaluation contract. The Lua
// extra solver evaluates kLua slots first so SignalSolver pass 1 can skip
// them and pass-2 sums still read fresh values. Empty by default (existing
// callers behave exactly as before). The Lua requirement is explicit in
// the shared builder: kLua needs a "lua" extra solver, not just any
// nonempty span (see configured_run.hpp).

#include "wengine/solvers/configured_run.hpp"
#include "wengine/world/state.hpp"

#include <span>

namespace wengine::solvers {

/**
 * @brief Build a run from cfg, attach sinks, advance cfg.steps, and copy
 *        the final state out.
 *
 * @param[in] cfg parsed run configuration (loop, solver selection,
 *            particle/signal/sequential sections).
 * @param[in] sinks caller-owned, non-null sink observers attached for the
 *            whole run (CLI file sinks, GUI views); the driver never
 *            owns or opens them.
 * @param[out] out_state receives a copy of the final world state; written
 *            only on success.
 * @param[in] extra caller-owned extra solvers stepped BEFORE the primary
 *            solver, in span order (the custom-source evaluation
 *            contract; empty by default). Callers keep them alive for the
 *            whole call.
 * @param[in] stop_requested optional poll (checked between step chunks);
 *            null runs all steps uninterrupted.
 * @return kOk on success; kInvalidArgument on config problems (unknown
 *         solver, bad dt/steps, misplaced per-solver sections, missing
 *         Lua runtime); the solver/runner Status unchanged when a step
 *         fails. An interrupted run (stop requested) still succeeds with
 *         the partial prefix in out_state.
 * @note Determinism: identical cfg plus identical sinks reproduce the
 *       run exactly — validation, spawn order, seeds, and step sequencing
 *       all live in the shared ConfiguredRun builder.
 */
[[nodiscard]] inline core::Status run_headless(const core::RunConfig &cfg,
                                               std::span<emit::ISink *> sinks,
                                               world::State &out_state,
                                               std::span<core::ISolver *> extra = {},
                                               bool (*stop_requested)() noexcept = nullptr) {
  ConfiguredRun run;
  // The Lua runtime flag is derived explicitly from the provided extras:
  // only a live "lua" solver satisfies kLua (a dummy/non-Lua nonempty span
  // does not — see configured_run.hpp).
  const bool has_lua = contains_lua_solver(extra);
  const core::Status built = ConfiguredRun::build(cfg, extra, has_lua, run, nullptr);
  if (!built.ok()) {
    return built;
  }
  for (emit::ISink *sink : sinks) {
    run.add_sink(*sink);
  }
  const core::Status st = run.run(cfg.steps, stop_requested);
  if (!st.ok()) {
    return st;
  }
  out_state = run.state();
  return core::Status::success();
}

} // namespace wengine::solvers
