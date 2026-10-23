#include "wengine/cli/commands.hpp"
#include "wengine/cli/logic_registry.hpp"
#include "wengine/cli/status.hpp"
#include "wengine/cli/subshell.hpp"
#include "wengine/core/status.hpp"

#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wengine::cli::commands {

namespace {

namespace logic = cli::logic;

// Shared verb-list printer (top-level `logic --help` and session `help`).
void print_verbs(std::ostream &out) {
  out << "Logic session verbs:\n";
  for (const auto &c : logic::LogicRegistry::all()) {
    out << "  " << c.name << " - " << c.description << '\n';
  }
}

} // namespace

int LogicSpec::run(ArgCursor &args, std::ostream &out) {
  if (!args.has_remaining()) {
    return kEnterSubShell; // bare `logic`: Shell pushes the session
  }
  std::optional<std::string_view> verb = args.next_positional();
  if (!verb) {
    // Only options remain (positionals skip "-..." tokens): honor --help,
    // otherwise the option is stray. Never dereference a disengaged verb.
    if (args.take_flag({"--help", "-h"})) {
      out << kName << " - " << kDescription << "\nUsage:\n" << kUsage << '\n';
      print_verbs(out);
      return core::kExitOk;
    }
    return unexpected_error(out, *args.first_remaining(), kUsage);
  }
  // Session control has no meaning one-shot: loud hint, never silent.
  if (verb == "back" || verb == "exit" || verb == "quit") {
    out << "Error: '" << *verb << "' leaves the logic session, but no session is active in one-shot mode\n";
    out << "(bare `logic` enters the session in the REPL)\n";
    return core::kExitUsage;
  }
  const CommandDef *def = logic::LogicRegistry::find(*verb);
  if (def == nullptr) {
    out << "Error: unknown logic command '" << *verb << "'\n";
    print_verbs(out);
    out << kUsage << '\n';
    return core::kExitUsage;
  }
  const int rc = def->run(args, out);
  if (rc == kExitSubShell) {
    // Only `back` returns this, and it is rejected above one-shot.
    out << "Error: 'back' leaves the logic session, but no session is active in one-shot mode\n";
    return core::kExitUsage;
  }
  return rc;
}

void LogicSpec::complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out) {
  complete_verbs<logic::LogicRegistry>(prior_args, partial, out);
}

} // namespace wengine::cli::commands

namespace wengine::cli::logic {

namespace {

wengine::logic::Netlist &workspace_impl() {
  static wengine::logic::Netlist nl{};
  return nl;
}

} // namespace

::wengine::logic::Netlist &workspace() { return workspace_impl(); }

int LogicHelpSpec::run(ArgCursor &args, std::ostream &out) {
  if (!args.has_remaining()) {
    out << "Logic session commands (prompt [logic]> ; `back` leaves):\n";
    for (const auto &c : LogicRegistry::all()) {
      out << "  " << c.name << " - " << c.description << '\n';
    }
    return core::kExitOk;
  }
  std::optional<std::string_view> verb = args.next_positional();
  if (!verb) {
    // Only options remain: honor --help, otherwise the option is stray.
    if (args.take_flag({"--help", "-h"})) {
      out << kName << " - " << kDescription << "\nUsage:\n" << kUsage << '\n';
      return core::kExitOk;
    }
    return cli::unexpected_error(out, *args.first_remaining(), kUsage);
  }
  const CommandDef *def = LogicRegistry::find(*verb);
  if (def == nullptr) {
    out << "Error: unknown logic command '" << *verb << "'\n";
    return core::kExitRuntime;
  }
  if (args.has_remaining()) {
    return cli::unexpected_error(out, *args.first_remaining(), def->usage);
  }
  out << def->name << " - " << def->description << '\n';
  if (!def->usage.empty()) {
    out << "Usage:\n" << def->usage << '\n';
  }
  return core::kExitOk;
}

void LogicHelpSpec::complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out) {
  if (!prior_args.empty()) {
    return;
  }
  for (const auto &c : LogicRegistry::all()) {
    if (c.name.substr(0, partial.size()) == partial) {
      out.emplace_back(c.name);
    }
  }
}

int LogicBackSpec::run(ArgCursor &args, std::ostream &out) {
  if (args.take_flag({"--help", "-h"}) && !args.has_remaining()) {
    out << kName << " - " << kDescription << "\nUsage:\n" << kUsage << '\n';
    return core::kExitOk;
  }
  if (args.has_remaining()) {
    return cli::unexpected_error(out, *args.first_remaining(), kUsage);
  }
  return kExitSubShell; // session loop pops; one-shot maps to a hint (see LogicSpec)
}

void LogicBackSpec::complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out) {
  (void)prior_args;
  if (partial.empty()) {
    return; // `back` takes no arguments; nothing to complete
  }
  constexpr std::string_view kHelp = "--help";
  if (kHelp.substr(0, partial.size()) == partial) {
    out.emplace_back(kHelp);
  }
}

} // namespace wengine::cli::logic
