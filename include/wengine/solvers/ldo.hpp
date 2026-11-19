#pragma once

/**
 * @file
 * Generic LDO (low-dropout linear regulator) operating-point solver:
 * headroom, dropout limit, quiescent current, power flows, efficiency,
 * and a first-order junction-temperature check. Owns the pure textbook
 * identities only (no part-specific numbers) — depends on units (and
 * math), never CLI types; input validation (VIN > 0, VOUT > 0,
 * currents >= 0, ...) is the CLI layer's job. Consumed by the `ldo`
 * CLI command and tests/test_ldo.cpp.
 */

// Generic LDO (low-dropout linear regulator) operating-point solver.
//
// A linear regulator holds VOUT fixed while the pass element drops
// VIN - VOUT at the load current. The power relationships below are
// textbook identities (no part-specific numbers, no datasheet needed):
//
//   headroom  = VIN - VOUT                  // must stay >= Vdo for regulation
//   VIN(min)  = VOUT + Vdo                  // lowest input that still regulates
//   IIN       = IOUT + Iq                   // Iq = ground-pin quiescent current
//   Ppass     = (VIN - VOUT) * IOUT         // heat in the pass element
//   Pq        = VIN * Iq                    // quiescent loss
//   Ptotal    = Ppass + Pq
//   POUT      = VOUT * IOUT
//   PIN       = VIN * IIN
//   eff       = POUT / PIN                  // fraction 0..1 (DC steady state)
//   TJ        = TA + Ptotal * RthJA         // first-order thermal model
//
// All functions are pure, header-only, constexpr + noexcept.
// Input validation (VIN > 0, VOUT > 0, currents >= 0, ...) is the
// caller's job (CLI layer). Temperatures are plain doubles in degC
// (deltas are identical in K); RthJA is plain C/W.

#include "wengine/units/units.hpp"

#include <limits>

namespace wengine::solvers {

/** @brief Complete LDO operating point for one (VIN, VOUT, IOUT) corner. */
struct LdoResult {
  /** @brief VIN - VOUT, in volts. */
  double headroom_v{0.0};
  /** @brief Lowest input that still regulates (VOUT + Vdo), in volts. */
  double vin_min_v{0.0};
  /** @brief Input current IOUT + Iq, in amperes. */
  double iin_a{0.0};
  /** @brief Delivered output power VOUT * IOUT, in watts. */
  double pout_w{0.0};
  /** @brief Drawn input power VIN * IIN, in watts. */
  double pin_w{0.0};
  /** @brief Pass-element dissipation (VIN - VOUT) * IOUT, in watts. */
  double p_pass_w{0.0};
  /** @brief Quiescent loss VIN * Iq, in watts. */
  double p_q_w{0.0};
  /** @brief Total device dissipation Ppass + Pq, in watts. */
  double p_total_w{0.0};
  double eff{0.0}; /**< @brief Efficiency POUT/PIN as a fraction 0..1 (1.0 when PIN == POUT == 0, inf when PIN == 0 < POUT). */
  bool dropout_ok{false}; /**< @brief Headroom >= Vdo (regulation verdict). */
  /** @brief Junction temperature TA + Ptotal * RthJA, in degrees C. */
  double tj_c{0.0};
  bool thermal_ok{true}; /**< @brief tj <= tjmax (tjmax = +inf means no limit). */
  bool pass{false}; /**< @brief dropout_ok && thermal_ok (design verdict). */
};

/**
 * @brief Generic LDO operating-point solver (static-only closed-form utility).
 *
 * @note Pure, header-only, constexpr + noexcept: every field is one
 *       textbook identity (no part-specific datasheet, no accumulation
 *       error). Infinity sentinels (efficiency with PIN == 0) are
 *       domain values; callers validate at the CLI layer. Temperatures
 *       are plain doubles in degrees C (deltas identical in K); RthJA
 *       is plain C/W.
 */
class LdoSolver {
public:
  /** @brief Deleted constructor: every solver here is a static function. */
  LdoSolver() = delete;

  // ------------------------------------------------------------ primitives
  /**
   * @brief Pass-element voltage headroom.
   * @param[in] vin_v input voltage, in volts.
   * @param[in] vout_v regulated output voltage, in volts.
   * @return VIN - VOUT, in volts; must stay >= Vdo for regulation.
   */
  [[nodiscard]] static constexpr double headroom_v(double vin_v, double vout_v) noexcept {
    return vin_v - vout_v;
  }

  /**
   * @brief Lowest input that still regulates.
   * @param[in] vout_v regulated output voltage, in volts.
   * @param[in] vdo_v dropout voltage at the load current, in volts.
   * @return VOUT + Vdo, in volts.
   */
  [[nodiscard]] static constexpr double vin_min_v(double vout_v, double vdo_v) noexcept {
    return vout_v + vdo_v;
  }

  /**
   * @brief Ground-pin input current.
   * @param[in] iout_a load current, in amperes.
   * @param[in] iq_a quiescent (ground-pin) current, in amperes.
   * @return IOUT + Iq, in amperes.
   */
  [[nodiscard]] static constexpr double input_current_a(double iout_a, double iq_a) noexcept {
    return iout_a + iq_a;
  }

  /**
   * @brief Pass-element dissipation.
   * @param[in] vin_v input voltage, in volts.
   * @param[in] vout_v output voltage, in volts.
   * @param[in] iout_a load current, in amperes.
   * @return (VIN - VOUT) * IOUT, in watts.
   */
  [[nodiscard]] static constexpr double pass_power_w(double vin_v, double vout_v, double iout_a) noexcept {
    return (vin_v - vout_v) * iout_a;
  }

  /**
   * @brief Quiescent dissipation.
   * @param[in] vin_v input voltage, in volts.
   * @param[in] iq_a quiescent current, in amperes.
   * @return VIN * Iq, in watts.
   */
  [[nodiscard]] static constexpr double quiescent_power_w(double vin_v, double iq_a) noexcept {
    return vin_v * iq_a;
  }

  /**
   * @brief Total device dissipation.
   * @param[in] vin_v input voltage, in volts.
   * @param[in] vout_v output voltage, in volts.
   * @param[in] iout_a load current, in amperes.
   * @param[in] iq_a quiescent current, in amperes.
   * @return Ppass + Pq, in watts.
   */
  [[nodiscard]] static constexpr double total_power_w(double vin_v, double vout_v, double iout_a, double iq_a) noexcept {
    return pass_power_w(vin_v, vout_v, iout_a) + quiescent_power_w(vin_v, iq_a);
  }

  /**
   * @brief Delivered output power.
   * @param[in] vout_v output voltage, in volts.
   * @param[in] iout_a load current, in amperes.
   * @return VOUT * IOUT, in watts.
   */
  [[nodiscard]] static constexpr double output_power_w(double vout_v, double iout_a) noexcept {
    return vout_v * iout_a;
  }

  /**
   * @brief Drawn input power.
   * @param[in] vin_v input voltage, in volts.
   * @param[in] iout_a load current, in amperes.
   * @param[in] iq_a quiescent current, in amperes.
   * @return VIN * (IOUT + Iq), in watts.
   */
  [[nodiscard]] static constexpr double input_power_w(double vin_v, double iout_a, double iq_a) noexcept {
    return vin_v * input_current_a(iout_a, iq_a);
  }

  /**
   * @brief DC steady-state efficiency.
   * @param[in] vout_v output voltage, in volts.
   * @param[in] vin_v input voltage, in volts.
   * @param[in] iout_a load current, in amperes.
   * @param[in] iq_a quiescent current, in amperes.
   * @return POUT / PIN as a fraction; 1.0 when both powers are zero,
   *         infinity when PIN == 0 < POUT.
   */
  [[nodiscard]] static constexpr double efficiency(double vout_v, double vin_v, double iout_a, double iq_a) noexcept {
    const double pin = input_power_w(vin_v, iout_a, iq_a);
    if (pin == 0.0) {
      const double pout = output_power_w(vout_v, iout_a);
      return pout == 0.0 ? 1.0 : std::numeric_limits<double>::infinity();
    }
    return output_power_w(vout_v, iout_a) / pin;
  }

  /**
   * @brief First-order junction temperature.
   * @param[in] ta_c ambient temperature, in degrees C.
   * @param[in] p_total_w total device dissipation, in watts.
   * @param[in] rth_cpw junction-to-ambient thermal resistance, in C/W.
   * @return TA + Ptotal * RthJA, in degrees C.
   */
  [[nodiscard]] static constexpr double junction_temp_c(double ta_c, double p_total_w, double rth_cpw) noexcept {
    return ta_c + p_total_w * rth_cpw;
  }

  // ------------------------------------------------------------- full solve
  // tjmax_c = +inf (default) means no thermal limit (thermal_ok stays true).
  /**
   * @brief Evaluate the full LDO operating point for one corner.
   *
   * @param[in] vin_v input voltage, in volts.
   * @param[in] vout_v regulated output voltage, in volts.
   * @param[in] iout_a load current, in amperes.
   * @param[in] iq_a quiescent current, in amperes (default 0).
   * @param[in] vdo_v dropout voltage at the load, in volts (default 0).
   * @param[in] ta_c ambient temperature, in degrees C (default 25).
   * @param[in] rth_cpw thermal resistance, in C/W (default 0: no rise).
   * @param[in] tjmax_c junction limit, in degrees C; +inf (default)
   *            means no thermal limit (thermal_ok stays true).
   * @return The complete corner, with dropout_ok, thermal_ok, and the
   *         combined pass verdict.
   */
  [[nodiscard]] static constexpr LdoResult
  solve(double vin_v, double vout_v, double iout_a, double iq_a = 0.0, double vdo_v = 0.0, double ta_c = 25.0, double rth_cpw = 0.0,
        double tjmax_c = std::numeric_limits<double>::infinity()) noexcept {
    LdoResult r{};
    r.headroom_v = headroom_v(vin_v, vout_v);
    r.vin_min_v = vin_min_v(vout_v, vdo_v);
    r.iin_a = input_current_a(iout_a, iq_a);
    r.pout_w = output_power_w(vout_v, iout_a);
    r.pin_w = input_power_w(vin_v, iout_a, iq_a);
    r.p_pass_w = pass_power_w(vin_v, vout_v, iout_a);
    r.p_q_w = quiescent_power_w(vin_v, iq_a);
    r.p_total_w = r.p_pass_w + r.p_q_w;
    r.eff = efficiency(vout_v, vin_v, iout_a, iq_a);
    r.dropout_ok = r.headroom_v >= vdo_v;
    r.tj_c = junction_temp_c(ta_c, r.p_total_w, rth_cpw);
    r.thermal_ok = r.tj_c <= tjmax_c;
    r.pass = r.dropout_ok && r.thermal_ok;
    return r;
  }

  // --------------------------------------------------- dimensionally-typed
  /**
   * @brief Headroom, dimensionally typed.
   * @param[in] vin input potential (SI volts).
   * @param[in] vout output potential (SI volts).
   * @return VIN - VOUT as an electric-potential quantity.
   */
  [[nodiscard]] static constexpr units::Value<units::ElectricPotential>
  headroom(units::Value<units::ElectricPotential> vin, units::Value<units::ElectricPotential> vout) noexcept {
    return vin - vout;
  }

  /**
   * @brief Lowest regulating input, dimensionally typed.
   * @param[in] vout output potential (SI volts).
   * @param[in] vdo dropout potential (SI volts).
   * @return VOUT + Vdo as an electric-potential quantity.
   */
  [[nodiscard]] static constexpr units::Value<units::ElectricPotential>
  vin_min(units::Value<units::ElectricPotential> vout, units::Value<units::ElectricPotential> vdo) noexcept {
    return units::Value<units::ElectricPotential>(vin_min_v(vout.get(), vdo.get()));
  }

  /**
   * @brief Input current, dimensionally typed.
   * @param[in] iout load current (SI amperes).
   * @param[in] iq quiescent current (SI amperes).
   * @return IOUT + Iq as an electric-current quantity.
   */
  [[nodiscard]] static constexpr units::Value<units::ElectricCurrent>
  input_current(units::Value<units::ElectricCurrent> iout, units::Value<units::ElectricCurrent> iq) noexcept {
    return units::Value<units::ElectricCurrent>(input_current_a(iout.get(), iq.get()));
  }

  /**
   * @brief Pass-element dissipation, dimensionally typed.
   * @param[in] vin input potential (SI volts).
   * @param[in] vout output potential (SI volts).
   * @param[in] iout load current (SI amperes).
   * @return (VIN - VOUT) * IOUT as a power quantity (SI watts).
   */
  [[nodiscard]] static constexpr units::Value<units::Power>
  pass_power(units::Value<units::ElectricPotential> vin, units::Value<units::ElectricPotential> vout, units::Value<units::ElectricCurrent> iout) noexcept {
    return units::Value<units::Power>(pass_power_w(vin.get(), vout.get(), iout.get()));
  }

  /**
   * @brief Delivered output power, dimensionally typed.
   * @param[in] vout output potential (SI volts).
   * @param[in] iout load current (SI amperes).
   * @return VOUT * IOUT as a power quantity (SI watts).
   */
  [[nodiscard]] static constexpr units::Value<units::Power>
  output_power(units::Value<units::ElectricPotential> vout, units::Value<units::ElectricCurrent> iout) noexcept {
    return vout * iout;
  }
};

// Exactly representable integer-volt/amp goldens (independent of the CLI).
static_assert(LdoSolver::headroom_v(5.0, 3.0) == 2.0);
static_assert(LdoSolver::vin_min_v(3.0, 1.0) == 4.0);
static_assert(LdoSolver::input_current_a(2.0, 1.0) == 3.0);
static_assert(LdoSolver::pass_power_w(5.0, 3.0, 2.0) == 4.0);
static_assert(LdoSolver::quiescent_power_w(5.0, 2.0) == 10.0);
static_assert(LdoSolver::total_power_w(5.0, 3.0, 2.0, 1.0) == 9.0);
static_assert(LdoSolver::output_power_w(3.0, 2.0) == 6.0);
static_assert(LdoSolver::input_power_w(5.0, 2.0, 1.0) == 15.0);
static_assert(LdoSolver::junction_temp_c(25.0, 2.0, 10.0) == 45.0);

} // namespace wengine::solvers
