#pragma once

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

using Handler = int (*)(ArgCursor &, std::ostream &);

// Completes one argument: prior tokens after the command name, the partial
// token under the cursor, output candidates (without trailing space).
using CompleteFn = void (*)(std::span<const std::string_view> prior_args,
                            std::string_view partial, std::vector<std::string> &out);

struct CommandDef {
  std::string_view name;
  std::string_view description;
  std::string_view usage;
  Handler run;
  CompleteFn complete = nullptr; // optional: Tab completion for arguments
};

template <typename S>
concept CommandSpec = requires(ArgCursor &a, std::ostream &o) {
  { S::kName } -> std::convertible_to<std::string_view>;
  { S::kDescription } -> std::convertible_to<std::string_view>;
  { S::run(a, o) } -> std::convertible_to<int>;
} && !std::string_view(S::kName).empty();

namespace detail {

[[nodiscard]] consteval bool has_whitespace(std::string_view s) {
  for (char c : s) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      return true;
    }
  }
  return false;
}

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

template <CommandSpec... Specs>
struct Registry {
  static constexpr std::array<CommandDef, sizeof...(Specs)> kCommands = {
      detail::make_def<Specs>()...};

  static_assert(detail::validate_defs(kCommands),
                "CLI registry invalid: empty/whitespace/duplicate name or null handler");

  [[nodiscard]] static constexpr const CommandDef *find(std::string_view name) noexcept {
    for (const auto &c : kCommands) {
      if (c.name == name) {
        return &c;
      }
    }
    return nullptr;
  }

  [[nodiscard]] static constexpr std::span<const CommandDef> all() noexcept {
    return {kCommands.data(), kCommands.size()};
  }
};
} // namespace wengine::cli
