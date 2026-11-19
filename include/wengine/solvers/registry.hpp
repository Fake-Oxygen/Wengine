#pragma once

/**
 * @file
 * Compile-time solver catalog machinery: SolverDef rows, the SolverSpec
 * contract every stepped solver must satisfy, and the Registry template
 * that validates and stores the catalog at compile time. Owns catalog
 * structure only — depends on core::ISolver (units, math downstream),
 * never CLI types; the CLI consumes this, never the reverse. Instantiated
 * by solvers/solver_registry.hpp and looked up by ConfiguredRun builders.
 */

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

/** @brief Factory signature: builds one default-configured solver instance. */
using SolverFactory = std::unique_ptr<core::ISolver> (*)();

/** @brief One catalog row: a named solver plus its factory. */
struct SolverDef {
  /** @brief Solver name (registry key): non-empty, no whitespace, unique. */
  std::string_view name;
  /** @brief One-line human description shown by `sim` listing and help. */
  std::string_view description;
  /** @brief Non-null factory building a default-configured instance. */
  SolverFactory create;
};

/**
 * @brief Contract every stepped solver class must satisfy to register.
 *
 * Requires kName (string_view), kDescription (string_view), and a static
 * create() returning a unique_ptr<core::ISolver>; kName must be non-empty.
 */
template <typename S>
concept SolverSpec = requires {
  { S::kName } -> std::convertible_to<std::string_view>;
  { S::kDescription } -> std::convertible_to<std::string_view>;
  { S::create() } -> std::convertible_to<std::unique_ptr<core::ISolver>>;
} && !std::string_view(S::kName).empty();

namespace detail {

/**
 * @brief True when s contains any space, tab, CR, or LF (name validity).
 * @param[in] s candidate solver name.
 * @return true when any whitespace character is present.
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
 * @brief Build one catalog row from a SolverSpec class.
 * @return The SolverDef carrying S::kName, S::kDescription, and a
 *         pointer to S::create.
 */
template <SolverSpec S>
[[nodiscard]] consteval SolverDef make_def() {
  return {.name = S::kName, .description = S::kDescription, .create = &S::create};
}

/**
 * @brief Compile-time catalog validation (consteval).
 *
 * @param[in] defs the full catalog array.
 * @return true when every name is non-empty and whitespace-free, every
 *         factory is non-null, and no name repeats.
 */
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

/**
 * @brief Compile-time solver registry over SolverSpec classes.
 *
 * @tparam Specs zero or more solver classes; each must satisfy SolverSpec.
 * @note Registration stays a single obvious entry: append the class to
 *       the Registry<...> list in solver_registry.hpp — validation
 *       (unique non-empty names, non-null factories) happens at compile
 *       time via static_assert, and lookup is a constexpr linear search
 *       (fastest for the small N of solver catalogs: no hash, no
 *       allocations).
 */
template <SolverSpec... Specs>
struct Registry {
  /** @brief The validated catalog rows, in registration (template) order. */
  static constexpr std::array<SolverDef, sizeof...(Specs)> kSolvers = {
      detail::make_def<Specs>()...};

  static_assert(detail::validate_defs(kSolvers),
                "Solver registry invalid: empty/whitespace/duplicate name or null factory");

  /**
   * @brief Look up one solver by name.
   *
   * @param[in] name exact registry key (e.g. ProjectileSolver::kName).
   * @return Pointer to the matching catalog row, or nullptr when the
   *         name is not registered (callers map that to a usage error).
   */
  [[nodiscard]] static constexpr const SolverDef *find(std::string_view name) noexcept {
    for (const auto &d : kSolvers) {
      if (d.name == name) {
        return &d;
      }
    }
    return nullptr;
  }

  /**
   * @brief Read-only view of the whole catalog, in registration order.
   * @return Span over kSolvers for listing and help output.
   */
  [[nodiscard]] static constexpr std::span<const SolverDef> all() noexcept {
    return {kSolvers.data(), kSolvers.size()};
  }
};

} // namespace wengine::solvers
