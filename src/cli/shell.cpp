#include "wengine/cli/shell.hpp"
#include "wengine/cli/args.hpp"
#include "wengine/cli/command_registry.hpp"

#include <array>
#include <exception>
#include <iostream>
#include <string>
#include <utility>

namespace wengine::cli {

int Shell::execute(std::string_view line, std::ostream &out) {
  // Own the line so string_view tokens stay valid for this call.
  // One allocation per line here (unavoidable for REPL); everything
  // below is zero-copy.
  std::string owned{line};

  std::array<std::string_view, kMaxArgs> buf{};
  const std::size_t n = tokenize_into(owned, buf);
  if (n == 0) {
    return 0;
  }
  if (n > buf.size()) {
    out << "Error: too many arguments (max " << buf.size() << ")\n";
    return 1;
  }

  std::string_view cmd_name = buf[0];
  std::span<const std::string_view> args{buf.data() + 1, n - 1};

  if (cmd_name == "exit" || cmd_name == "quit") {
    return -1;
  }
  if (cmd_name == "help") {
    // "help <cmd>" prints that command's usage.
    if (n > 1) {
      if (const auto *d = CliRegistry::find(buf[1])) {
        out << d->name << " - " << d->description << '\n';
        if (!d->usage.empty()) {
          out << "Usage:\n" << d->usage << '\n';
        }
        return 0;
      }
      out << "Unknown command: " << buf[1] << '\n';
      return 1;
    }
    print_help(out);
    return 0;
  }

  const auto *def = CliRegistry::find(cmd_name);
  if (def == nullptr) {
    out << "Unknown command: " << cmd_name << '\n';
    out << "Type 'help' for available commands.\n";
    return 1;
  }

  ArgCursor cursor{args};
  try {
    return def->run(cursor, out);
  } catch (const std::exception &e) {
    out << "Error: " << e.what() << '\n';
    return 1;
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
}

} // namespace wengine::cli
