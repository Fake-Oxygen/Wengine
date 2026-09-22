#pragma once

// Single place to register stepped solvers: append the spec to the list.
// Validation (unique names, non-empty) happens at compile time.

#include "wengine/solvers/projectile.hpp"
#include "wengine/solvers/registry.hpp"

namespace wengine::solvers {

using SolverRegistry = Registry<ProjectileSolver>;

} // namespace wengine::solvers
