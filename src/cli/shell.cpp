#include "wengine/cli/shell.hpp"
#include "wengine/cli/args.hpp"
#include "wengine/cli/command_registry.hpp"
#include "wengine/cli/logic_registry.hpp"
#include "wengine/core/status.hpp"

#include <array>
#include <exception>
#include <iostream>
#include <string>
#include <utility>

namespace wengine::cli {

namespace {

// Invoke one resolved command with exception isolation (shared by the
// top-level path and module sessions).
int invoke(const CommandDef *def, std::span<const std::string_view> args, std::ostream &out) {
  ArgCursor cursor{args};
  try {
    return def->run(cursor, out);
  } catch (const std::exception &e) {
    out << "Error: " << e.what() << '\n';
    return core::to_exit_code(core::Status{core::StatusCode::kInternal});
  }
}

// Module session configs by top-level entry command. To give a future
// module its own session, add one branch here (entry handling itself
// stays generic in run_session()).
std::optional<SubShellConfig> module_session_for(std::string_view top_cmd) {
  if (top_cmd == commands::LogicSpec::kName) {
    return SubShellConfig{.name = logic::kSessionName,
                          .prompt = logic::kSessionPrompt,
                          .commands = logic::LogicRegistry::all(),
                          .exit_hint = logic::kSessionExitHint};
  }
  return std::nullopt;
}

} // namespace

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
  pending_.reset();
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

  const int rc = invoke(def, args, out);
  if (rc == kEnterSubShell) {
    // A module entry asked for its session; Shell::run() (REPL) and
    // main() (headless) consume it via run_pending_session(). Unknown
    // entries defensively keep the code without a session.
    pending_ = module_session_for(cmd_name);
  }
  return rc;
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
    if (rc == kEnterSubShell && has_pending_session()) {
      if (run_pending_session(in, out) == -1) {
        break;
      }
    }
  }
}

int Shell::run_pending_session(std::istream &in, std::ostream &out) {
  if (!pending_) {
    return core::kExitOk;
  }
  const SubShellConfig cfg = *pending_;
  pending_.reset();
  return run_session(cfg, in, out);
}

int Shell::run_session(const SubShellConfig &cfg, std::istream &in, std::ostream &out) {
  if (sessions_.size() >= kMaxSubShellDepth) {
    out << "Error: already inside the '" << sessions_.back().name << "' session (nested sessions are not supported)\n";
    return core::kExitRuntime;
  }
  sessions_.push_back(cfg);
  reader_.set_complete_commands(cfg.commands);
  out << "Entering " << cfg.name << " session (prompt '" << cfg.prompt << "'; " << cfg.exit_hint << ").\n";

  int result = core::kExitOk;
  while (true) {
    auto line = reader_.read_line(cfg.prompt, in, out);
    if (!line) {
      break; // EOF leaves the session silently
    }
    // Own the line so string_view tokens stay valid for this iteration
    // (same contract as execute()).
    std::string owned{*line};
    std::array<std::string_view, kMaxArgs> buf{};
    const std::size_t n = tokenize_into(owned, buf);
    if (n > buf.size()) {
      out << "Error: too many arguments (max " << buf.size() << ")\n";
      continue;
    }
    std::span<const std::string_view> tokens{buf.data(), n};
    if (tokens.empty()) {
      continue;
    }
    // Session control: exit/quit leave the session (only the outer
    // loop quits the app). `back` is a real module verb so it gets
    // argument validation plus completion; it returns kExitSubShell.
    if (tokens[0] == "exit" || tokens[0] == "quit") {
      break;
    }
    const CommandDef *def = resolve_subcommand(tokens, cfg.commands);
    if (def == nullptr) {
      out << "Unknown " << cfg.name << " command: " << tokens[0] << '\n';
      out << "Type 'help' for session commands.\n";
      continue;
    }
    const int rc = invoke(def, std::span<const std::string_view>{tokens.data() + 1, tokens.size() - 1}, out);
    if (rc == kExitSubShell) {
      break;
    }
    if (rc == kEnterSubShell) {
      out << "Error: nested sessions are not supported\n";
      continue;
    }
    if (rc == -1) {
      result = -1; // a session verb requested app quit: unwind first
      break;
    }
  }

  sessions_.pop_back();
  reader_.set_complete_commands(CliRegistry::all());
  out << "Leaving " << cfg.name << " session.\n";
  return result;
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
