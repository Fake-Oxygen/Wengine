#pragma once

// Headless batch driver: the engine side of the core::RunConfig seam.
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
