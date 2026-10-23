#include "wengine/cli/shell.hpp"
#include "wengine/cli/subshell.hpp"
#ifdef WENGINE_HAVE_TUI
#include "wengine/tui/run.hpp"
#endif
#include <iostream>
#include <string_view>
#include <vector>

int main(int argc, char **argv) {
  wengine::cli::Shell shell;

  // TUI entry: same binary, same commands, ncurses display. A launcher
  // flag (not a registry command) so the TUI trivially reuses the whole
  // CLI path instead of forking one.
  if (argc > 1 && std::string_view(argv[1]) == "--tui") {
#ifdef WENGINE_HAVE_TUI
    if (argc > 2) {
      std::cerr << "Usage: PhysicsSimApp --tui  (takes no further arguments)\n";
      return 2;
    }
    return wengine::tui::run_tui();
#else
    std::cerr << "TUI support is not built (configure with -DBUILD_TUI=ON and libncurses-dev)\n";
    return 1;
#endif
  }

  // Headless mode: single command from argv, run once and exit.
  // Exit code mirrors the command result (exit/quit -> 0).
  if (argc > 1) {
    std::vector<std::string_view> args;
    args.reserve(static_cast<std::size_t>(argc) - 1);
    for (int i = 1; i < argc; ++i) {
      args.emplace_back(argv[i]);
    }
    const int rc = shell.execute_argv(args, std::cout);
    if (rc == -1) {
      return 0; // exit/quit
    }
    if (rc == wengine::cli::kEnterSubShell) {
      // Bare module entry (e.g. `PhysicsSimApp logic`): run the session
      // on stdin so pipes work like the REPL
      // (`printf 'help\nback\n' | PhysicsSimApp logic`).
      if (!shell.has_pending_session()) {
        std::cerr << "Error: module session requested but none registered\n";
        return 1;
      }
      shell.run_pending_session(std::cin, std::cout);
      return 0;
    }
    return rc;
  }

  shell.run(std::cin, std::cout);
  return 0;
}
