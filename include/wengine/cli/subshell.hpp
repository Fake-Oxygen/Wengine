#pragma once

// Generic module sub-shell framework (scaffold — LOGIC-1 wires it in).
//
// Problem: some modules (first: `logic`) need a session with their own
// prompt (`[logic]> `) and their own command vocabulary, while staying
// scriptable headless (`logic <verb> ...` one-shot) and reusing all of
// this for future modules unchanged.
//
// Contract (docs/logic.md §3):
// - `SubShell` is registry-agnostic: it dispatches lines against any
//   `span<const CommandDef>` with any prompt string. One implementation,
//   N modules.
// - Entering the session is a `Shell` concern (like `help`/`exit`),
//   because `Spec::run(ArgCursor&, ostream&)` has no `istream` access.
//   A module spec with empty args returns `kEnterSubShell`; `Shell`
//   interprets it and runs the session loop on the same `LineReader`
//   (history shared) with the prompt swapped.
// - Inside the session, completion calls `complete_line` with the MODULE
//   registry only — the top-level registry is unreachable by construction.
// - `back` (and `exit`/`quit` inside the session) leaves the session;
//   only the outer loop quits the app. Depth is capped (no nesting).
// - Headless one-shot (`logic wire A.Y B.1`) dispatches to the same
//   module registry without entering any loop, so pipes/scripts/TUI
//   behave identically to the session line-by-line.
//
// No iostream here (prompt is a string_view; the driver owns I/O).

#include "wengine/cli/command.hpp"

#include <cstddef>
#include <span>
#include <string_view>

namespace wengine::cli {

// Returned by a module entry spec (e.g. bare `logic`) when invoked
// without a sub-verb in the REPL. Distinct from kExitOk/Runtime/Usage
// and from -1 (quit): tells Shell to push the module session.
inline constexpr int kEnterSubShell = 100;

// Maximum session nesting (1 = no sub-shell inside a sub-shell;
// deeper requests print a loud error instead of recursing).
inline constexpr std::size_t kMaxSubShellDepth = 1;

struct SubShellConfig {
  std::string_view prompt; // e.g. "[logic]> "
  std::span<const CommandDef> commands; // module registry only
  std::string_view exit_hint; // e.g. "back - leave the logic session"
};

// Pure helpers (unit-testable without a TTY):
// - `is_session_exit(name)`: true for back/exit/quit.
// - `resolve(words, commands)`: find the verb def or nullptr.
[[nodiscard]] inline constexpr bool is_session_exit(std::string_view word) noexcept {
  return word == "back" || word == "exit" || word == "quit";
}

[[nodiscard]] inline constexpr const CommandDef *resolve_subcommand(std::span<const std::string_view> words, std::span<const CommandDef> commands) noexcept {
  if (words.empty()) {
    return nullptr;
  }
  for (const auto &c : commands) {
    if (c.name == words[0]) {
      return &c;
    }
  }
  return nullptr;
}

} // namespace wengine::cli
