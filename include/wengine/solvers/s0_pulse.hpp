#pragma once

/**
 * @file
 * S0 pulse-output solver for DIN 43864 energy meters: converts between
 * constant load and impulse rate for a meter with constant C in
 * imp/kWh. Owns the pure unit-bridge and pulse math only — depends on
 * units (and math), never CLI types; input validation (C > 0, P >= 0,
 * ...) is the CLI layer's job. Consumed by the `s0` CLI command and
 * tests/test_s0_pulse.cpp.
 */

// S0 pulse-output solver (DIN 43864 energy meters).
//
// A meter with constant C [imp/kWh] emits one impulse per 1/C kWh.
// SI base inside the solver: power [W], energy [J == Ws], time [s],
// pulse rate [Hz == imp/s], meter constant [imp/kWh].
//
// Core identities (P in W, E in kWh, t in s, f in Hz, N in imp):
//   f = P * C / 3_600_000
//   P = f * 3_600_000 / C
//   N = E_kWh * C = P * t * C / 3_600_000
//   E_kWh = N / C
// Unit bridges: 1 kWh = 3.6e6 J, 1 Wh = 3600 J, 1 Wh/s = 3600 W,
//   imp/J (== imp/Ws) = C / 3.6e6, J/imp = 3.6e6 / C.
//
// All functions are pure, header-only, constexpr + noexcept.
// Input validation (C > 0, P >= 0, ...) is the caller's job (CLI layer).

#include "wengine/units/units.hpp"

#include <limits>

namespace wengine::solvers {

/** @brief Complete S0 picture for a constant load over a time span (all SI units). */
struct S0FullResult {
  /** @brief Constant load power, in watts. */
  double power_w{0.0};
  /** @brief Energy over the span, in kWh. */
  double energy_kwh{0.0};
  /** @brief Energy over the span, in joules. */
  double energy_j{0.0};
  /** @brief Pulse rate f = P * C / 3.6e6, in hertz (impulses per second). */
  double freq_hz{0.0};
  /** @brief Pulse period 1/f, in seconds (infinity at zero load). */
  double period_s{0.0};
  /** @brief Impulses emitted over the span, dimensionless count. */
  double pulses{0.0};
  /** @brief Echo of the meter constant, in imp/kWh. */
  double imp_per_kwh{0.0};
  /** @brief Energy per impulse, in joules (infinity when C is 0). */
  double j_per_imp{0.0};
  /** @brief Energy per impulse, in Wh (infinity when C is 0). */
  double wh_per_imp{0.0};
};

/**
 * @brief S0 pulse-output closed-form solver (static-only utility).
 *
 * @note Pure, header-only, constexpr + noexcept: every output is one
 *       algebraic identity from the banner (no accumulation error, no
 *       stepping). Infinity sentinels (C == 0, zero load period) are
 *       domain values; callers validate at the CLI layer.
 */
class S0PulseSolver {
public:
  /** @brief Deleted constructor: every solver here is a static function. */
  S0PulseSolver() = delete;

  /** @brief Unit bridge: 1 kWh = 3.6e6 J. */
  static constexpr double kJoulesPerKwh = 3600000.0;
  /** @brief Unit bridge: 1 Wh = 3600 J. */
  static constexpr double kJoulesPerWh = 3600.0;

  // ------------------------------------------------ meter-constant units
  // C in imp/kWh -> impulses per Wh.
  /**
   * @brief Scale a meter constant to impulses per Wh.
   * @param[in] c meter constant, in imp/kWh.
   * @return c / 1000, in imp/Wh.
   */
  [[nodiscard]] static constexpr double imp_per_kwh_to_imp_per_wh(double c) noexcept {
    return c / 1000.0;
  }
  // C in imp/kWh -> impulses per joule (== impulses per Ws).
  /**
   * @brief Scale a meter constant to impulses per joule.
   * @param[in] c meter constant, in imp/kWh.
   * @return c / 3.6e6, in imp/J (== imp/Ws).
   */
  [[nodiscard]] static constexpr double imp_per_kwh_to_imp_per_j(double c) noexcept {
    return c / kJoulesPerKwh;
  }
  /**
   * @brief Scale a meter constant to impulses per watt-second.
   * @param[in] c meter constant, in imp/kWh.
   * @return c / 3.6e6, in imp/Ws (identical to impulses per joule).
   */
  [[nodiscard]] static constexpr double imp_per_kwh_to_imp_per_ws(double c) noexcept {
    return imp_per_kwh_to_imp_per_j(c);
  }
  // Energy carried by a single impulse.
  /**
   * @brief Energy per output impulse.
   * @param[in] c meter constant, in imp/kWh.
   * @return 3.6e6 / c, in joules per impulse; infinity when c is 0.
   */
  [[nodiscard]] static constexpr double imp_per_kwh_to_j_per_imp(double c) noexcept {
    return c == 0.0 ? std::numeric_limits<double>::infinity() : kJoulesPerKwh / c;
  }
  /**
   * @brief Energy per output impulse, in Wh.
   * @param[in] c meter constant, in imp/kWh.
   * @return 1000 / c, in Wh per impulse; infinity when c is 0.
   */
  [[nodiscard]] static constexpr double imp_per_kwh_to_wh_per_imp(double c) noexcept {
    return c == 0.0 ? std::numeric_limits<double>::infinity() : 1000.0 / c;
  }
  /**
   * @brief Energy per output impulse, in kWh.
   * @param[in] c meter constant, in imp/kWh.
   * @return 1 / c, in kWh per impulse; infinity when c is 0.
   */
  [[nodiscard]] static constexpr double imp_per_kwh_to_kwh_per_imp(double c) noexcept {
    return c == 0.0 ? std::numeric_limits<double>::infinity() : 1.0 / c;
  }

  // ------------------------------------------------ energy/power/time units
  /**
   * @brief Convert joules to watt-hours.
   * @param[in] j energy, in joules.
   * @return j / 3600, in Wh.
   */
  [[nodiscard]] static constexpr double joules_to_wh(double j) noexcept {
    return j / kJoulesPerWh;
  }
  /**
   * @brief Convert watt-hours to joules.
   * @param[in] wh energy, in watt-hours.
   * @return wh * 3600, in joules.
   */
  [[nodiscard]] static constexpr double wh_to_joules(double wh) noexcept {
    return wh * kJoulesPerWh;
  }
  /**
   * @brief Convert joules to kilowatt-hours.
   * @param[in] j energy, in joules.
   * @return j / 3.6e6, in kWh.
   */
  [[nodiscard]] static constexpr double joules_to_kwh(double j) noexcept {
    return j / kJoulesPerKwh;
  }
  /**
   * @brief Convert kilowatt-hours to joules.
   * @param[in] kwh energy, in kilowatt-hours.
   * @return kwh * 3.6e6, in joules.
   */
  [[nodiscard]] static constexpr double kwh_to_joules(double kwh) noexcept {
    return kwh * kJoulesPerKwh;
  }
  // 1 Wh/s == 3600 W.
  /**
   * @brief Convert a Wh/s rate to watts.
   * @param[in] wh_per_s rate, in watt-hours per second.
   * @return wh_per_s * 3600, in watts.
   */
  [[nodiscard]] static constexpr double wh_per_s_to_w(double wh_per_s) noexcept {
    return wh_per_s * kJoulesPerWh;
  }
  /**
   * @brief Convert watts to a Wh/s rate.
   * @param[in] w power, in watts.
   * @return w / 3600, in watt-hours per second.
   */
  [[nodiscard]] static constexpr double w_to_wh_per_s(double w) noexcept {
    return w / kJoulesPerWh;
  }

  // ------------------------------------------------ forward: load -> pulses
  /**
   * @brief Real load power from a metered UI pair.
   * @param[in] u_v measured voltage, in volts.
   * @param[in] i_a measured current, in amperes.
   * @param[in] power_factor dimensionless ratio (default 1.0).
   * @return U * I * PF, in watts.
   */
  [[nodiscard]] static constexpr double power_w_from_ui(double u_v, double i_a,
                                                        double power_factor = 1.0) noexcept {
    return u_v * i_a * power_factor;
  }

  // Pulse rate [Hz == imp/s] for a constant load.
  /**
   * @brief Pulse rate for a constant load.
   * @param[in] power_w load power, in watts.
   * @param[in] imp_per_kwh meter constant, in imp/kWh.
   * @return f = P * C / 3.6e6, in hertz (impulses per second).
   */
  [[nodiscard]] static constexpr double frequency_hz(double power_w,
                                                     double imp_per_kwh) noexcept {
    return power_w * imp_per_kwh / kJoulesPerKwh;
  }
  /**
   * @brief Pulse period for a constant load.
   * @param[in] power_w load power, in watts.
   * @param[in] imp_per_kwh meter constant, in imp/kWh.
   * @return 1 / f, in seconds; infinity at zero load.
   */
  [[nodiscard]] static constexpr double period_s(double power_w,
                                                 double imp_per_kwh) noexcept {
    const double f = frequency_hz(power_w, imp_per_kwh);
    return f == 0.0 ? std::numeric_limits<double>::infinity() : 1.0 / f;
  }
  /**
   * @brief Impulses for a constant load over a time span.
   * @param[in] power_w load power, in watts.
   * @param[in] time_s elapsed time, in seconds.
   * @param[in] imp_per_kwh meter constant, in imp/kWh.
   * @return P * t * C / 3.6e6, dimensionless impulse count.
   */
  [[nodiscard]] static constexpr double pulses_from_power_time(double power_w, double time_s,
                                                               double imp_per_kwh) noexcept {
    return power_w * time_s * imp_per_kwh / kJoulesPerKwh;
  }
  /**
   * @brief Impulses for an energy in kWh.
   * @param[in] energy_kwh energy, in kilowatt-hours.
   * @param[in] imp_per_kwh meter constant, in imp/kWh.
   * @return E * C, dimensionless impulse count.
   */
  [[nodiscard]] static constexpr double pulses_from_energy_kwh(double energy_kwh,
                                                               double imp_per_kwh) noexcept {
    return energy_kwh * imp_per_kwh;
  }
  /**
   * @brief Impulses for an energy in joules.
   * @param[in] energy_j energy, in joules.
   * @param[in] imp_per_kwh meter constant, in imp/kWh.
   * @return E * C / 3.6e6, dimensionless impulse count.
   */
  [[nodiscard]] static constexpr double pulses_from_energy_j(double energy_j,
                                                             double imp_per_kwh) noexcept {
    return energy_j * imp_per_kwh / kJoulesPerKwh;
  }

  // ------------------------------------------------ inverse: pulses -> load
  /**
   * @brief Load power from an observed pulse rate.
   * @param[in] freq_hz observed pulse rate, in hertz.
   * @param[in] imp_per_kwh meter constant, in imp/kWh.
   * @return f * 3.6e6 / C, in watts; infinity when C is 0.
   */
  [[nodiscard]] static constexpr double power_w_from_frequency(double freq_hz,
                                                               double imp_per_kwh) noexcept {
    return imp_per_kwh == 0.0 ? std::numeric_limits<double>::infinity()
                              : freq_hz * kJoulesPerKwh / imp_per_kwh;
  }
  /**
   * @brief Energy in kWh from an impulse count.
   * @param[in] pulses observed impulses, dimensionless count.
   * @param[in] imp_per_kwh meter constant, in imp/kWh.
   * @return N / C, in kWh; infinity when C is 0.
   */
  [[nodiscard]] static constexpr double energy_kwh_from_pulses(double pulses,
                                                               double imp_per_kwh) noexcept {
    return imp_per_kwh == 0.0 ? std::numeric_limits<double>::infinity()
                              : pulses / imp_per_kwh;
  }
  /**
   * @brief Energy in joules from an impulse count.
   * @param[in] pulses observed impulses, dimensionless count.
   * @param[in] imp_per_kwh meter constant, in imp/kWh.
   * @return (N / C) * 3.6e6, in joules; infinity when C is 0.
   */
  [[nodiscard]] static constexpr double energy_j_from_pulses(double pulses,
                                                             double imp_per_kwh) noexcept {
    return kwh_to_joules(energy_kwh_from_pulses(pulses, imp_per_kwh));
  }
  /**
   * @brief Time needed to emit a given impulse count.
   * @param[in] pulses wanted impulses, dimensionless count.
   * @param[in] power_w constant load power, in watts.
   * @param[in] imp_per_kwh meter constant, in imp/kWh.
   * @return N / f, in seconds; infinity at zero load.
   */
  [[nodiscard]] static constexpr double time_s_for_pulses(double pulses, double power_w,
                                                          double imp_per_kwh) noexcept {
    const double f = frequency_hz(power_w, imp_per_kwh);
    return f == 0.0 ? std::numeric_limits<double>::infinity() : pulses / f;
  }

  // ------------------------------------------------ full picture
  /**
   * @brief Full S0 picture for a constant load over a time span.
   * @param[in] power_w load power, in watts.
   * @param[in] time_s elapsed time, in seconds.
   * @param[in] imp_per_kwh meter constant, in imp/kWh.
   * @return Power, energy (J and kWh), pulse rate and period, impulse
   *         count, and the per-impulse energy scales.
   */
  [[nodiscard]] static constexpr S0FullResult solve_full(double power_w, double time_s,
                                                         double imp_per_kwh) noexcept {
    const double energy_j = power_w * time_s;
    S0FullResult r{};
    r.power_w = power_w;
    r.energy_j = energy_j;
    r.energy_kwh = joules_to_kwh(energy_j);
    r.freq_hz = frequency_hz(power_w, imp_per_kwh);
    r.period_s = r.freq_hz == 0.0 ? std::numeric_limits<double>::infinity() : 1.0 / r.freq_hz;
    r.pulses = pulses_from_power_time(power_w, time_s, imp_per_kwh);
    r.imp_per_kwh = imp_per_kwh;
    r.j_per_imp = imp_per_kwh_to_j_per_imp(imp_per_kwh);
    r.wh_per_imp = imp_per_kwh_to_wh_per_imp(imp_per_kwh);
    return r;
  }

  /**
   * @brief Full S0 picture from a metered UI pair.
   * @param[in] u_v measured voltage, in volts.
   * @param[in] i_a measured current, in amperes.
   * @param[in] power_factor dimensionless ratio.
   * @param[in] time_s elapsed time, in seconds.
   * @param[in] imp_per_kwh meter constant, in imp/kWh.
   * @return The full picture for power U*I*PF over the span.
   */
  [[nodiscard]] static constexpr S0FullResult
  solve_full_from_ui(double u_v, double i_a, double power_factor, double time_s,
                     double imp_per_kwh) noexcept {
    return solve_full(power_w_from_ui(u_v, i_a, power_factor), time_s, imp_per_kwh);
  }

  // ------------------------------------------------ dimensionally-typed API
  /**
   * @brief Pulse rate, dimensionally typed.
   * @param[in] power load power (SI watts).
   * @param[in] imp_per_kwh meter constant, in imp/kWh.
   * @return Pulse rate as a frequency quantity (SI hertz).
   */
  [[nodiscard]] static constexpr units::Value<units::Frequency>
  frequency(units::Value<units::Power> power, double imp_per_kwh) noexcept {
    return units::Value<units::Frequency>(frequency_hz(power.get(), imp_per_kwh));
  }
  /**
   * @brief Load power, dimensionally typed.
   * @param[in] freq observed pulse rate (SI hertz).
   * @param[in] imp_per_kwh meter constant, in imp/kWh.
   * @return Load power as a power quantity (SI watts); infinite when
   *         the meter constant is 0.
   */
  [[nodiscard]] static constexpr units::Value<units::Power>
  power(units::Value<units::Frequency> freq, double imp_per_kwh) noexcept {
    return units::Value<units::Power>(power_w_from_frequency(freq.get(), imp_per_kwh));
  }
  // Energy [J] stored in N pulses.
  /**
   * @brief Energy stored in an impulse count, dimensionally typed.
   * @param[in] pulses observed impulses (SI count).
   * @param[in] imp_per_kwh meter constant, in imp/kWh.
   * @return Energy as an energy quantity (SI joules); infinite when
   *         the meter constant is 0.
   */
  [[nodiscard]] static constexpr units::Value<units::Energy>
  energy(units::Value<units::Count> pulses, double imp_per_kwh) noexcept {
    return units::Value<units::Energy>(energy_j_from_pulses(pulses.get(), imp_per_kwh));
  }
  // Pulse count for an energy batch [J].
  /**
   * @brief Impulses for an energy batch, dimensionally typed.
   * @param[in] energy energy batch (SI joules).
   * @param[in] imp_per_kwh meter constant, in imp/kWh.
   * @return Impulse count as a count quantity.
   */
  [[nodiscard]] static constexpr units::Value<units::Count>
  pulses(units::Value<units::Energy> energy, double imp_per_kwh) noexcept {
    return units::Value<units::Count>(pulses_from_energy_j(energy.get(), imp_per_kwh));
  }
  // Pulse count for a constant load over a time span.
  /**
   * @brief Impulses for a constant load over a span, dimensionally typed.
   * @param[in] power load power (SI watts).
   * @param[in] time elapsed time (SI seconds).
   * @param[in] imp_per_kwh meter constant, in imp/kWh.
   * @return Impulse count as a count quantity.
   */
  [[nodiscard]] static constexpr units::Value<units::Count>
  pulses(units::Value<units::Power> power, units::Value<units::Time> time,
         double imp_per_kwh) noexcept {
    return units::Value<units::Count>(
        pulses_from_power_time(power.get(), time.get(), imp_per_kwh));
  }
};

} // namespace wengine::solvers
