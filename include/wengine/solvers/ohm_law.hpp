#pragma once

/**
 * @file
 * Ohm's law closed-form solver: fills a complete resistive operating
 * point from any two of voltage, current, and resistance. Owns the pure
 * constexpr math and the SI result struct only — depends on units (and
 * math), never CLI types; zero/divisor inputs are domain values that the
 * CLI layer validates (solvers stay pure, per core/status.hpp). Consumed
 * by the `ohm` CLI command and tests/test_ohm.cpp.
 */

#include "wengine/units/units.hpp"

namespace wengine::solvers {

/**
 * @brief Complete resistive operating point, all fields SI quantities.
 */
struct ResistorState {
  /** @brief Voltage drop across the resistor, in volts (V). */
  units::Value<units::ElectricPotential> voltage{0.0};
  /** @brief Current through the resistor, in amperes (A). */
  units::Value<units::ElectricCurrent> current{0.0};
  /** @brief Resistance, in ohms. */
  units::Value<units::ElectricResistance> resistance{0.0};
  /** @brief Dissipated power U*I, in watts (W). */
  units::Value<units::Power> power{0.0};
};

/**
 * @brief Ohm's law closed-form solver (static-only utility).
 *
 * @note Pure, header-only, constexpr + noexcept: each field is one
 *       closed-form expression, so results are exactly reproducible —
 *       no accumulation error, no state, no I/O.
 */
class OhmLawSolver {
public:
  /** @brief Deleted constructor: every solver here is a static function. */
  OhmLawSolver() = delete;

  /**
   * @brief Solve from terminal voltage and current.
   *
   * @param[in] u voltage drop, in volts (V).
   * @param[in] i current, in amperes (A).
   * @return Filled state with resistance U/I and power U*I.
   * @note Closed-form exact; i = 0 yields an infinite resistance per
   *       IEEE-754 (domain value, never an exception or Status).
   */
  [[nodiscard]] static constexpr ResistorState solve_from_ui(
      units::Value<units::ElectricPotential> u,
      units::Value<units::ElectricCurrent> i) noexcept {
    return {u, i, u / i, u * i};
  }

  /**
   * @brief Solve from current and resistance.
   *
   * @param[in] i current, in amperes (A).
   * @param[in] r resistance, in ohms.
   * @return Filled state with voltage I*R and power U*I.
   * @note Closed-form exact for all finite inputs.
   */
  [[nodiscard]] static constexpr ResistorState solve_from_ir(
      units::Value<units::ElectricCurrent> i,
      units::Value<units::ElectricResistance> r) noexcept {
    const units::Value<units::ElectricPotential> u = i * r;
    return {u, i, r, u * i};
  }

  /**
   * @brief Solve from terminal voltage and resistance.
   *
   * @param[in] u voltage drop, in volts (V).
   * @param[in] r resistance, in ohms.
   * @return Filled state with current U/R and power U*I.
   * @note Closed-form exact; r = 0 yields an infinite current per
   *       IEEE-754 (domain value, never an exception or Status).
   */
  [[nodiscard]] static constexpr ResistorState solve_from_ur(
      units::Value<units::ElectricPotential> u,
      units::Value<units::ElectricResistance> r) noexcept {
    const units::Value<units::ElectricCurrent> i = u / r;
    return {u, i, r, u * i};
  }
};

} // namespace wengine::solvers
