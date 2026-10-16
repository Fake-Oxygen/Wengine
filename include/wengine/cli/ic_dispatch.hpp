#pragma once

// Two-level dispatch for the `ic` command (IC-specific calculators).
//
// The top level stays fixed (`ic <chip> <op> ...`): new chips register in
// `IcRegistry` (see `wengine/cli/ic_registry.hpp`) and never become new
// top-level commands. Both helpers are templates over any chip registry so
// tests can exercise them with local dummy chips without touching the
// production registry.
//
// Chips are plain `CommandSpec`s (`kName` = lowercase part number, `run`,
// optional `kUsage`/`complete`), so the existing compile-time registry
// validation (unique, non-empty, whitespace-free names) applies unchanged.

#include "wengine/cli/command.hpp"
#include "wengine/cli/status.hpp"
#include "wengine/core/status.hpp"

#include <concepts>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wengine::cli::ic {

template <typename R>
concept ChipRegistry = requires {
  { R::find(std::string_view{}) } -> std::convertible_to<const CommandDef *>;
  { R::all() } -> std::convertible_to<std::span<const CommandDef>>;
};

namespace detail {

template <ChipRegistry R>
void list_chips(std::ostream &out) {
  if (R::all().empty()) {
    out << "  (none registered yet)\n";
    return;
  }
  for (const auto &c : R::all()) {
    out << "  " << c.name << " - " << c.description << '\n';
  }
}

} // namespace detail

// Route `args` (tokens after `ic`) to the chip named by the first
// positional. The chip token is consumed, so the chip's own `run` sees
// only the rest — including a trailing `--help`, which means
// `ic <chip> --help` prints the chip help, not the top-level help. With
// no chip positional, `--help` prints the top-level help (with the live
// chip list); anything else is a usage error.
template <ChipRegistry R>
int dispatch(ArgCursor &args, std::ostream &out, std::string_view top_name,
             std::string_view top_description, std::string_view top_usage) {
  std::optional<std::string_view> chip = args.next_positional();
  if (!chip) {
    if (args.take_flag({"--help", "-h"})) {
      out << top_name << " - " << top_description << "\nUsage:\n" << top_usage << '\n';
      out << "Available ICs:\n";
      detail::list_chips<R>(out);
      return core::kExitOk;
    }
    if (args.has_remaining()) {
      return cli::unexpected_error(out, *args.first_remaining(), top_usage);
    }
    return cli::usage_error(out, "missing chip (ic <chip> <op> ...)", top_usage);
  }
  const CommandDef *def = R::find(*chip);
  if (def == nullptr) {
    out << "Error: unknown IC '" << *chip << "'\n";
    out << "Available ICs:\n";
    detail::list_chips<R>(out);
    out << top_usage << '\n';
    return core::kExitUsage;
  }
  return def->run(args, out);
}

// Complete `ic` arguments: no prior tokens -> chip names (plus `--help`);
// a known chip first -> delegate to that chip's `complete` (if any) with
// the chip token stripped. Unknown chips complete to nothing.
template <ChipRegistry R>
void complete(std::span<const std::string_view> prior_args, std::string_view partial,
              std::vector<std::string> &out) {
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

} // namespace wengine::cli::ic
