#pragma once

// Single place to add commands: append your Spec to the Registry list.
// Validation (unique names, non-empty) happens at compile time.

#include "wengine/cli/commands.hpp"
#include "wengine/cli/command.hpp"

namespace wengine::cli {

using CliRegistry =
    Registry<commands::OhmSpec, commands::S0Spec, commands::I2cSpec, commands::SimSpec>;

} // namespace wengine::cli
