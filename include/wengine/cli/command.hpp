#pragma once

/**
 * @file
 * Compile-time command registry: the command spec structure every CLI
 * command implements (kName, kDescription, kUsage, run, optional
 * complete) plus the consteval-validated Registry template. Guarantees
 * checked at compile time via consteval: every name non-empty and
 * whitespace-free, every handler non-null, all names unique. Lookup
 * find() is a constexpr linear search: fastest for the small command
 * counts here (no hash, no allocations, branch-predictor friendly).
 * Adding a command means defining one spec struct and appending it to
 * the registry list (see command_registry.hpp and AGENTS.md section 9);
 * each command parses named `--key value` / `--key=value` arguments via
 * ArgCursor, refuses stray positionals loudly, and exits 0/1/2 (with -1
 * reserved for the Shell exit signal). Consumers: Shell, the completion
 * engine, every command implementation, and the `ic`/logic two-level
 * dispatches (chips and logic verbs are plain specs, so the same
 * validation applies unchanged). Dependency direction: cli to solvers
 * and core, never the reverse.
 */

// Compile-time command registry.
//
// Usage: define one spec struct per command, then alias a registry:
//
//   struct OhmSpec {
//     static constexpr std::string_view kName = "ohm";
//     static constexpr std::string_view kDescription = "Solve resistor";
//     static constexpr std::string_view kUsage = "ohm [U] [I] [--u V] ...";
//     static int run(ArgCursor &args, std::ostream &out);
//     // optional Tab completion for arguments:
//     static void complete(std::span<const std::string_view> prior_args,
//                          std::string_view partial, std::vector<std::string> &out);
//   };
//   using CliRegistry = Registry<OhmSpec, ...>;
//
// Guarantees (checked at compile time via consteval):
// - every name non-empty, no whitespace, unique
// - every handler non-null
// Lookup `find()` is constexpr linear search: fastest for the small N
// typical of CLIs (no hash, no allocations, branch-predictor friendly).

#include "wengine/cli/args.hpp"

#include <array>
#include <concepts>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wengine::cli {

/**
 * @brief Command entry point: tokens after the command name plus output.
 * @note Returns the process exit code (0 success, 1 runtime/check
 *       failure, 2 usage error); -1 is the Shell exit signal, and module
 *       sessions use kEnterSubShell/kExitSubShell - never a process code.
 */
using Handler = int (*)(ArgCursor &, std::ostream &);

// Completes one argument: prior tokens after the command name, the partial
// token under the cursor, output candidates (without trailing space).
/**
 * @brief Tab-completion for one argument position.
 * @note Called with the tokens after the command name, the partial token
 *       under the cursor, and an output vector taking candidates without
 *       trailing space (common-prefix folding happens in complete.hpp).
 */
using CompleteFn = void (*)(std::span<const std::string_view> prior_args,
                            std::string_view partial, std::vector<std::string> &out);

/**
 * @brief One registered command: identity, help text, and entry points.
 */
struct CommandDef {
  /**
   * @brief Command name token (non-empty, whitespace-free, unique).
   */
  std::string_view name;
  /**
   * @brief One-line summary shown by help listings.
   */
  std::string_view description;
  /**
   * @brief Full usage text (may be empty; printed with usage errors).
   */
  std::string_view usage;
  /**
   * @brief Command entry point (never null; validated at compile time).
   */
  Handler run;
  /**
   * @brief Argument completer (null when the command completes nothing).
   */
  CompleteFn complete = nullptr; // optional: Tab completion for arguments
};

/**
 * @brief Compile-time concept for one command spec struct.
 * @note Requires kName (non-empty), kDescription, and run(); kUsage and
 *       complete() are picked up when present (see detail::make_def).
 */
template <typename S>
concept CommandSpec = requires(ArgCursor &a, std::ostream &o) {
  { S::kName } -> std::convertible_to<std::string_view>;
  { S::kDescription } -> std::convertible_to<std::string_view>;
  { S::run(a, o) } -> std::convertible_to<int>;
} && !std::string_view(S::kName).empty();

namespace detail {

/**
 * @brief Reject blank/whitespace names (consteval registry validation).
 * @param[in] s Command name candidate.
 * @return True when the name contains a space, tab, newline, or
 *         carriage return.
 */
[[nodiscard]] consteval bool has_whitespace(std::string_view s) {
  for (char c : s) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      return true;
    }
  }
  return false;
}

/**
 * @brief Build one CommandDef from a spec (picks up kUsage/complete).
 * @return The fully populated registry entry: name/description from the
 *         spec's required members, usage from kUsage, run bound to
 *         S::run, and complete bound to S::complete when the spec
 *         provides it.
 * @note kUsage defaults to empty when the spec omits it; complete
 *       defaults to null when the spec omits it.
 */
template <CommandSpec S>
[[nodiscard]] consteval CommandDef make_def() {
  std::string_view usage{};
  if constexpr (requires { S::kUsage; }) {
    usage = S::kUsage;
  }
  CompleteFn complete = nullptr;
  if constexpr (requires(std::span<const std::string_view> a, std::string_view p,
                         std::vector<std::string> &o) { S::complete(a, p, o); }) {
    complete = &S::complete;
  }
  return {.name = S::kName,
          .description = S::kDescription,
          .usage = usage,
          .run = &S::run,
          .complete = complete};
}

/**
 * @brief Validate a registry table (non-empty names, no null handlers).
 * @param[in] defs Candidate command table.
 * @return False on an empty/whitespace name, a null handler, or a
 *         duplicate name; true otherwise (fed to static_assert).
 */
template <std::size_t N>
[[nodiscard]] consteval bool validate_defs(const std::array<CommandDef, N> &defs) {
  for (std::size_t i = 0; i < N; ++i) {
    if (defs[i].name.empty() || has_whitespace(defs[i].name)) {
      return false;
    }
    if (defs[i].run == nullptr) {
      return false;
    }
    for (std::size_t j = i + 1; j < N; ++j) {
      if (defs[i].name == defs[j].name) {
        return false; // duplicate command name
      }
    }
  }
  return true;
}

} // namespace detail

/**
 * @brief Compile-time command table with validated lookup.
 * @note kCommands is validated by static_assert (empty names, whitespace,
 *       duplicates, and null handlers fail the build, never the run).
 *       The registry-size static_assert in tests/test_cli_args.cpp must
 *       stay in sync when commands are added.
 */
template <CommandSpec... Specs>
struct Registry {
  /**
   * @brief Validated command table (static_asserted at compile time).
   */
  static constexpr std::array<CommandDef, sizeof...(Specs)> kCommands = {
      detail::make_def<Specs>()...};

  static_assert(detail::validate_defs(kCommands),
                "CLI registry invalid: empty/whitespace/duplicate name or null handler");

  /**
   * @brief Find a command by name (constexpr linear search).
   * @param[in] name Command token to look up.
   * @return Pointer to the definition, or nullptr when unknown.
   */
  [[nodiscard]] static constexpr const CommandDef *find(std::string_view name) noexcept {
    for (const auto &c : kCommands) {
      if (c.name == name) {
        return &c;
      }
    }
    return nullptr;
  }

  /**
   * @brief Whole command span (help listings, completion, tests).
   * @return Span over the validated command table.
   */
  [[nodiscard]] static constexpr std::span<const CommandDef> all() noexcept {
    return {kCommands.data(), kCommands.size()};
  }
};
} // namespace wengine::cli
