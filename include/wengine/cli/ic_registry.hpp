#pragma once

// Production registry of IC-specific chips for the `ic` command.
//
// To add a chip (full recipe: AGENTS.md section 9, starter solver:
// `include/wengine/solvers/ic/template.hpp`):
//   1. add `include/wengine/solvers/ic/<chip>.hpp` (pure solver, no CLI types)
//   2. add `include/wengine/cli/ic/<chip>.hpp` (one CommandSpec: kName =
//      lowercase part number, kDescription, kUsage, run, optional complete)
//   3. implement `src/cli/commands/ic/<chip>.cpp`
//   4. append the spec below + the .cpp to `wengine_cli` in CMakeLists.txt
//
// Dispatch/complete plumbing is shared (`ic_dispatch.hpp`); chips never
// become top-level commands.

#include "wengine/cli/command.hpp"
#include "wengine/cli/ic/ap63205.hpp"
#include "wengine/cli/ic/ina219.hpp"

// Chip spec headers (one per part, e.g. "wengine/cli/ic/ne555.hpp"):
/* #include "wengine/cli/ic/ne555.hpp" */

namespace wengine::cli {

// Append new chip specs here, e.g. `Registry<ic::Ap63205Spec, ic::Ne555Spec>`.
// Names stay unique via the compile-time validation.
using IcRegistry = Registry<ic::Ap63205Spec, ic::Ina219Spec>;

} // namespace wengine::cli
