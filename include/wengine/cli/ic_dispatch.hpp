#pragma once

/**
 * @file
 * Two-level dispatch for the `ic` command (IC-specific calculators).
 * The top level stays fixed (`ic` plus chip plus op): new chips register
 * in IcRegistry (see wengine/cli/ic_registry.hpp) and never become new
 * top-level commands. Both helpers are templates over any chip registry
 * so tests can exercise them with local dummy chips without touching the
 * production registry. Chips are plain CommandSpecs (kName is the
 * lowercase part number, run, optional kUsage/complete), so the existing
 * compile-time registry validation (unique, non-empty, whitespace-free
 * names) applies unchanged. Consumers: the IcSpec command implementation
 * and its completion entry. Dependency direction: cli to solvers and
 * core, never the reverse.
 */

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

/**
 * @brief Concept for any chip registry dispatchable by `ic`.
 * @note Requires the same find()/all() shape as cli::Registry, so the
 *       production IcRegistry and test-local dummy registries route
 *       identically.
 */
template <typename R>
concept ChipRegistry = requires {
  { R::find(std::string_view{}) } -> std::convertible_to<const CommandDef *>;
  { R::all() } -> std::convertible_to<std::span<const CommandDef>>;
};

namespace detail {

/**
 * @brief Print the chip list (one `name - description` line per chip).
 * @param[out] out Stream receiving the list (or the empty-registry note).
 */
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
/**
 * @brief Route tokens after `ic` to the named chip spec.
 * @param[in,out] args Tokens after `ic`; the chip token is consumed so
 *            the chip run() sees only the rest.
 * @param[out] out Stream receiving chip output, the chip list, or errors.
 * @param[in] top_name Top-level command name for the help header.
 * @param[in] top_description Top-level description for the help header.
 * @param[in] top_usage Top-level usage printed with usage errors.
 * @return The chip run() result; kExitOk with the top-level help
 *         (plus live chip list) on `--help` with no chip; kExitUsage on
 *         a missing or unknown chip (unknown chips list the registry).
 * @note `ic` plus chip plus `--help` prints the chip help, not the
 *       top-level help; chips never become top-level commands.
 */
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
/**
 * @brief Complete `ic` arguments (chip names, then chip-local options).
 * @param[in] prior_args Tokens after `ic` before the partial token.
 * @param[in] partial Partial token under the cursor.
 * @param[out] out Chip names (no priors) or the chip completer output
 *             (known chip first); unknown chips complete to nothing.
 */
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
