#pragma once

/**
 * @file
 * Production top-level command registry: the single place to add
 * commands (append a spec to the Registry list; uniqueness and
 * non-emptiness are validated at compile time). Commands stay generic
 * here - one command per solver domain (`sig` owns all signal
 * vocabulary, `ic` all IC-specific vocabulary); `sim run` takes no
 * solver options beyond the grandfathered `--gravity`. Consumers: Shell
 * (dispatch, help, completion) and the headless entry point. The
 * registry-size static_assert in tests/test_cli_args.cpp must stay in
 * sync with this list.
 */

// Single place to add commands: append your Spec to the Registry list.
// Validation (unique names, non-empty) happens at compile time.

#include "wengine/cli/commands.hpp"
#include "wengine/cli/command.hpp"

namespace wengine::cli {

/**
 * @brief Production top-level registry (one command per solver domain).
 * @note Append new command specs here; names stay unique via the
 *       compile-time validation. The registry-size static_assert in
 *       tests/test_cli_args.cpp must stay in sync with this list.
 */
using CliRegistry =
    Registry<commands::OhmSpec, commands::S0Spec, commands::I2cSpec, commands::LdoSpec,
             commands::LedSpec, commands::SimSpec, commands::SigSpec, commands::IcSpec,
             commands::LogicSpec>;

} // namespace wengine::cli
