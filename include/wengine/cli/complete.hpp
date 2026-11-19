#pragma once

/**
 * @file
 * Tab-completion engine (pure logic, no TTY code, unit testable).
 * complete_line looks at the buffer before the cursor and returns
 * candidates for the token under the cursor: command names in first
 * position (registry plus help/exit/quit), the `help` command argument,
 * or the target command own CommandDef::complete for the rest. Coverage
 * follows the command contract: modes, option names, and `--opt=` value
 * forms (each spec own complete() owns its value vocabulary). Consumers:
 * LineReader (TTY editing) and module sessions (which swap in their own
 * registry while active). Dependency direction: cli to solvers and core,
 * never the reverse.
 */

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
/**
 * @brief Builtin REPL names completed alongside registered commands.
 * @note help/exit/quit are Shell builtins, not registry entries, but
 *       first-position completion offers them uniformly.
 */
inline constexpr std::array<std::string_view, 3> kBuiltinCommands{{"help", "exit", "quit"}};

// Candidates for the token ending at `cursor` (byte index into `buf`).
// Only `buf[0..cursor)` is considered; text after the cursor is untouched.
/**
 * @brief Complete the token ending at the cursor.
 * @param[in] buf Full input buffer (only buf[0..cursor) is considered;
 *                text after the cursor is untouched).
 * @param[in] cursor Byte index into buf marking the token end.
 * @param[in] cmds Command table to complete against (top-level registry,
 *                 or the module registry inside a session).
 * @return Candidates for the token under the cursor (common-prefix
 *         folding is the caller job via common_prefix).
 * @note Pure function over views; no TTY access, no allocation beyond
 *       the returned vector. First position completes command names,
 *       `help` second position completes its command argument, the rest
 *       delegates to the target command own complete().
 */
std::vector<std::string> complete_line(std::string_view buf, std::size_t cursor,
                                       std::span<const CommandDef> cmds);

// Longest common byte prefix of all candidates (empty when none).
/**
 * @brief Longest common byte prefix of all candidates.
 * @param[in] candidates Completion candidates from complete_line.
 * @return Shared prefix (empty when there are no candidates); the
 *         reader inserts it plus one completion step per Tab.
 */
std::string common_prefix(std::span<const std::string> candidates);

} // namespace wengine::cli
