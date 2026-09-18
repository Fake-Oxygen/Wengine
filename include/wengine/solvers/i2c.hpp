#pragma once

// I2C-bus resistor solver: pull-up Rp sizing + series Rs checks.
//
// Reference: NXP UM10204 "I2C-bus specification and user manual" Rev. 7,
// plus TI SLVA689 (same derivation, tabulated limits).
//
// Pull-up Rp (UM10204 section 7.1), SI base V / F / s / A / Ohm:
//   Rp(min) = (VDD - VOL(max)) / IOL                      // sink-current limit
//     e.g. (5.5 - 0.4) / 0.003 = 1.7 kOhm (spec example)
//   Rp(max) = tr / (0.8473 * Cb)                          // rise-time limit
//     0.8473 = ln(7/3): RC rise from VIL = 0.3*VDD to VIH = 0.7*VDD
//     (UM10204 section 3.1.2 input levels; TI SLVA689 eq. 5-6)
// Recommended pick: geometric mean sqrt(Rp(min)*Rp(max)), snapped to E12.
//
// Per-mode limits (UM10204 Table 10 timing; VOL/IOL per Table 9 + TI table):
//   Standard (100 kHz): tr = 1000 ns, tf = 300 ns, Cb max = 400 pF
//   Fast (400 kHz):     tr = 300 ns,  tf = 300 ns, Cb max = 400 pF
//   Fast-plus (1 MHz):  tr = 120 ns,  tf = 120 ns, Cb max = 550 pF
//   VOL(max) = 0.4 V at IOL = 3 mA for VDD > 2 V (Standard/Fast);
//   for VDD <= 2 V: VOL = 0.2*VDD at IOL = 2 mA (Fast/Fast-plus rows).
//   Fast-plus drivers are 10x stronger (UM10204: "drive strength by as much
//   as 10x"): IOL = 30 mA here (NXP Fm+ bus training, AMF-IND-T3704).
//   All of VOL/IOL/tr are overridable for custom/legacy analysis.
// High-speed mode is intentionally NOT a preset: UM10204 section 5 uses
// switched current-source pull-ups there, not resistor sizing.
//
// Series Rs (protection/filter resistor between pin and bus):
// UM10204 Table 9 footnote allows series protection resistors as long as
// the fall-time limit is met. Rs is bounded by two budgets:
//   LOW-level margin: VOL(driver) + IOL*Rs <= VIL = 0.3*VDD
//     -> Rs <= (0.3*VDD - VOL) / IOL
//   Rise-time budget: the charge path sees Rp + Rs, sharing Rp(max):
//     -> Rs <= Rp(max) - Rp
// Fall estimate: tf ~= 0.8473 * (Ron + Rs) * Cb with Ron = VOL/IOL,
// checked against the mode tf limit (first-order model: driver as Ron).
//
// All functions are pure, header-only, constexpr + noexcept.
// Input validation (VDD > 0, Cb >= 0, ...) is the caller's job (CLI layer).

#include "wengine/units/units.hpp"

#include <array>
#include <limits>

namespace wengine::solvers {

enum class I2cMode : int { Standard, Fast, FastPlus };

struct I2cModeLimits {
  double tr_max_s{0.0};  // SDA/SCL rise-time limit
  double tf_max_s{0.0};  // SDA/SCL fall-time limit
  double cb_max_f{0.0};  // maximum bus capacitance
  double iol_a{0.0};     // driver LOW-level sink capability
};

[[nodiscard]] constexpr I2cModeLimits i2c_mode_limits(I2cMode mode) noexcept {
  if (mode == I2cMode::FastPlus) {
    return {120e-9, 120e-9, 550e-12, 30e-3};
  }
  if (mode == I2cMode::Fast) {
    return {300e-9, 300e-9, 400e-12, 3e-3};
  }
  return {1000e-9, 300e-9, 400e-12, 3e-3}; // Standard
}

// Default VOL(max): 0.4 V above 2 V, else 0.2*VDD (UM10204 Table 9 style).
[[nodiscard]] constexpr double i2c_default_vol_v(double vdd_v) noexcept {
  return vdd_v > 2.0 ? 0.4 : 0.2 * vdd_v;
}

// Default IOL: mode capability, except 2 mA for VDD <= 2 V (non-Fast-plus).
[[nodiscard]] constexpr double i2c_default_iol_a(double vdd_v, I2cMode mode) noexcept {
  if (mode == I2cMode::FastPlus) {
    return 30e-3;
  }
  if (vdd_v <= 2.0) {
    return 2e-3;
  }
  return 3e-3;
}

struct I2cPullupResult {
  double rp_min_ohm{0.0};
  double rp_max_ohm{0.0};
  bool feasible{false}; // rp_min <= rp_max
  double rp_rec_ohm{0.0}; // geometric mean (0 when infeasible)
  double rp_e12_ohm{0.0}; // nearest E12 to rec (0 when infeasible)
  double rp_used_ohm{0.0}; // evaluated value: explicit choice or E12
  double actual_tr_s{0.0}; // rise time at rp_used
  double static_current_a{0.0}; // (VDD - VOL) / Rp when line held LOW
  double static_power_w{0.0}; // (VDD - VOL)^2 / Rp per line
  bool tr_ok{false}; // actual_tr <= tr limit
  bool iol_ok{false}; // static_current <= IOL
};

struct I2cSeriesResult {
  double rs_max_low_ohm{0.0}; // LOW-level margin bound
  double rs_max_rise_ohm{0.0}; // rise-time budget bound (Rp(max) - Rp)
  double rs_max_ohm{0.0}; // min of both (may be negative -> infeasible)
  bool feasible{false};
  double rs_used_ohm{0.0};
  double vol_at_bus_v{0.0}; // VOL(driver) + IOL*Rs as seen on the bus
  double vil_margin_v{0.0}; // 0.3*VDD - vol_at_bus (>= 0 required)
  double tr_with_rs_s{0.0}; // 0.8473*(Rp + Rs)*Cb
  double tf_est_s{0.0}; // 0.8473*(Ron + Rs)*Cb, Ron = VOL/IOL
  bool low_ok{false};
  bool rise_ok{false};
  bool fall_ok{false};
};

class I2cBusSolver {
public:
  I2cBusSolver() = delete;

  // ln(7/3): RC rise/fall between 0.3*VDD and 0.7*VDD thresholds.
  static constexpr double kRiseFactor = 0.8472978603872034;
  static constexpr double kVilFactor = 0.3; // VIL = 0.3 * VDD (UM10204 3.1.2)

  static constexpr std::array<double, 12> kE12{{
      1.0, 1.2, 1.5, 1.8, 2.2, 2.7, 3.3, 3.9, 4.7, 5.6, 6.8, 8.2,
  }};

  // ------------------------------------------------------------ primitives
  [[nodiscard]] static constexpr double rp_min_ohm(double vdd_v, double vol_v,
                                                   double iol_a) noexcept {
    return iol_a == 0.0 ? std::numeric_limits<double>::infinity()
                        : (vdd_v - vol_v) / iol_a;
  }

  [[nodiscard]] static constexpr double rp_max_ohm(double tr_s, double cb_f) noexcept {
    return cb_f == 0.0 ? std::numeric_limits<double>::infinity()
                       : tr_s / (kRiseFactor * cb_f);
  }

  [[nodiscard]] static constexpr double rise_time_s(double rp_ohm, double cb_f) noexcept {
    return kRiseFactor * rp_ohm * cb_f;
  }

  [[nodiscard]] static constexpr double static_current_a(double vdd_v, double vol_v,
                                                         double rp_ohm) noexcept {
    return rp_ohm == 0.0 ? std::numeric_limits<double>::infinity()
                         : (vdd_v - vol_v) / rp_ohm;
  }

  [[nodiscard]] static constexpr double static_power_w(double vdd_v, double vol_v,
                                                       double rp_ohm) noexcept {
    if (rp_ohm == 0.0) {
      return std::numeric_limits<double>::infinity();
    }
    const double i = (vdd_v - vol_v) / rp_ohm;
    return (vdd_v - vol_v) * i;
  }

  [[nodiscard]] static constexpr double sqrt_newton(double x) noexcept {
    if (!(x > 0.0)) {
      return 0.0;
    }
    double r = x > 1.0 ? x : 1.0;
    for (int k = 0; k < 40; ++k) {
      r = 0.5 * (r + x / r);
    }
    return r;
  }

  [[nodiscard]] static constexpr double geometric_mean(double a, double b) noexcept {
    return sqrt_newton(a * b);
  }

  // Nearest E12 value (0 for non-positive input).
  [[nodiscard]] static constexpr double e12_nearest(double r) noexcept {
    if (!(r > 0.0)) {
      return 0.0;
    }
    double mant = r;
    int dec = 0;
    while (mant >= 10.0) {
      mant /= 10.0;
      ++dec;
    }
    while (mant < 1.0) {
      mant *= 10.0;
      --dec;
    }
    double best = kE12[0];
    double best_ratio = mant >= kE12[0] ? mant / kE12[0] : kE12[0] / mant;
    int best_dec = dec;
    for (std::size_t k = 1; k < kE12.size(); ++k) {
      const double b = kE12[k];
      const double ratio = mant >= b ? mant / b : b / mant;
      if (ratio < best_ratio) {
        best_ratio = ratio;
        best = b;
        best_dec = dec;
      }
    }
    // Next decade's 1.0 (= 10.0 mantissa) competes too, e.g. 966 -> 1k.
    if (10.0 / mant < best_ratio) {
      best = 1.0;
      best_dec = dec + 1;
    }
    double scale = 1.0;
    for (int k = 0; k < best_dec; ++k) {
      scale *= 10.0;
    }
    for (int k = 0; k > best_dec; --k) {
      scale /= 10.0;
    }
    return best * scale;
  }

  // ------------------------------------------------------------- pull-up
  // rp_choice_ohm <= 0 selects the automatic E12 recommendation.
  [[nodiscard]] static constexpr I2cPullupResult
  solve_pullup(double vdd_v, double cb_f, double tr_s, double vol_v, double iol_a,
               double rp_choice_ohm = 0.0) noexcept {
    I2cPullupResult r{};
    r.rp_min_ohm = rp_min_ohm(vdd_v, vol_v, iol_a);
    r.rp_max_ohm = rp_max_ohm(tr_s, cb_f);
    r.feasible = r.rp_min_ohm <= r.rp_max_ohm;
    if (r.feasible) {
      r.rp_rec_ohm = geometric_mean(r.rp_min_ohm, r.rp_max_ohm);
      r.rp_e12_ohm = e12_nearest(r.rp_rec_ohm);
    }
    r.rp_used_ohm = rp_choice_ohm > 0.0 ? rp_choice_ohm : r.rp_e12_ohm;
    if (r.rp_used_ohm > 0.0) {
      r.actual_tr_s = rise_time_s(r.rp_used_ohm, cb_f);
      r.static_current_a = static_current_a(vdd_v, vol_v, r.rp_used_ohm);
      r.static_power_w = static_power_w(vdd_v, vol_v, r.rp_used_ohm);
      r.tr_ok = r.actual_tr_s <= tr_s;
      r.iol_ok = r.static_current_a <= iol_a;
    }
    return r;
  }

  // -------------------------------------------------------------- series
  // rs_choice_ohm < 0 selects the automatic maximum (most protection in spec).
  [[nodiscard]] static constexpr I2cSeriesResult
  solve_series(double vdd_v, double vol_v, double iol_a, double rp_used_ohm, double cb_f,
               double tr_s, double tf_max_s, double rs_choice_ohm = -1.0) noexcept {
    I2cSeriesResult r{};
    const double vil = kVilFactor * vdd_v;
    r.rs_max_low_ohm = iol_a == 0.0 ? std::numeric_limits<double>::infinity()
                                    : (vil - vol_v) / iol_a;
    r.rs_max_rise_ohm = rp_max_ohm(tr_s, cb_f) - rp_used_ohm;
    r.rs_max_ohm =
        r.rs_max_low_ohm < r.rs_max_rise_ohm ? r.rs_max_low_ohm : r.rs_max_rise_ohm;
    r.feasible = r.rs_max_ohm >= 0.0;
    r.rs_used_ohm = rs_choice_ohm >= 0.0 ? rs_choice_ohm : (r.feasible ? r.rs_max_ohm : 0.0);
    r.vol_at_bus_v = vol_v + iol_a * r.rs_used_ohm;
    r.vil_margin_v = vil - r.vol_at_bus_v;
    r.tr_with_rs_s = kRiseFactor * (rp_used_ohm + r.rs_used_ohm) * cb_f;
    const double ron = iol_a == 0.0 ? std::numeric_limits<double>::infinity() : vol_v / iol_a;
    r.tf_est_s = kRiseFactor * (ron + r.rs_used_ohm) * cb_f;
    r.low_ok = r.vil_margin_v >= 0.0;
    r.rise_ok = r.tr_with_rs_s <= tr_s;
    r.fall_ok = r.tf_est_s <= tf_max_s;
    return r;
  }

  // --------------------------------------------------- dimensionally-typed
  [[nodiscard]] static constexpr units::Value<units::ElectricResistance>
  rp_min(units::Value<units::ElectricPotential> vdd, units::Value<units::ElectricPotential> vol,
         units::Value<units::ElectricCurrent> iol) noexcept {
    return iol.get() == 0.0
               ? units::Value<units::ElectricResistance>(
                     std::numeric_limits<double>::infinity())
               : (vdd - vol) / iol;
  }

  [[nodiscard]] static constexpr units::Value<units::ElectricResistance>
  rp_max(units::Value<units::Time> tr, units::Value<units::Capacitance> cb) noexcept {
    return units::Value<units::ElectricResistance>(rp_max_ohm(tr.get(), cb.get()));
  }

  [[nodiscard]] static constexpr units::Value<units::Time>
  rise_time(units::Value<units::ElectricResistance> rp,
            units::Value<units::Capacitance> cb) noexcept {
    return units::Value<units::Time>(rise_time_s(rp.get(), cb.get()));
  }
};

} // namespace wengine::solvers
