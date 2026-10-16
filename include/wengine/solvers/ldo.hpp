#pragma once

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

struct LdoResult {
  double headroom_v{0.0};
  double vin_min_v{0.0};
  double iin_a{0.0};
  double pout_w{0.0};
  double pin_w{0.0};
  double p_pass_w{0.0};
  double p_q_w{0.0};
  double p_total_w{0.0};
  double eff{0.0}; // fraction 0..1 (1.0 when PIN == POUT == 0, inf when PIN == 0 < POUT)
  bool dropout_ok{false}; // headroom >= Vdo
  double tj_c{0.0};
  bool thermal_ok{true}; // tj <= tjmax (tjmax = +inf means no limit)
  bool pass{false}; // dropout_ok && thermal_ok
};

class LdoSolver {
public:
  LdoSolver() = delete;

  // ------------------------------------------------------------ primitives
  [[nodiscard]] static constexpr double headroom_v(double vin_v, double vout_v) noexcept {
    return vin_v - vout_v;
  }

  [[nodiscard]] static constexpr double vin_min_v(double vout_v, double vdo_v) noexcept {
    return vout_v + vdo_v;
  }

  [[nodiscard]] static constexpr double input_current_a(double iout_a, double iq_a) noexcept {
    return iout_a + iq_a;
  }

  [[nodiscard]] static constexpr double pass_power_w(double vin_v, double vout_v, double iout_a) noexcept {
    return (vin_v - vout_v) * iout_a;
  }

  [[nodiscard]] static constexpr double quiescent_power_w(double vin_v, double iq_a) noexcept {
    return vin_v * iq_a;
  }

  [[nodiscard]] static constexpr double total_power_w(double vin_v, double vout_v, double iout_a, double iq_a) noexcept {
    return pass_power_w(vin_v, vout_v, iout_a) + quiescent_power_w(vin_v, iq_a);
  }

  [[nodiscard]] static constexpr double output_power_w(double vout_v, double iout_a) noexcept {
    return vout_v * iout_a;
  }

  [[nodiscard]] static constexpr double input_power_w(double vin_v, double iout_a, double iq_a) noexcept {
    return vin_v * input_current_a(iout_a, iq_a);
  }

  [[nodiscard]] static constexpr double efficiency(double vout_v, double vin_v, double iout_a, double iq_a) noexcept {
    const double pin = input_power_w(vin_v, iout_a, iq_a);
    if (pin == 0.0) {
      const double pout = output_power_w(vout_v, iout_a);
      return pout == 0.0 ? 1.0 : std::numeric_limits<double>::infinity();
    }
    return output_power_w(vout_v, iout_a) / pin;
  }

  [[nodiscard]] static constexpr double junction_temp_c(double ta_c, double p_total_w, double rth_cpw) noexcept {
    return ta_c + p_total_w * rth_cpw;
  }

  // ------------------------------------------------------------- full solve
  // tjmax_c = +inf (default) means no thermal limit (thermal_ok stays true).
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
  [[nodiscard]] static constexpr units::Value<units::ElectricPotential>
  headroom(units::Value<units::ElectricPotential> vin, units::Value<units::ElectricPotential> vout) noexcept {
    return vin - vout;
  }

  [[nodiscard]] static constexpr units::Value<units::ElectricPotential>
  vin_min(units::Value<units::ElectricPotential> vout, units::Value<units::ElectricPotential> vdo) noexcept {
    return units::Value<units::ElectricPotential>(vin_min_v(vout.get(), vdo.get()));
  }

  [[nodiscard]] static constexpr units::Value<units::ElectricCurrent>
  input_current(units::Value<units::ElectricCurrent> iout, units::Value<units::ElectricCurrent> iq) noexcept {
    return units::Value<units::ElectricCurrent>(input_current_a(iout.get(), iq.get()));
  }

  [[nodiscard]] static constexpr units::Value<units::Power>
  pass_power(units::Value<units::ElectricPotential> vin, units::Value<units::ElectricPotential> vout, units::Value<units::ElectricCurrent> iout) noexcept {
    return units::Value<units::Power>(pass_power_w(vin.get(), vout.get(), iout.get()));
  }

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
