#pragma once

/**
 * @file
 * Generic module sub-shell framework: the machinery behind module
 * sessions such as the logic workspace (`[logic]> ` prompt) while
 * staying scriptable headless (`logic <verb> ...` one-shot). Registry-
 * agnostic by contract (docs/logic.md 3): SubShell dispatches lines
 * against any span of CommandDefs with any prompt; entering the session
 * is a Shell concern (module specs with empty args return
 * kEnterSubShell and Shell runs the session loop on the shared
 * LineReader); inside a session, completion sees only the module
 * registry (the top level is unreachable by construction); depth is
 * capped (no nesting). No iostream here - the driver owns I/O.
 * Consumers: cli/shell.hpp (REPL + session loop), the logic module
 * entry spec (cli/logic_registry.hpp), and future module entries that
 * reuse complete_verbs instead of growing per-module plumbing.
 */

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
#include <string>
#include <string_view>
#include <vector>

namespace wengine::cli {

// Returned by a module entry spec (e.g. bare `logic`) when invoked
// without a sub-verb in the REPL. Distinct from kExitOk/Runtime/Usage
// and from -1 (quit): tells Shell to push the module session.
/**
 * @brief Return code asking Shell to push the module's session.
 * @note Distinct from kExitOk/kExitRuntime/kExitUsage and from -1
 *       (quit): produced by a module entry spec invoked with no sub-verb
 *       in the REPL; never escapes to the process boundary (headless
 *       one-shot treats bare entry as a usage error).
 */
inline constexpr int kEnterSubShell = 100;

// Returned by a module session-control verb (e.g. `back`): tells the
// session loop to pop back to the outer shell. In a one-shot
// (`logic back`) the entry spec maps it to a usage error with a hint,
// so this code never escapes to the process boundary either.
/**
 * @brief Return code asking the session loop to pop back to the shell.
 * @note In a one-shot (`logic back`) the entry spec maps it to a usage
 *       error with a hint, so this code never escapes to the process
 *       boundary either.
 */
inline constexpr int kExitSubShell = 101;

// Maximum session nesting (1 = no sub-shell inside a sub-shell;
// deeper requests print a loud error instead of recursing).
/**
 * @brief Maximum session nesting depth (1: no sub-shell inside a
 *        sub-shell).
 * @note Deeper requests print a loud error instead of recursing.
 */
inline constexpr std::size_t kMaxSubShellDepth = 1;

/**
 * @brief One module session: prompt, verb registry, and help text.
 * @note Views point at caller-owned static storage (spec string
 *       literals and static registry arrays) - they must outlive the
 *       session by construction. commands spans the module registry
 *       only; session control (back/exit/quit) is interpreted by the
 *       loop, never dispatched as a verb.
 */
struct SubShellConfig {
  /**
   * @brief Module name, e.g. `logic` (used in diagnostics).
   */
  std::string_view name;   // e.g. "logic" (used in diagnostics)
  /**
   * @brief Session prompt, e.g. `[logic]> `.
   */
  std::string_view prompt; // e.g. "[logic]> "
  /**
   * @brief Module verb registry (never the top-level registry).
   */
  std::span<const CommandDef> commands; // module registry only
  /**
   * @brief Hint line shown by session help (e.g. how to leave).
   */
  std::string_view exit_hint; // e.g. "back - leave the logic session"
};

// Pure helpers (unit-testable without a TTY):
// - `is_session_exit(name)`: true for back/exit/quit.
// - `resolve(words, commands)`: find the verb def or nullptr.
/**
 * @brief Whether a word leaves the session (back/exit/quit).
 * @param[in] word Candidate token from the prompt line.
 * @return True for `back`, `exit`, or `quit`.
 * @note Pure helper, unit-testable without a TTY.
 */
[[nodiscard]] inline constexpr bool is_session_exit(std::string_view word) noexcept {
  return word == "back" || word == "exit" || word == "quit";
}

/**
 * @brief Resolve the first token of a line against a verb registry.
 * @param[in] words Line tokens (may be empty).
 * @param[in] commands Verb registry to search (module registry only).
 * @return The matching CommandDef, or nullptr when words is empty or
 *         the verb is unknown.
 */
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

// Complete one module-verb argument line (the entry spec's `complete`:
// no prior tokens -> verb names plus --help/-h; a known verb first ->
// delegate to that verb's own `complete` with the verb stripped).
// Template over any CommandSpec registry (same shape as
// ic::complete over IcRegistry); future module entries reuse this
// instead of growing per-module plumbing.
/**
 * @brief Complete one module-verb argument line (the entry spec's
 *        `complete`).
 * @param[in] prior_args Tokens after the module name, before the
 *                       partial (first one is the verb when present).
 * @param[in] partial Partial token under the cursor.
 * @param[out] out Candidates: verb names plus `--help`/`-h` with no
 *                 prior tokens; otherwise delegate to the verb's own
 *                 complete with the verb token stripped.
 * @note Template over any CommandSpec registry (same shape as
 *       ic::complete over IcRegistry); future module entries reuse this
 *       instead of growing per-module plumbing.
 */
template <typename R>
void complete_verbs(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out) {
  if (prior_args.empty()) {
    if (!partial.empty() && partial.front() == '-') {
      constexpr std::string_view kHelp = "--help";
      if (kHelp.substr(0, partial.size()) == partial) {
        out.emplace_back(kHelp);
      }
      constexpr std::string_view kShort = "-h";
      if (kShort.substr(0, partial.size()) == partial) {
        out.emplace_back(kShort);
      }
      return;
    }
    for (const auto &c : R::all()) {
      if (c.name.substr(0, partial.size()) == partial) {
        out.emplace_back(c.name);
      }
    }
    return;
  }
  const CommandDef *def = R::find(prior_args[0]);
  if (def != nullptr && def->complete != nullptr) {
    def->complete(prior_args.subspan(1), partial, out);
  }
}

} // namespace wengine::cli
