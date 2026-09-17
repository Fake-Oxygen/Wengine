#pragma once

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

struct S0FullResult {
  double power_w{0.0};
  double energy_kwh{0.0};
  double energy_j{0.0};
  double freq_hz{0.0};
  double period_s{0.0};
  double pulses{0.0};
  double imp_per_kwh{0.0};
  double j_per_imp{0.0};
  double wh_per_imp{0.0};
};

class S0PulseSolver {
public:
  S0PulseSolver() = delete;

  static constexpr double kJoulesPerKwh = 3600000.0;
  static constexpr double kJoulesPerWh = 3600.0;

  // ------------------------------------------------ meter-constant units
  // C in imp/kWh -> impulses per Wh.
  [[nodiscard]] static constexpr double imp_per_kwh_to_imp_per_wh(double c) noexcept {
    return c / 1000.0;
  }
  // C in imp/kWh -> impulses per joule (== impulses per Ws).
  [[nodiscard]] static constexpr double imp_per_kwh_to_imp_per_j(double c) noexcept {
    return c / kJoulesPerKwh;
  }
  [[nodiscard]] static constexpr double imp_per_kwh_to_imp_per_ws(double c) noexcept {
    return imp_per_kwh_to_imp_per_j(c);
  }
  // Energy carried by a single impulse.
  [[nodiscard]] static constexpr double imp_per_kwh_to_j_per_imp(double c) noexcept {
    return c == 0.0 ? std::numeric_limits<double>::infinity() : kJoulesPerKwh / c;
  }
  [[nodiscard]] static constexpr double imp_per_kwh_to_wh_per_imp(double c) noexcept {
    return c == 0.0 ? std::numeric_limits<double>::infinity() : 1000.0 / c;
  }
  [[nodiscard]] static constexpr double imp_per_kwh_to_kwh_per_imp(double c) noexcept {
    return c == 0.0 ? std::numeric_limits<double>::infinity() : 1.0 / c;
  }

  // ------------------------------------------------ energy/power/time units
  [[nodiscard]] static constexpr double joules_to_wh(double j) noexcept {
    return j / kJoulesPerWh;
  }
  [[nodiscard]] static constexpr double wh_to_joules(double wh) noexcept {
    return wh * kJoulesPerWh;
  }
  [[nodiscard]] static constexpr double joules_to_kwh(double j) noexcept {
    return j / kJoulesPerKwh;
  }
  [[nodiscard]] static constexpr double kwh_to_joules(double kwh) noexcept {
    return kwh * kJoulesPerKwh;
  }
  // 1 Wh/s == 3600 W.
  [[nodiscard]] static constexpr double wh_per_s_to_w(double wh_per_s) noexcept {
    return wh_per_s * kJoulesPerWh;
  }
  [[nodiscard]] static constexpr double w_to_wh_per_s(double w) noexcept {
    return w / kJoulesPerWh;
  }

  // ------------------------------------------------ forward: load -> pulses
  [[nodiscard]] static constexpr double power_w_from_ui(double u_v, double i_a,
                                                        double power_factor = 1.0) noexcept {
    return u_v * i_a * power_factor;
  }

  // Pulse rate [Hz == imp/s] for a constant load.
  [[nodiscard]] static constexpr double frequency_hz(double power_w,
                                                     double imp_per_kwh) noexcept {
    return power_w * imp_per_kwh / kJoulesPerKwh;
  }
  [[nodiscard]] static constexpr double period_s(double power_w,
                                                 double imp_per_kwh) noexcept {
    const double f = frequency_hz(power_w, imp_per_kwh);
    return f == 0.0 ? std::numeric_limits<double>::infinity() : 1.0 / f;
  }
  [[nodiscard]] static constexpr double pulses_from_power_time(double power_w, double time_s,
                                                               double imp_per_kwh) noexcept {
    return power_w * time_s * imp_per_kwh / kJoulesPerKwh;
  }
  [[nodiscard]] static constexpr double pulses_from_energy_kwh(double energy_kwh,
                                                               double imp_per_kwh) noexcept {
    return energy_kwh * imp_per_kwh;
  }
  [[nodiscard]] static constexpr double pulses_from_energy_j(double energy_j,
                                                             double imp_per_kwh) noexcept {
    return energy_j * imp_per_kwh / kJoulesPerKwh;
  }

  // ------------------------------------------------ inverse: pulses -> load
  [[nodiscard]] static constexpr double power_w_from_frequency(double freq_hz,
                                                               double imp_per_kwh) noexcept {
    return imp_per_kwh == 0.0 ? std::numeric_limits<double>::infinity()
                              : freq_hz * kJoulesPerKwh / imp_per_kwh;
  }
  [[nodiscard]] static constexpr double energy_kwh_from_pulses(double pulses,
                                                               double imp_per_kwh) noexcept {
    return imp_per_kwh == 0.0 ? std::numeric_limits<double>::infinity()
                              : pulses / imp_per_kwh;
  }
  [[nodiscard]] static constexpr double energy_j_from_pulses(double pulses,
                                                             double imp_per_kwh) noexcept {
    return kwh_to_joules(energy_kwh_from_pulses(pulses, imp_per_kwh));
  }
  [[nodiscard]] static constexpr double time_s_for_pulses(double pulses, double power_w,
                                                          double imp_per_kwh) noexcept {
    const double f = frequency_hz(power_w, imp_per_kwh);
    return f == 0.0 ? std::numeric_limits<double>::infinity() : pulses / f;
  }

  // ------------------------------------------------ full picture
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

  [[nodiscard]] static constexpr S0FullResult
  solve_full_from_ui(double u_v, double i_a, double power_factor, double time_s,
                     double imp_per_kwh) noexcept {
    return solve_full(power_w_from_ui(u_v, i_a, power_factor), time_s, imp_per_kwh);
  }

  // ------------------------------------------------ dimensionally-typed API
  [[nodiscard]] static constexpr units::Value<units::Frequency>
  frequency(units::Value<units::Power> power, double imp_per_kwh) noexcept {
    return units::Value<units::Frequency>(frequency_hz(power.get(), imp_per_kwh));
  }
  [[nodiscard]] static constexpr units::Value<units::Power>
  power(units::Value<units::Frequency> freq, double imp_per_kwh) noexcept {
    return units::Value<units::Power>(power_w_from_frequency(freq.get(), imp_per_kwh));
  }
  // Energy [J] stored in N pulses.
  [[nodiscard]] static constexpr units::Value<units::Energy>
  energy(units::Value<units::Count> pulses, double imp_per_kwh) noexcept {
    return units::Value<units::Energy>(energy_j_from_pulses(pulses.get(), imp_per_kwh));
  }
  // Pulse count for an energy batch [J].
  [[nodiscard]] static constexpr units::Value<units::Count>
  pulses(units::Value<units::Energy> energy, double imp_per_kwh) noexcept {
    return units::Value<units::Count>(pulses_from_energy_j(energy.get(), imp_per_kwh));
  }
  // Pulse count for a constant load over a time span.
  [[nodiscard]] static constexpr units::Value<units::Count>
  pulses(units::Value<units::Power> power, units::Value<units::Time> time,
         double imp_per_kwh) noexcept {
    return units::Value<units::Count>(
        pulses_from_power_time(power.get(), time.get(), imp_per_kwh));
  }
};

} // namespace wengine::solvers
