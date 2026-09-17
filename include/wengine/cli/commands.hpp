#pragma once

#include "wengine/cli/shell.hpp"

namespace wengine::cli::commands {

int ohm(std::span<const std::string> args, std::ostream &out) noexcept;

} // namespace wengine::cli::commands
