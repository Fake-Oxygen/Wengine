#include "wengine/cli/shell.hpp"
#include <iostream>
#include <string_view>
#include <vector>

int main(int argc, char **argv) {
  wengine::cli::Shell shell;

  // Headless mode: single command from argv, run once and exit.
  // Exit code mirrors the command result (exit/quit -> 0).
  if (argc > 1) {
    std::vector<std::string_view> args;
    args.reserve(static_cast<std::size_t>(argc) - 1);
    for (int i = 1; i < argc; ++i) {
      args.emplace_back(argv[i]);
    }
    const int rc = shell.execute_argv(args, std::cout);
    return rc == -1 ? 0 : rc;
  }

  shell.run(std::cin, std::cout);
  return 0;
}
