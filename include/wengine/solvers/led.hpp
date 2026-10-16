#pragma once

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

struct LedResult {
  double headroom_v{0.0};
  double rs_ohm{0.0}; // exact value (<= 0 when infeasible)
  double rs_e12_ohm{0.0}; // nearest E12 to exact (0 when infeasible)
  double rs_used_ohm{0.0}; // evaluated value: explicit choice or E12
  double if_target_a{0.0};
  double if_actual_a{0.0}; // at rs_used
  double p_res_w{0.0}; // at actual current
  double p_led_w{0.0};
  double p_total_w{0.0};
  double eff{0.0}; // fraction (1.0 when Ptot == Pled == 0, inf when Ptot == 0 < Pled)
  bool feasible{false}; // headroom > 0
  bool rating_ok{true}; // p_res <= prating (+inf rating means no limit)
  bool pass{false}; // feasible && rating_ok
};

class LedSolver {
public:
  LedSolver() = delete;

  // ------------------------------------------------------------ primitives
  [[nodiscard]] static constexpr double headroom_v(double vs_v, double vf_v) noexcept {
    return vs_v - vf_v;
  }

  [[nodiscard]] static constexpr double series_resistance_ohm(double vs_v, double vf_v, double if_a) noexcept {
    return if_a == 0.0 ? std::numeric_limits<double>::infinity() : (vs_v - vf_v) / if_a;
  }

  [[nodiscard]] static constexpr double actual_current_a(double vs_v, double vf_v, double rs_ohm) noexcept {
    return rs_ohm == 0.0 ? std::numeric_limits<double>::infinity() : (vs_v - vf_v) / rs_ohm;
  }

  [[nodiscard]] static constexpr double resistor_power_w(double vs_v, double vf_v, double if_a) noexcept {
    return (vs_v - vf_v) * if_a;
  }

  [[nodiscard]] static constexpr double led_power_w(double vf_v, double if_a) noexcept {
    return vf_v * if_a;
  }

  [[nodiscard]] static constexpr double total_power_w(double vs_v, double if_a) noexcept {
    return vs_v * if_a;
  }

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
  [[nodiscard]] static constexpr units::Value<units::ElectricPotential>
  headroom(units::Value<units::ElectricPotential> vs, units::Value<units::ElectricPotential> vf) noexcept {
    return vs - vf;
  }

  [[nodiscard]] static constexpr units::Value<units::ElectricResistance>
  series_resistance(units::Value<units::ElectricPotential> vs, units::Value<units::ElectricPotential> vf, units::Value<units::ElectricCurrent> i) noexcept {
    return units::Value<units::ElectricResistance>(series_resistance_ohm(vs.get(), vf.get(), i.get()));
  }

  [[nodiscard]] static constexpr units::Value<units::ElectricCurrent>
  actual_current(units::Value<units::ElectricPotential> vs, units::Value<units::ElectricPotential> vf, units::Value<units::ElectricResistance> rs) noexcept {
    return units::Value<units::ElectricCurrent>(actual_current_a(vs.get(), vf.get(), rs.get()));
  }

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
