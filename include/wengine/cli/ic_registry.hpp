#pragma once

/**
 * @file
 * Production registry of IC-specific chips for the `ic` command. To add a
 * chip (full recipe: AGENTS.md section 9, starter solver in
 * solvers/ic/template.hpp): add the pure solver header (no CLI types),
 * add the chip CommandSpec header here (kName is the lowercase part
 * number, run, optional complete), implement the command TU, and append
 * the spec below plus the TU to wengine_cli in CMakeLists.txt. Dispatch
 * and completion plumbing is shared (ic_dispatch.hpp); chips never
 * become top-level commands. Consumers: the IcSpec command and its
 * completion entry. Dependency direction: cli to solvers and core, never
 * the reverse.
 */

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
#include "wengine/cli/ic/tmc22xx.hpp"

// Chip spec headers (one per part, e.g. "wengine/cli/ic/ne555.hpp"):
/* #include "wengine/cli/ic/ne555.hpp" */

namespace wengine::cli {

// Append new chip specs here, e.g. `Registry<ic::Ap63205Spec, ic::Ne555Spec>`.
// Names stay unique via the compile-time validation.
/**
 * @brief Production chip registry for two-level `ic` dispatch.
 * @note Append new chip specs here (kName is the lowercase part number);
 *       names stay unique via the compile-time validation. One command
 *       per chip family: adding a part never adds a top-level command.
 */
using IcRegistry = Registry<ic::Ap63205Spec, ic::Ina219Spec, ic::Tmc22xxSpec>;

} // namespace wengine::cli
