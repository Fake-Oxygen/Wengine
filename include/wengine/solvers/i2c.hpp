#pragma once

/**
 * @file
 * I2C-bus resistor solver: pull-up Rp sizing plus series Rs checks,
 * closed-form and constexpr. Owns the pure sizing math and the E12
 * snap; depends on units (and math) only — never CLI types; input
 * validation (VDD > 0, Cb >= 0, ...) is the CLI layer's job per the
 * solver error convention. Reference: NXP UM10204 "I2C-bus
 * specification and user manual" Rev. 7 (details preserved in the
 * banner below). Consumed by the `i2c` CLI command and tests/test_i2c.cpp.
 */

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

/**
 * @brief I2C speed mode preset selecting the UM10204 Table 10 limits.
 *
 * Standard = 100 kHz (tr 1000 ns, Cb max 400 pF), Fast = 400 kHz
 * (tr 300 ns, Cb max 400 pF), FastPlus = 1 MHz (tr 120 ns, Cb max
 * 550 pF, 10x drive strength). High-speed mode is intentionally not a
 * preset (UM10204 section 5 uses switched current-source pull-ups).
 */
enum class I2cMode : int { Standard, Fast, FastPlus };

/** @brief Per-mode timing and drive limits (UM10204 Tables 9-10 rows). */
struct I2cModeLimits {
  double tr_max_s{0.0};  /**< @brief SDA/SCL rise-time limit, in seconds. */
  double tf_max_s{0.0};  /**< @brief SDA/SCL fall-time limit, in seconds. */
  double cb_max_f{0.0};  /**< @brief Maximum bus capacitance, in farads. */
  double iol_a{0.0};     /**< @brief Driver LOW-level sink capability, in amperes. */
};

/**
 * @brief Look up the timing and drive limits for one speed mode.
 *
 * @param[in] mode speed preset (Standard / Fast / FastPlus).
 * @return UM10204 Table 10 / Table 9 limits for the mode: rise/fall
 *         times in seconds, bus capacitance ceiling in farads, and
 *         LOW-level sink capability in amperes.
 */
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
/**
 * @brief Default VOL(max) for the supply rail.
 *
 * @param[in] vdd_v supply voltage, in volts.
 * @return 0.4 V when VDD > 2 V, else 0.2*VDD (UM10204 Table 9 style).
 */
[[nodiscard]] constexpr double i2c_default_vol_v(double vdd_v) noexcept {
  return vdd_v > 2.0 ? 0.4 : 0.2 * vdd_v;
}

// Default IOL: mode capability, except 2 mA for VDD <= 2 V (non-Fast-plus).
/**
 * @brief Default driver sink current for the mode and supply.
 *
 * @param[in] vdd_v supply voltage, in volts.
 * @param[in] mode speed preset.
 * @return Fast-plus 30 mA capability; 2 mA for VDD <= 2 V on other
 *         modes; 3 mA otherwise.
 */
[[nodiscard]] constexpr double i2c_default_iol_a(double vdd_v, I2cMode mode) noexcept {
  if (mode == I2cMode::FastPlus) {
    return 30e-3;
  }
  if (vdd_v <= 2.0) {
    return 2e-3;
  }
  return 3e-3;
}

/** @brief Pull-up sizing result for one bus corner (all SI units). */
struct I2cPullupResult {
  /** @brief Sink-current lower bound (VDD - VOL) / IOL, in ohms. */
  double rp_min_ohm{0.0};
  /** @brief Rise-time upper bound tr / (0.8473 * Cb), in ohms. */
  double rp_max_ohm{0.0};
  bool feasible{false}; /**< @brief rp_min <= rp_max. */
  double rp_rec_ohm{0.0}; /**< @brief Geometric mean recommendation (0 when infeasible), in ohms. */
  double rp_e12_ohm{0.0}; /**< @brief Nearest E12 to rec (0 when infeasible), in ohms. */
  double rp_used_ohm{0.0}; /**< @brief Evaluated value: explicit choice or E12, in ohms. */
  double actual_tr_s{0.0}; /**< @brief Rise time at rp_used, in seconds. */
  double static_current_a{0.0}; /**< @brief (VDD - VOL) / Rp when the line is held LOW, in amperes. */
  double static_power_w{0.0}; /**< @brief (VDD - VOL)^2 / Rp per line, in watts. */
  bool tr_ok{false}; /**< @brief actual_tr <= tr limit. */
  bool iol_ok{false}; /**< @brief static_current <= IOL. */
};

/** @brief Series-resistor (Rs) check result for one chosen Rs (all SI units). */
struct I2cSeriesResult {
  double rs_max_low_ohm{0.0}; /**< @brief LOW-level margin bound, in ohms. */
  double rs_max_rise_ohm{0.0}; /**< @brief Rise-time budget bound (Rp(max) - Rp), in ohms. */
  double rs_max_ohm{0.0}; /**< @brief Min of both bounds — may be negative (infeasible), in ohms. */
  /** @brief True when rs_max is non-negative. */
  bool feasible{false};
  /** @brief Evaluated value: explicit choice or the automatic maximum, in ohms. */
  double rs_used_ohm{0.0};
  double vol_at_bus_v{0.0}; /**< @brief VOL(driver) + IOL*Rs as seen on the bus, in volts. */
  double vil_margin_v{0.0}; /**< @brief 0.3*VDD - vol_at_bus (>= 0 required), in volts. */
  double tr_with_rs_s{0.0}; /**< @brief Rise time 0.8473*(Rp + Rs)*Cb, in seconds. */
  double tf_est_s{0.0}; /**< @brief Fall estimate 0.8473*(Ron + Rs)*Cb with Ron = VOL/IOL, in seconds. */
  /** @brief LOW level meets VIL at the bus. */
  bool low_ok{false};
  /** @brief Rise time with Rs meets the tr limit. */
  bool rise_ok{false};
  /** @brief Fall estimate meets the tf limit. */
  bool fall_ok{false};
};

/**
 * @brief I2C-bus resistor solver (static-only closed-form utility).
 *
 * @note Pure, header-only, constexpr + noexcept: every result is one
 *       closed-form expression (UM10204 equations in the banner above)
 *       — exactly reproducible, no accumulation error. Infinity sentinels
 *       (iol_a == 0, cb_f == 0) are domain values; callers validate at
 *       the CLI layer.
 */
class I2cBusSolver {
public:
  /** @brief Deleted constructor: every solver here is a static function. */
  I2cBusSolver() = delete;

  // ln(7/3): RC rise/fall between 0.3*VDD and 0.7*VDD thresholds.
  /** @brief ln(7/3) = 0.8473: RC rise/fall factor between 0.3*VDD and 0.7*VDD thresholds. */
  static constexpr double kRiseFactor = 0.8472978603872034;
  static constexpr double kVilFactor = 0.3; /**< @brief VIL = 0.3 * VDD (UM10204 3.1.2). */

  /** @brief E12 preferred series mantissas used for the resistor snap. */
  static constexpr std::array<double, 12> kE12{{
      1.0, 1.2, 1.5, 1.8, 2.2, 2.7, 3.3, 3.9, 4.7, 5.6, 6.8, 8.2,
  }};

  // ------------------------------------------------------------ primitives
  /**
   * @brief Rp lower bound from the sink-current limit (UM10204 7.1).
   * @param[in] vdd_v supply voltage, in volts.
   * @param[in] vol_v driver LOW-level output voltage, in volts.
   * @param[in] iol_a LOW-level sink capability, in amperes.
   * @return Rp(min) = (VDD - VOL) / IOL, in ohms; infinity when IOL is 0.
   */
  [[nodiscard]] static constexpr double rp_min_ohm(double vdd_v, double vol_v,
                                                   double iol_a) noexcept {
    return iol_a == 0.0 ? std::numeric_limits<double>::infinity()
                        : (vdd_v - vol_v) / iol_a;
  }

  /**
   * @brief Rp upper bound from the rise-time limit (UM10204 7.1).
   * @param[in] tr_s allowed rise time, in seconds.
   * @param[in] cb_f total bus capacitance, in farads.
   * @return Rp(max) = tr / (0.8473 * Cb), in ohms; infinity when Cb is 0.
   */
  [[nodiscard]] static constexpr double rp_max_ohm(double tr_s, double cb_f) noexcept {
    return cb_f == 0.0 ? std::numeric_limits<double>::infinity()
                       : tr_s / (kRiseFactor * cb_f);
  }

  /**
   * @brief Rise time for a given pull-up and bus capacitance.
   * @param[in] rp_ohm pull-up resistance, in ohms.
   * @param[in] cb_f total bus capacitance, in farads.
   * @return tr = 0.8473 * Rp * Cb, in seconds.
   */
  [[nodiscard]] static constexpr double rise_time_s(double rp_ohm, double cb_f) noexcept {
    return kRiseFactor * rp_ohm * cb_f;
  }

  /**
   * @brief Static current while any master holds the line LOW.
   * @param[in] vdd_v supply voltage, in volts.
   * @param[in] vol_v LOW-level output voltage, in volts.
   * @param[in] rp_ohm pull-up resistance, in ohms.
   * @return (VDD - VOL) / Rp, in amperes; infinity when Rp is 0.
   */
  [[nodiscard]] static constexpr double static_current_a(double vdd_v, double vol_v,
                                                         double rp_ohm) noexcept {
    return rp_ohm == 0.0 ? std::numeric_limits<double>::infinity()
                         : (vdd_v - vol_v) / rp_ohm;
  }

  /**
   * @brief Static dissipation while the line is held LOW.
   * @param[in] vdd_v supply voltage, in volts.
   * @param[in] vol_v LOW-level output voltage, in volts.
   * @param[in] rp_ohm pull-up resistance, in ohms.
   * @return (VDD - VOL)^2 / Rp per line, in watts; infinity when Rp is 0.
   */
  [[nodiscard]] static constexpr double static_power_w(double vdd_v, double vol_v,
                                                       double rp_ohm) noexcept {
    if (rp_ohm == 0.0) {
      return std::numeric_limits<double>::infinity();
    }
    const double i = (vdd_v - vol_v) / rp_ohm;
    return (vdd_v - vol_v) * i;
  }

  /**
   * @brief Newton square root (constexpr, libm-free).
   * @param[in] x value to root.
   * @return sqrt(x) for x > 0 (40 iterations); 0.0 for non-positive x.
   */
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

  /**
   * @brief Geometric mean sqrt(a*b) via sqrt_newton.
   * @param[in] a first factor.
   * @param[in] b second factor.
   * @return sqrt(a*b); 0.0 when either factor is non-positive.
   */
  [[nodiscard]] static constexpr double geometric_mean(double a, double b) noexcept {
    return sqrt_newton(a * b);
  }

  // Nearest E12 value (0 for non-positive input).
  /**
   * @brief Snap a resistance to the nearest E12 preferred value.
   *
   * @param[in] r exact resistance, in ohms (any decade).
   * @return The E12 value minimizing the ratio distance to r; 0.0 for
   *         non-positive input.
   * @note The next decade's 1.0 (= 10.0 mantissa) competes with the
   *       in-decade values, so 966 ohms snaps to 1k, not 820 (AGENTS.md
   *       known lesson — the next-decade rule is mandatory here).
   */
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
  /**
   * @brief Size the bus pull-up for one corner.
   *
   * Computes Rp(min) from the sink-current limit, Rp(max) from the
   * rise-time limit, recommends the geometric mean snapped to E12, then
   * evaluates rise time, static current, and static power at the used
   * value with two pass/fail checks.
   *
   * @param[in] vdd_v supply voltage, in volts.
   * @param[in] cb_f total bus capacitance, in farads.
   * @param[in] tr_s allowed rise time for the mode, in seconds.
   * @param[in] vol_v LOW-level output voltage, in volts.
   * @param[in] iol_a LOW-level sink capability, in amperes.
   * @param[in] rp_choice_ohm explicit resistor at hand, in ohms; <= 0
   *            selects the automatic E12 recommendation.
   * @return Fully evaluated corner: bounds, recommendation, used value,
   *         actual timing/current/power, and tr_ok / iol_ok verdicts.
   */
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
  /**
   * @brief Size the series (protection) resistor for one corner.
   *
   * Rs is bounded by the LOW-level margin (VOL + IOL*Rs <= 0.3*VDD) and
   * by the shared rise-time budget (Rs <= Rp(max) - Rp), then evaluated
   * at the used value with three pass/fail checks (LOW level, rise time,
   * first-order fall estimate).
   *
   * @param[in] vdd_v supply voltage, in volts.
   * @param[in] vol_v driver LOW-level output voltage, in volts.
   * @param[in] iol_a LOW-level sink capability, in amperes.
   * @param[in] rp_used_ohm the pull-up in use, in ohms.
   * @param[in] cb_f total bus capacitance, in farads.
   * @param[in] tr_s allowed rise time, in seconds.
   * @param[in] tf_max_s allowed fall time for the mode, in seconds.
   * @param[in] rs_choice_ohm explicit resistor at hand, in ohms; < 0
   *            selects the automatic maximum (most protection in spec).
   * @return Fully evaluated corner: both bounds, the tighter rs_max,
   *         used value, bus-visible VOL, VIL margin, timing estimates,
   *         and low_ok / rise_ok / fall_ok verdicts.
   */
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
  /**
   * @brief Rp lower bound, dimensionally typed (sink-current limit).
   * @param[in] vdd supply potential (SI volts).
   * @param[in] vol LOW-level output potential (SI volts).
   * @param[in] iol LOW-level sink current (SI amperes).
   * @return (VDD - VOL) / IOL as an electric-resistance quantity; an
   *         infinite resistance when IOL is zero.
   */
  [[nodiscard]] static constexpr units::Value<units::ElectricResistance>
  rp_min(units::Value<units::ElectricPotential> vdd, units::Value<units::ElectricPotential> vol,
         units::Value<units::ElectricCurrent> iol) noexcept {
    return iol.get() == 0.0
               ? units::Value<units::ElectricResistance>(
                     std::numeric_limits<double>::infinity())
               : (vdd - vol) / iol;
  }

  /**
   * @brief Rp upper bound, dimensionally typed (rise-time limit).
   * @param[in] tr allowed rise time (SI seconds).
   * @param[in] cb total bus capacitance (SI farads).
   * @return tr / (0.8473 * Cb) as an electric-resistance quantity; an
   *         infinite resistance when Cb is zero.
   */
  [[nodiscard]] static constexpr units::Value<units::ElectricResistance>
  rp_max(units::Value<units::Time> tr, units::Value<units::Capacitance> cb) noexcept {
    return units::Value<units::ElectricResistance>(rp_max_ohm(tr.get(), cb.get()));
  }

  /**
   * @brief Rise time, dimensionally typed.
   * @param[in] rp pull-up resistance (SI ohms).
   * @param[in] cb total bus capacitance (SI farads).
   * @return 0.8473 * Rp * Cb as a time quantity (SI seconds).
   */
  [[nodiscard]] static constexpr units::Value<units::Time>
  rise_time(units::Value<units::ElectricResistance> rp,
            units::Value<units::Capacitance> cb) noexcept {
    return units::Value<units::Time>(rise_time_s(rp.get(), cb.get()));
  }
};

} // namespace wengine::solvers
