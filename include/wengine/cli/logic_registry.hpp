#pragma once

// Module registry for the `logic` sub-shell (LOGIC-1: session entry +
// `help`/`back` session verbs; LOGIC-8 appends the solver verbs).
//
// To add a verb: declare one CommandSpec below (kName, kDescription,
// kUsage, run, optional complete), append it to LogicRegistry, keep the
// registry-size static_assert in tests/test_cli_args.cpp in sync.
// Verbs dispatch identically one-shot (`logic <verb> ...`) and in-session
// (`<verb> ...` at the `[logic]> ` prompt); session control
// (`back`/`exit`/`quit`) is interpreted by Shell, never by verbs —
// except `back`, which is a real verb (validates args, returns
// kExitSubShell) so completion and behavior stay uniform.

#include "wengine/cli/command.hpp"

#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wengine::cli::logic {

inline constexpr std::string_view kSessionName = "logic";
inline constexpr std::string_view kSessionPrompt = "[logic]> ";
inline constexpr std::string_view kSessionExitHint = "back - leave the logic session";

struct LogicHelpSpec {
  static constexpr std::string_view kName = "help";
  static constexpr std::string_view kDescription = "Show logic-session commands";
  static constexpr std::string_view kUsage =
      "help [verb]   (inside the logic session, or `logic help [verb]` one-shot)\n"
      "  help         list session verbs\n"
      "  help back    show one verb's usage";

  static int run(ArgCursor &args, std::ostream &out);
  static void complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out);
};

struct LogicBackSpec {
  static constexpr std::string_view kName = "back";
  static constexpr std::string_view kDescription = "Leave the logic session";
  static constexpr std::string_view kUsage = "back   (no arguments; `exit`/`quit` also leave the session)";

  static int run(ArgCursor &args, std::ostream &out);
  static void complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out);
};

// Append new verb specs here; names stay unique via compile-time validation.
using LogicRegistry = Registry<LogicHelpSpec, LogicBackSpec>;

} // namespace wengine::cli::logic
