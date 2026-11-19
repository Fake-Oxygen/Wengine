#pragma once

/**
 * @file
 * TUI executor seam: the ncurses front end reuses the CLI command path
 * verbatim instead of forking one. Every submitted line goes to
 * `cli::Shell::execute` and the captured `ostream` text is returned for
 * display — the TUI never reimplements parsing, validation, or solver
 * calls, so TUI output is byte-identical to the REPL/headless CLI by
 * construction (docs/tui.md D-TUI-1; the TUI links `wengine_cli`, the one
 * sanctioned exception to the apps-never-cli shape). Completion delegates
 * to `cli::complete_line` over the same `CliRegistry`, so new commands
 * work in the TUI with no TUI-side change. Consumers: `src/tui/` TUs
 * (input line, palette, forms, batches), `tests/test_tui_exec.cpp`
 * (pins executor-vs-`Shell` parity over the full registry).
 *
 * No ncurses here: this header needs `wengine_cli` only and stays
 * unit-testable without a terminal.
 */

#include "wengine/cli/command_registry.hpp"
#include "wengine/cli/complete.hpp"
#include "wengine/cli/shell.hpp"

#include <cstddef>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wengine::tui {

/**
 * @brief One executed line: the CLI exit code plus exactly what the CLI
 *        printed.
 */
struct ExecResult {
  /**
   * @brief CLI exit code as returned by `cli::Shell::execute`:
   *        0 ok, 1 runtime/check failure, 2 usage error, -1 exit signal
   *        (`exit`/`quit`).
   */
  int rc = 0;
  /**
   * @brief The full captured CLI output text for the line.
   */
  std::string text;
};

/**
 * @brief Thin adapter over the shared CLI shell: one executor, one
 *        captured-output execution path, one completion vocabulary.
 *
 * @note Owns no simulation state and no terminal state — it is the
 *       seam between the ncurses loop and `cli::Shell`, nothing more.
 */
class TuiExecutor {
public:
  /**
   * @brief Idle executor with a fresh `cli::Shell`.
   */
  TuiExecutor() = default;

  /**
   * @brief Run one input line through the shared CLI path and capture
   *        its output.
   *
   * @param[in] line One command line (same text the REPL would read).
   * @return Exit code plus captured text; the text is byte-identical to
   *         what the REPL/headless CLI prints for the same line.
   * @note Return codes match the CLI (`0` ok, `1` runtime, `2` usage,
   *        `-1` exit); the caller owns rendering and exit handling.
   */
  ExecResult execute_line(std::string_view line) {
    std::ostringstream out;
    const int rc = shell_.execute(line, out);
    return {.rc = rc, .text = out.str()};
  }

  /**
   * @brief Tab-completion candidates for `buf` with the cursor at
   *        `cursor`.
   *
   * @param[in] buf The input-line text before the cursor.
   * @param[in] cursor Cursor position within `buf` (0-based).
   * @return Candidates in the same vocabulary as the REPL: registry
   *         commands plus builtins plus per-command option completion.
   * @note Delegates to `cli::complete_line` over `cli::CliRegistry::all()`
   *       so completion parity is by construction, not by duplication.
   */
  [[nodiscard]] std::vector<std::string> complete(std::string_view buf,
                                                  std::size_t cursor) const {
    return cli::complete_line(buf, cursor, cli::CliRegistry::all());
  }

  /**
   * @brief Set the persistent history file (shared with the REPL).
   *
   * @param[in] path History file path; empty disables persistence.
   * @note Same default path, format, and cap as `cli::Shell`, so TUI
   *       recall and REPL recall share one history.
   */
  void set_history_file(std::string path) {
    shell_.set_history_file(std::move(path));
  }

private:
  cli::Shell shell_;
};

} // namespace wengine::tui
