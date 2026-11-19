#pragma once

/**
 * @file
 * Generic LED series-resistor solver: exact Rs from headroom and
 * forward current, E12 snap (reusing the tested I2C snap, including the
 * next-decade rule), then power flows at the used resistor with an
 * optional power-rating verdict. Owns the pure textbook math only —
 * depends on units (and math) and i2c.hpp's E12 helper, never CLI
 * types; input validation (VS > 0, VF > 0, IF > 0, ...) is the CLI
 * layer's job. Consumed by the `led` CLI command and tests/test_led.cpp.
 */

// Generic LED series-resistor solver.
//
// An LED run at forward voltage VF and forward current IF from a supply
// VS needs a series resistor dropping the headroom VS - VF:
//
//   headroom = VS - VF                    // must stay > 0 for regulation
//   Rs       = headroom / IF              // exact value
//   Rs(E12)  = nearest E12 to Rs         // preferred pick (reuses the
//                                        // I2C solver's tested E12 snap,
//                                        // incl. the next-decade rule)
//   IF'      = headroom / Rs_used        // actual current at the used Rs
//   Pres     = headroom * IF'            // resistor dissipation
//   Pled     = VF * IF'                  // LED power
//   Ptot     = VS * IF'                  // supply power
//   eff      = Pled / Ptot = VF / VS     // fraction (DC steady state)
//
// An explicit Rs choice (resistor at hand) is evaluated as-is; otherwise
// the E12 pick is used. An optional resistor power rating gives a
// PASS/FAIL verdict (Pres <= Prating), mirroring the LDO thermal limit.
//
// All functions are pure, header-only, constexpr + noexcept.
// Input validation (VS > 0, VF > 0, IF > 0, ...) is the caller's job
// (CLI layer).

#include "wengine/solvers/i2c.hpp"
#include "wengine/units/units.hpp"

#include <limits>

namespace wengine::solvers {

/** @brief Complete LED series-resistor operating point (all SI units). */
struct LedResult {
  /** @brief Supply-to-forward headroom VS - VF, in volts. */
  double headroom_v{0.0};
  double rs_ohm{0.0}; /**< @brief Exact series resistor headroom/IF, in ohms (<= 0 when infeasible). */
  double rs_e12_ohm{0.0}; /**< @brief Nearest E12 to the exact value, in ohms (0 when infeasible). */
  double rs_used_ohm{0.0}; /**< @brief Evaluated value: explicit choice or the E12 pick, in ohms. */
  /** @brief Requested forward current, in amperes. */
  double if_target_a{0.0};
  double if_actual_a{0.0}; /**< @brief Actual forward current at rs_used, in amperes. */
  double p_res_w{0.0}; /**< @brief Resistor dissipation at the actual current, in watts. */
  /** @brief LED power VF * IF', in watts. */
  double p_led_w{0.0};
  /** @brief Supply power VS * IF', in watts. */
  double p_total_w{0.0};
  double eff{0.0}; /**< @brief Efficiency Pled/Ptot as a fraction (1.0 when Ptot == Pled == 0, inf when Ptot == 0 < Pled). */
  bool feasible{false}; /**< @brief Headroom > 0 (regulation possible). */
  bool rating_ok{true}; /**< @brief p_res <= prating (+inf rating means no limit). */
  bool pass{false}; /**< @brief feasible && rating_ok (design verdict). */
};

/**
 * @brief Generic LED series-resistor solver (static-only closed-form utility).
 *
 * @note Pure, header-only, constexpr + noexcept: exact Rs is one
 *       division, the E12 snap reuses I2cBusSolver::e12_nearest
 *       (including the mandatory next-decade rule), and all power
 *       fields are single multiplications — no accumulation error.
 *       Infinity sentinels (IF or Rs zero) are domain values; callers
 *       validate at the CLI layer.
 */
class LedSolver {
public:
  /** @brief Deleted constructor: every solver here is a static function. */
  LedSolver() = delete;

  // ------------------------------------------------------------ primitives
  /**
   * @brief Supply-to-forward headroom.
   * @param[in] vs_v supply voltage, in volts.
   * @param[in] vf_v LED forward voltage, in volts.
   * @return VS - VF, in volts; must stay > 0 for regulation.
   */
  [[nodiscard]] static constexpr double headroom_v(double vs_v, double vf_v) noexcept {
    return vs_v - vf_v;
  }

  /**
   * @brief Exact series resistor for the target current.
   * @param[in] vs_v supply voltage, in volts.
   * @param[in] vf_v LED forward voltage, in volts.
   * @param[in] if_a target forward current, in amperes.
   * @return (VS - VF) / IF, in ohms; infinity when IF is 0.
   */
  [[nodiscard]] static constexpr double series_resistance_ohm(double vs_v, double vf_v, double if_a) noexcept {
    return if_a == 0.0 ? std::numeric_limits<double>::infinity() : (vs_v - vf_v) / if_a;
  }

  /**
   * @brief Actual forward current at a chosen resistor.
   * @param[in] vs_v supply voltage, in volts.
   * @param[in] vf_v LED forward voltage, in volts.
   * @param[in] rs_ohm series resistor in use, in ohms.
   * @return (VS - VF) / Rs, in amperes; infinity when Rs is 0.
   */
  [[nodiscard]] static constexpr double actual_current_a(double vs_v, double vf_v, double rs_ohm) noexcept {
    return rs_ohm == 0.0 ? std::numeric_limits<double>::infinity() : (vs_v - vf_v) / rs_ohm;
  }

  /**
   * @brief Resistor dissipation at a given current.
   * @param[in] vs_v supply voltage, in volts.
   * @param[in] vf_v LED forward voltage, in volts.
   * @param[in] if_a forward current, in amperes.
   * @return (VS - VF) * IF, in watts.
   */
  [[nodiscard]] static constexpr double resistor_power_w(double vs_v, double vf_v, double if_a) noexcept {
    return (vs_v - vf_v) * if_a;
  }

  /**
   * @brief LED dissipation.
   * @param[in] vf_v LED forward voltage, in volts.
   * @param[in] if_a forward current, in amperes.
   * @return VF * IF, in watts.
   */
  [[nodiscard]] static constexpr double led_power_w(double vf_v, double if_a) noexcept {
    return vf_v * if_a;
  }

  /**
   * @brief Total supply draw.
   * @param[in] vs_v supply voltage, in volts.
   * @param[in] if_a forward current, in amperes.
   * @return VS * IF, in watts.
   */
  [[nodiscard]] static constexpr double total_power_w(double vs_v, double if_a) noexcept {
    return vs_v * if_a;
  }

  /**
   * @brief DC steady-state efficiency.
   * @param[in] vs_v supply voltage, in volts.
   * @param[in] vf_v LED forward voltage, in volts.
   * @param[in] if_a forward current, in amperes.
   * @return Pled / Ptot = VF / VS as a fraction; 1.0 when both powers
   *         are zero, infinity when Ptot == 0 < Pled.
   */
  [[nodiscard]] static constexpr double efficiency(double vs_v, double vf_v, double if_a) noexcept {
    const double ptot = total_power_w(vs_v, if_a);
    if (ptot == 0.0) {
      const double pled = led_power_w(vf_v, if_a);
      return pled == 0.0 ? 1.0 : std::numeric_limits<double>::infinity();
    }
    return led_power_w(vf_v, if_a) / ptot;
  }

  // ------------------------------------------------------------- full solve
  // rs_choice_ohm <= 0 selects the automatic E12 pick.
  // prating_w = +inf (default) means no power-rating limit.
  /**
   * @brief Evaluate the full LED operating point for one corner.
   *
   * @param[in] vs_v supply voltage, in volts.
   * @param[in] vf_v LED forward voltage, in volts.
   * @param[in] if_a target forward current, in amperes.
   * @param[in] rs_choice_ohm resistor at hand, in ohms; <= 0 selects
   *            the automatic E12 pick.
   * @param[in] prating_w resistor power rating, in watts; +inf
   *            (default) means no rating limit.
   * @return The complete corner: exact and E12 resistors, actual
   *         current, power flows, efficiency, and the
   *         feasible/rating_ok/pass verdicts.
   */
  [[nodiscard]] static constexpr LedResult
  solve(double vs_v, double vf_v, double if_a, double rs_choice_ohm = 0.0, double prating_w = std::numeric_limits<double>::infinity()) noexcept {
    LedResult r{};
    r.if_target_a = if_a;
    r.headroom_v = headroom_v(vs_v, vf_v);
    r.feasible = r.headroom_v > 0.0;
    r.rs_ohm = series_resistance_ohm(vs_v, vf_v, if_a);
    if (r.feasible && r.rs_ohm > 0.0) {
      r.rs_e12_ohm = I2cBusSolver::e12_nearest(r.rs_ohm);
    }
    r.rs_used_ohm = rs_choice_ohm > 0.0 ? rs_choice_ohm : r.rs_e12_ohm;
    if (r.rs_used_ohm > 0.0) {
      r.if_actual_a = actual_current_a(vs_v, vf_v, r.rs_used_ohm);
      r.p_res_w = resistor_power_w(vs_v, vf_v, r.if_actual_a);
      r.p_led_w = led_power_w(vf_v, r.if_actual_a);
      r.p_total_w = total_power_w(vs_v, r.if_actual_a);
      r.eff = efficiency(vs_v, vf_v, r.if_actual_a);
      r.rating_ok = r.p_res_w <= prating_w;
    }
    r.pass = r.feasible && r.rs_used_ohm > 0.0 && r.rating_ok;
    return r;
  }

  // --------------------------------------------------- dimensionally-typed
  /**
   * @brief Headroom, dimensionally typed.
   * @param[in] vs supply potential (SI volts).
   * @param[in] vf forward potential (SI volts).
   * @return VS - VF as an electric-potential quantity.
   */
  [[nodiscard]] static constexpr units::Value<units::ElectricPotential>
  headroom(units::Value<units::ElectricPotential> vs, units::Value<units::ElectricPotential> vf) noexcept {
    return vs - vf;
  }

  /**
   * @brief Exact series resistor, dimensionally typed.
   * @param[in] vs supply potential (SI volts).
   * @param[in] vf forward potential (SI volts).
   * @param[in] i target current (SI amperes).
   * @return (VS - VF) / IF as an electric-resistance quantity.
   */
  [[nodiscard]] static constexpr units::Value<units::ElectricResistance>
  series_resistance(units::Value<units::ElectricPotential> vs, units::Value<units::ElectricPotential> vf, units::Value<units::ElectricCurrent> i) noexcept {
    return units::Value<units::ElectricResistance>(series_resistance_ohm(vs.get(), vf.get(), i.get()));
  }

  /**
   * @brief Actual forward current, dimensionally typed.
   * @param[in] vs supply potential (SI volts).
   * @param[in] vf forward potential (SI volts).
   * @param[in] rs series resistance in use (SI ohms).
   * @return (VS - VF) / Rs as an electric-current quantity.
   */
  [[nodiscard]] static constexpr units::Value<units::ElectricCurrent>
  actual_current(units::Value<units::ElectricPotential> vs, units::Value<units::ElectricPotential> vf, units::Value<units::ElectricResistance> rs) noexcept {
    return units::Value<units::ElectricCurrent>(actual_current_a(vs.get(), vf.get(), rs.get()));
  }

  /**
   * @brief Resistor dissipation, dimensionally typed.
   * @param[in] vs supply potential (SI volts).
   * @param[in] vf forward potential (SI volts).
   * @param[in] i forward current (SI amperes).
   * @return (VS - VF) * IF as a power quantity (SI watts).
   */
  [[nodiscard]] static constexpr units::Value<units::Power>
  resistor_power(units::Value<units::ElectricPotential> vs, units::Value<units::ElectricPotential> vf, units::Value<units::ElectricCurrent> i) noexcept {
    return units::Value<units::Power>(resistor_power_w(vs.get(), vf.get(), i.get()));
  }
};

// Exactly representable integer-volt/amp goldens (independent of the CLI).
static_assert(LedSolver::headroom_v(5.0, 3.0) == 2.0);
static_assert(LedSolver::series_resistance_ohm(5.0, 3.0, 2.0) == 1.0);
static_assert(LedSolver::actual_current_a(5.0, 3.0, 2.0) == 1.0);
static_assert(LedSolver::resistor_power_w(5.0, 3.0, 2.0) == 4.0);
static_assert(LedSolver::led_power_w(3.0, 2.0) == 6.0);
static_assert(LedSolver::total_power_w(5.0, 2.0) == 10.0);

} // namespace wengine::solvers
