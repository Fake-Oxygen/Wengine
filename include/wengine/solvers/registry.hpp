#pragma once

// Compile-time solver registry. Mirrors cli::Registry:
//
//   struct ProjectileSolver : public core::ISolver {
//     static constexpr std::string_view kName = "projectile";
//     static constexpr std::string_view kDescription = "...";
//     static std::unique_ptr<core::ISolver> create();
//     ...
//   };
//   using SolverRegistry = Registry<ProjectileSolver, ...>;
//
// Guarantees (checked at compile time via consteval):
// - every name non-empty, no whitespace, unique
// - every factory non-null
// Lookup `find()` is constexpr linear search: fastest for the small N
// typical of solver catalogs (no hash, no allocations).
// Factories build default-configured instances; callers configure the
// solver before handing it to core::Runner, which owns no solvers itself
// (the headless path centralizes this in solvers::run_headless).

#include "wengine/core/solver.hpp"

#include <array>
#include <concepts>
#include <memory>
#include <span>
#include <string_view>

namespace wengine::solvers {

using SolverFactory = std::unique_ptr<core::ISolver> (*)();

struct SolverDef {
  std::string_view name;
  std::string_view description;
  SolverFactory create;
};

template <typename S>
concept SolverSpec = requires {
  { S::kName } -> std::convertible_to<std::string_view>;
  { S::kDescription } -> std::convertible_to<std::string_view>;
  { S::create() } -> std::convertible_to<std::unique_ptr<core::ISolver>>;
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

template <SolverSpec S>
[[nodiscard]] consteval SolverDef make_def() {
  return {.name = S::kName, .description = S::kDescription, .create = &S::create};
}

template <std::size_t N>
[[nodiscard]] consteval bool validate_defs(const std::array<SolverDef, N> &defs) {
  for (std::size_t i = 0; i < N; ++i) {
    if (defs[i].name.empty() || has_whitespace(defs[i].name)) {
      return false;
    }
    if (defs[i].create == nullptr) {
      return false;
    }
    for (std::size_t j = i + 1; j < N; ++j) {
      if (defs[i].name == defs[j].name) {
        return false; // duplicate solver name
      }
    }
  }
  return true;
}

} // namespace detail

template <SolverSpec... Specs>
struct Registry {
  static constexpr std::array<SolverDef, sizeof...(Specs)> kSolvers = {
      detail::make_def<Specs>()...};

  static_assert(detail::validate_defs(kSolvers),
                "Solver registry invalid: empty/whitespace/duplicate name or null factory");

  [[nodiscard]] static constexpr const SolverDef *find(std::string_view name) noexcept {
    for (const auto &d : kSolvers) {
      if (d.name == name) {
        return &d;
      }
    }
    return nullptr;
  }

  [[nodiscard]] static constexpr std::span<const SolverDef> all() noexcept {
    return {kSolvers.data(), kSolvers.size()};
  }
};

} // namespace wengine::solvers
