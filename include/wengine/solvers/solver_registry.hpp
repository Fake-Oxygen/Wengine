#pragma once

/**
 * @file
 * The engine's concrete solver catalog: one Registry instantiation listing
 * every registered stepped solver. This is the single registration point
 * — adding a solver means appending one obvious line here (AGENTS.md
 * section 9); names, factories, and uniqueness are validated at compile
 * time by the Registry template. Depends on the solver headers only,
 * never CLI types; consumed by ConfiguredRun::build, the CLI `sim`
 * commands, and the GUI controller.
 */

// Single place to register stepped solvers: append the spec to the list.
// Validation (unique names, non-empty) happens at compile time.

#include "wengine/solvers/projectile.hpp"
#include "wengine/solvers/registry.hpp"
#include "wengine/solvers/sequential.hpp"
#include "wengine/solvers/signal.hpp"

namespace wengine::solvers {

/** @brief The registered solver catalog: projectile, signal, and logic. */
using SolverRegistry = Registry<ProjectileSolver, SignalSolver, SequentialSolver>;

} // namespace wengine::solvers
