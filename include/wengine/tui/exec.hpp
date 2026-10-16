#pragma once

// TUI executor seam: the ncurses front end reuses the CLI command path
// verbatim (thin adapter over the CLI, which owns user interaction).
//
// Every submitted line goes to `cli::Shell::execute` and the captured
// `ostream` text is returned for display — the TUI never reimplements
// parsing, validation, or solver calls, so TUI output is byte-identical
// to the REPL/headless CLI by construction. Completion delegates to
// `cli::complete_line` over the same `CliRegistry`.
//
// No ncurses here: this header needs `wengine_cli` only and stays
// unit-testable without a terminal (see `tests/test_tui_exec.cpp`).

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

// One executed line: the CLI exit code plus exactly what the CLI printed.
struct ExecResult {
  int rc = 0;
  std::string text;
};

class TuiExecutor {
public:
  TuiExecutor() = default;

  // Run one input line through the shared CLI path and capture its output.
  // Return codes match the CLI (`0` ok, `1` runtime, `2` usage, `-1` exit).
  ExecResult execute_line(std::string_view line) {
    std::ostringstream out;
    const int rc = shell_.execute(line, out);
    return {.rc = rc, .text = out.str()};
  }

  // Tab-completion candidates for `buf` with the cursor at `cursor`
  // (same vocabulary as the REPL: registry + builtins + per-command specs).
  [[nodiscard]] std::vector<std::string> complete(std::string_view buf,
                                                  std::size_t cursor) const {
    return cli::complete_line(buf, cursor, cli::CliRegistry::all());
  }

  void set_history_file(std::string path) {
    shell_.set_history_file(std::move(path));
  }

private:
  cli::Shell shell_;
};

} // namespace wengine::tui
