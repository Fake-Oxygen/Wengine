#include "wengine/cli/shell.hpp"
#include "wengine/cli/args.hpp"
#include "wengine/cli/command_registry.hpp"
#include "wengine/core/status.hpp"

#include <array>
#include <exception>
#include <iostream>
#include <string>
#include <utility>

namespace wengine::cli {

// Unified convention: commands return `core::kExitOk/Runtime/Usage`; `-1`
// is the REPL exit signal (not a Status). The `catch` below is a defensive
// last resort only — engine/CLI code is noexcept and never throws; any
// exception maps to `kInternal` -> exit 1.

int Shell::execute(std::string_view line, std::ostream &out) {
  // Own the line so string_view tokens stay valid for this call.
  // One allocation per line here (unavoidable for REPL); everything
  // below is zero-copy.
  std::string owned{line};

  std::array<std::string_view, kMaxArgs> buf{};
  const std::size_t n = tokenize_into(owned, buf);
  if (n > buf.size()) {
    out << "Error: too many arguments (max " << buf.size() << ")\n";
    return core::kExitRuntime;
  }
  return execute_argv(std::span<const std::string_view>{buf.data(), n}, out);
}

int Shell::execute_argv(std::span<const std::string_view> tokens, std::ostream &out) {
  if (tokens.empty()) {
    return core::kExitOk;
  }
  if (tokens.size() > kMaxArgs) {
    out << "Error: too many arguments (max " << kMaxArgs << ")\n";
    return core::kExitRuntime;
  }

  std::string_view cmd_name = tokens[0];
  std::span<const std::string_view> args{tokens.data() + 1, tokens.size() - 1};

  if (cmd_name == "exit" || cmd_name == "quit") {
    return -1;
  }
  if (cmd_name == "help" || cmd_name == "--help" || cmd_name == "-h") {
    // "help <cmd>" prints that command's usage.
    if (!args.empty()) {
      if (const auto *d = CliRegistry::find(args[0])) {
        out << d->name << " - " << d->description << '\n';
        if (!d->usage.empty()) {
          out << "Usage:\n" << d->usage << '\n';
        }
        return core::kExitOk;
      }
      out << "Unknown command: " << args[0] << '\n';
      return core::kExitRuntime;
    }
    print_help(out);
    return core::kExitOk;
  }

  const auto *def = CliRegistry::find(cmd_name);
  if (def == nullptr) {
    out << "Unknown command: " << cmd_name << '\n';
    out << "Type 'help' for available commands.\n";
    return core::kExitRuntime;
  }

  ArgCursor cursor{args};
  try {
    return def->run(cursor, out);
  } catch (const std::exception &e) {
    out << "Error: " << e.what() << '\n';
    return core::to_exit_code(core::Status{core::StatusCode::kInternal});
  }
}

void Shell::run(std::istream &in, std::ostream &out) {
  while (true) {
    auto line = reader_.read_line("> ", in, out);
    if (!line) {
      break; // EOF (Ctrl-D / closed stdin)
    }
    const int rc = execute(*line, out);
    if (rc == -1) {
      break;
    }
  }
}

void Shell::set_history_file(std::string path) {
  reader_.set_history_file(std::move(path));
}

void Shell::print_help(std::ostream &out) const {
  out << "Available commands:\n\n";
  for (const auto &c : CliRegistry::all()) {
    out << "  " << c.name << " - " << c.description << '\n';
  }
  out << "\n help [cmd] - Show help\n";
  out << " exit - Exit shell\n";
  out << "\nTip: `PhysicsSimApp --tui` opens the ncurses interface (same commands).\n";
}

} // namespace wengine::cli
