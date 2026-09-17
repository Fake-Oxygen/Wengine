#pragma once

#include "wengine/cli/commands.hpp"
#include "wengine/cli/shell.hpp"

namespace wengine::cli {

inline constexpr Command kCommands[] = {
    {.name = "ohm",
     .description = "Solve resistor (Ohm's law demo)",
     .function = &commands::ohm},
};

} // namespace wengine::cli
