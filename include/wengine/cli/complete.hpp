#pragma once

// Tab-completion engine (pure logic, no TTY code — unit testable).
//
// `complete_line` looks at the buffer before the cursor and returns
// candidates for the token under the cursor: command names in first
// position (registry + help/exit/quit), `help`'s command argument, or the
// target command's own `CommandDef::complete` for the rest.

#include "wengine/cli/command.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wengine::cli {

// Builtin names completed alongside registered commands.
inline constexpr std::array<std::string_view, 3> kBuiltinCommands{{"help", "exit", "quit"}};

// Candidates for the token ending at `cursor` (byte index into `buf`).
// Only `buf[0..cursor)` is considered; text after the cursor is untouched.
std::vector<std::string> complete_line(std::string_view buf, std::size_t cursor,
                                       std::span<const CommandDef> cmds);

// Longest common byte prefix of all candidates (empty when none).
std::string common_prefix(std::span<const std::string> candidates);

} // namespace wengine::cli
