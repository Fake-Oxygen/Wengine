#pragma once

/**
 * @file
 * AP63205WU-7 synchronous buck converter design solver: closed-form
 * sizing helpers for the inductor, output capacitor, EN UVLO divider,
 * startup delay, and thermal check. Owns the pure datasheet math only —
 * depends on nothing but plain constexpr arithmetic (units, math
 * downstream), never CLI types; input validation (VIN range, VON > VOFF,
 * ...) is the CLI layer's job per the solver error convention.
 * Reference: Diodes Incorporated AP63200/AP63201/AP63203/AP63205
 * datasheet, document DS41326 Rev. 3-2 (November 2024) — equations and
 * worked goldens preserved in the banner below. Consumed by the
 * `ic ap63205` CLI glue (src/cli/commands/ic/ap63205.cpp) and
 * tests/test_ap63205.cpp.
 */

// AP63205WU-7 synchronous buck converter design solver.
//
// Reference: Diodes Incorporated AP63200/AP63201/AP63203/AP63205
// datasheet, document DS41326 Rev. 3-2 (November 2024), Application
// Information sections 3-13. AP63205WU-7 ordering-table row: fixed 5.0V
// output, 1.1MHz switching frequency, PWM/PFM, TSOT26 package.
//
// Covered equations (all SI base V / A / H / F / s / W / Ohm inside):
//   Eq.1  R5 = (0.932*VON - VOFF) / 4.1uA                    (EN UVLO divider)
//   Eq.2  R6 = 1.1*R5 / (VOFF - 1.1V + 5.5uA*R5)
//   Eq.3  Cd[nF] = 1.27 * ts[ms]                             (EN startup delay)
//   Eq.4  TRISE = PD * thetaJA             (thetaJA = 89 C/W, TSOT26)
//   Eq.5  TJ = TA + TRISE                   (design target: TJ <= +125 C)
//   Eq.7  L = VOUT*(VIN - VOUT) / (VIN*dIL*fSW)               (inductor)
//   Eq.8  IL_PEAK = ILOAD + dIL/2                            (peak current)
//   Eq.9  VOUT_ripple = dIL * ESR                            (output ripple)
//   Eq.10 COUT > L*(IOUT + dIL/2)^2 / ((dV + VOUT)^2 - VOUT^2) (transient)
// Worked goldens (hand-derived, see tests/test_ap63205.cpp):
//   VIN=12V, dIL=0.8A -> L = 3.3143939uH, IL_PEAK(2A) = 2.4A
//   dIL=0.6A (30%) -> L = 4.4191919uH ~= Table 3 pick 4.7uH
//   VON=8V, VOFF=7V -> R5 = 111.2195kOhm, R6 = 18.7879kOhm
//
// Notes: the AP63205 output is fixed at 5V (no feedback divider; Eq.6 is
// the adjustable AP63200/AP63201 sibling only). The conduction-loss
// helper below is a standard buck estimate (DCR-free switch losses), not
// a datasheet equation, and is labeled as such at call sites.
//
// All functions are pure, header-only, constexpr + noexcept.
// Input validation (VIN range, VON > VOFF, ...) is the caller's job
// (CLI layer).

namespace wengine::solvers::ic {

/** @brief Inductor sizing result for one (VIN, ILOAD, dIL) corner (SI units). */
struct Ap63205InductorResult {
  /** @brief Design ripple current dIL, in amperes. */
  double delta_il_a{0.0};
  /** @brief Eq.7 inductance VOUT*(VIN-VOUT)/(VIN*dIL*fSW), in henries. */
  double l_h{0.0};
  /** @brief Eq.8 peak current ILOAD + dIL/2, in amperes. */
  double peak_a{0.0};
  /** @brief On-time at this corner, in seconds. */
  double ton_s{0.0};
  bool peak_ok{false}; /**< @brief peak <= HS peak-limit minimum (2.5A). */
  bool ton_ok{false};  /**< @brief on-time >= 80ns minimum. */
};

/** @brief Output-capacitor sizing result (ripple + transient minimum). */
struct Ap63205CoutResult {
  /** @brief Eq.9 output ripple dIL * ESR, in volts. */
  double ripple_v{0.0};
  /** @brief Eq.10 transient minimum, in farads. */
  double cout_min_f{0.0};
};

/** @brief EN UVLO divider result (Eq.1/Eq.2), in ohms. */
struct Ap63205UvloResult {
  /** @brief Eq.1 divider upper leg R5 = (0.932*VON - VOFF) / 4.1uA, in ohms. */
  double r5_ohm{0.0};
  /** @brief Eq.2 divider lower leg R6 = 1.1*R5 / (VOFF - 1.1V + 5.5uA*R5), in ohms. */
  double r6_ohm{0.0};
};

/** @brief Thermal check result (Eq.4/Eq.5, first-order). */
struct Ap63205ThermalResult {
  /** @brief Dissipated power used for the check, in watts. */
  double pd_w{0.0};
  /** @brief Eq.4 rise PD * thetaJA, in degrees C. */
  double rise_c{0.0};
  /** @brief Eq.5 junction temperature TA + rise, in degrees C. */
  double tj_c{0.0};
  bool ok{false}; /**< @brief TJ <= +125 C thermal-design target. */
};

/**
 * @brief AP63205WU-7 buck converter design solver (static-only utility).
 *
 * @note Pure, header-only, constexpr + noexcept: every formula is one
 *       datasheet equation (numbered per the banner above), exactly
 *       reproducible with no accumulation error. Zero/negative sentinels
 *       (out-of-range inputs) are domain values; callers validate at
 *       the CLI layer. The part's output is fixed at 5 V — no feedback
 *       divider here (Eq.6 belongs to the adjustable AP63200/AP63201
 *       sibling only).
 */
class Ap63205Solver {
public:
  /** @brief Deleted constructor: every solver here is a static function. */
  Ap63205Solver() = delete;

  // Fixed part parameters (ordering table + electrical tables).
  /** @brief Fixed 5.0 V output (ordering-table row for the WU-7). */
  static constexpr double kVoutV = 5.0;
  /** @brief Fixed 1.1 MHz PWM switching frequency. */
  static constexpr double kFswHz = 1100000.0;
  /** @brief Maximum continuous load current, in amperes. */
  static constexpr double kMaxLoadA = 2.0;
  /** @brief Minimum operating input voltage, in volts. */
  static constexpr double kVinMinV = 3.8;
  /** @brief Maximum operating input voltage, in volts. */
  static constexpr double kVinMaxV = 32.0;
  static constexpr double kVinUvloV = 3.1; /**< @brief Internal UVLO disable level, in volts. */
  static constexpr double kThetaJaCpw = 89.0; /**< @brief Junction-to-ambient thermal resistance (TSOT26), in C/W. */
  /** @brief Thermal design target for TJ, in degrees C. */
  static constexpr double kTjDesignMaxC = 125.0;
  static constexpr double kPeakLimitMinA = 2.5; /**< @brief HS switch peak-current limit minimum (worst-case column), in amperes. */
  /** @brief Minimum HS on-time, in seconds. */
  static constexpr double kTonMinS = 80e-9;
  /** @brief HS switch on-resistance (typical), in ohms. */
  static constexpr double kRdsHsOhm = 0.125;
  /** @brief LS switch on-resistance (typical), in ohms. */
  static constexpr double kRdsLsOhm = 0.068;
  static constexpr double kTssMs = 4.0; /**< @brief Built-in soft-start (info only), in milliseconds. */

  // Datasheet Table 3 recommendation for the AP63205 (5V, 1.1MHz).
  /** @brief Table 3 recommended inductor, in henries. */
  static constexpr double kTableLh = 4.7e-6;
  /** @brief Table 3 recommended input capacitor, in farads. */
  static constexpr double kTableCinF = 10e-6;
  static constexpr double kTableCoutF = 44e-6; /**< @brief Table 3 recommended output capacitance (2 x 22uF), in farads. */
  /** @brief Table 3 recommended bootstrap capacitor, in farads. */
  static constexpr double kTableBstF = 100e-9;

  // Design ripple band: 30%..50% of the 2A maximum load (section 10).
  /** @brief Ripple band lower edge (30% of 2 A), in amperes. */
  static constexpr double kRippleLoA = 0.6;
  /** @brief Ripple band upper edge (50% of 2 A), in amperes. */
  static constexpr double kRippleHiA = 1.0;
  /** @brief Default ripple fraction inside the band (dimensionless). */
  static constexpr double kRippleDefaultFrac = 0.4;

  // ------------------------------------------------------------- ripple
  /**
   * @brief Design ripple current as a fraction of the 2 A maximum load.
   * @param[in] frac ripple fraction (0.3..0.5 per section 10).
   * @return frac * kMaxLoadA, in amperes.
   */
  [[nodiscard]] static constexpr double default_ripple_a(double frac) noexcept {
    return frac * kMaxLoadA;
  }

  // ------------------------------------------------------------------ Eq.7
  /**
   * @brief Inductor value (datasheet Eq.7).
   * @param[in] vin_v input voltage, in volts.
   * @param[in] vout_v output voltage, in volts.
   * @param[in] delta_il_a design ripple current dIL, in amperes.
   * @param[in] fsw_hz switching frequency, in hertz.
   * @return L = VOUT*(VIN - VOUT) / (VIN*dIL*fSW), in henries.
   */
  [[nodiscard]] static constexpr double inductor_h(double vin_v, double vout_v, double delta_il_a, double fsw_hz) noexcept {
    return vout_v * (vin_v - vout_v) / (vin_v * delta_il_a * fsw_hz);
  }

  // ------------------------------------------------------------------ Eq.8
  /**
   * @brief Peak switch current (datasheet Eq.8).
   * @param[in] iload_a load current, in amperes.
   * @param[in] delta_il_a design ripple current, in amperes.
   * @return IL_PEAK = ILOAD + dIL/2, in amperes.
   */
  [[nodiscard]] static constexpr double peak_current_a(double iload_a, double delta_il_a) noexcept {
    return iload_a + delta_il_a / 2.0;
  }

  /**
   * @brief Ideal buck duty cycle at this corner.
   * @param[in] vin_v input voltage, in volts.
   * @param[in] vout_v output voltage, in volts.
   * @return VOUT / VIN as a dimensionless fraction.
   */
  [[nodiscard]] static constexpr double duty(double vin_v, double vout_v) noexcept {
    return vout_v / vin_v;
  }

  /**
   * @brief HS on-time from duty and frequency.
   * @param[in] vin_v input voltage, in volts.
   * @param[in] vout_v output voltage, in volts.
   * @param[in] fsw_hz switching frequency, in hertz.
   * @return D / fSW, in seconds.
   */
  [[nodiscard]] static constexpr double on_time_s(double vin_v, double vout_v, double fsw_hz) noexcept {
    return duty(vin_v, vout_v) / fsw_hz;
  }

  /**
   * @brief Full inductor sizing for one corner (Eq.7 + Eq.8 + checks).
   * @param[in] vin_v input voltage, in volts.
   * @param[in] iload_a load current, in amperes.
   * @param[in] delta_il_a design ripple current, in amperes.
   * @return Inductance, peak current, on-time, and the peak_ok /
   *         ton_ok verdicts against the part limits.
   */
  [[nodiscard]] static constexpr Ap63205InductorResult solve_inductor(double vin_v, double iload_a, double delta_il_a) noexcept {
    Ap63205InductorResult r{};
    r.delta_il_a = delta_il_a;
    r.l_h = inductor_h(vin_v, kVoutV, delta_il_a, kFswHz);
    r.peak_a = peak_current_a(iload_a, delta_il_a);
    r.ton_s = on_time_s(vin_v, kVoutV, kFswHz);
    r.peak_ok = r.peak_a <= kPeakLimitMinA;
    r.ton_ok = r.ton_s >= kTonMinS;
    return r;
  }

  // ------------------------------------------------------------------ Eq.9
  /**
   * @brief Output ripple from ESR (datasheet Eq.9).
   * @param[in] delta_il_a design ripple current, in amperes.
   * @param[in] esr_ohm output capacitor ESR, in ohms.
   * @return dIL * ESR, in volts.
   */
  [[nodiscard]] static constexpr double cout_ripple_v(double delta_il_a, double esr_ohm) noexcept {
    return delta_il_a * esr_ohm;
  }

  // ----------------------------------------------------------------- Eq.10
  /**
   * @brief Output capacitance for a load transient (datasheet Eq.10).
   * @param[in] l_h inductor value in use, in henries.
   * @param[in] iout_a load current, in amperes.
   * @param[in] delta_il_a design ripple current, in amperes.
   * @param[in] vout_v output voltage, in volts.
   * @param[in] delta_v_v allowed transient dip, in volts.
   * @return COUT > L*(IOUT + dIL/2)^2 / ((dV + VOUT)^2 - VOUT^2),
   *         in farads (the stated lower bound).
   */
  [[nodiscard]] static constexpr double cout_min_f(double l_h, double iout_a, double delta_il_a, double vout_v, double delta_v_v) noexcept {
    const double i_peak = iout_a + delta_il_a / 2.0;
    const double denom = (delta_v_v + vout_v) * (delta_v_v + vout_v) - vout_v * vout_v;
    return l_h * i_peak * i_peak / denom;
  }

  /**
   * @brief Full output-capacitor sizing (Eq.9 + Eq.10 at the fixed 5 V).
   * @param[in] l_h inductor value in use, in henries.
   * @param[in] iout_a load current, in amperes.
   * @param[in] delta_il_a design ripple current, in amperes.
   * @param[in] esr_ohm output capacitor ESR, in ohms.
   * @param[in] delta_v_v allowed transient dip, in volts.
   * @return ESR ripple and the Eq.10 transient minimum, in volts and
   *         farads.
   */
  [[nodiscard]] static constexpr Ap63205CoutResult solve_cout(double l_h, double iout_a, double delta_il_a, double esr_ohm, double delta_v_v) noexcept {
    Ap63205CoutResult r{};
    r.ripple_v = cout_ripple_v(delta_il_a, esr_ohm);
    r.cout_min_f = cout_min_f(l_h, iout_a, delta_il_a, kVoutV, delta_v_v);
    return r;
  }

  // ------------------------------------------------------------- Eq.1/Eq.2
  /**
   * @brief EN UVLO divider upper leg (datasheet Eq.1).
   * @param[in] von_v wanted enable (rising) threshold, in volts.
   * @param[in] voff_v wanted disable (falling) threshold, in volts.
   * @return R5 = (0.932*VON - VOFF) / 4.1uA, in ohms.
   */
  [[nodiscard]] static constexpr double uvlo_r5_ohm(double von_v, double voff_v) noexcept {
    return (0.932 * von_v - voff_v) / 4.1e-6;
  }

  /**
   * @brief EN UVLO divider lower leg (datasheet Eq.2).
   * @param[in] voff_v wanted disable (falling) threshold, in volts.
   * @param[in] r5_ohm divider upper leg from Eq.1, in ohms.
   * @return R6 = 1.1*R5 / (VOFF - 1.1V + 5.5uA*R5), in ohms.
   */
  [[nodiscard]] static constexpr double uvlo_r6_ohm(double voff_v, double r5_ohm) noexcept {
    return 1.1 * r5_ohm / (voff_v - 1.1 + 5.5e-6 * r5_ohm);
  }

  /**
   * @brief Full EN UVLO divider for a (VON, VOFF) pair.
   * @param[in] von_v wanted enable (rising) threshold, in volts.
   * @param[in] voff_v wanted disable (falling) threshold, in volts.
   * @return Divider legs R5 (Eq.1) and R6 (Eq.2), in ohms.
   */
  [[nodiscard]] static constexpr Ap63205UvloResult solve_uvlo(double von_v, double voff_v) noexcept {
    Ap63205UvloResult r{};
    r.r5_ohm = uvlo_r5_ohm(von_v, voff_v);
    r.r6_ohm = uvlo_r6_ohm(voff_v, r.r5_ohm);
    return r;
  }

  // ------------------------------------------------------------------ Eq.3
  /**
   * @brief EN delay capacitor for a wanted startup delay (Eq.3).
   * @param[in] ts_ms wanted EN startup delay, in milliseconds.
   * @return CD = 1.27 * ts, in nanofarads.
   */
  [[nodiscard]] static constexpr double en_delay_cap_nf(double ts_ms) noexcept {
    return 1.27 * ts_ms;
  }

  /**
   * @brief Inverse of Eq.3: the delay a capacitor yields.
   * @param[in] cd_nf EN capacitor, in nanofarads.
   * @return ts = CD / 1.27, in milliseconds.
   */
  [[nodiscard]] static constexpr double en_delay_time_ms(double cd_nf) noexcept {
    return cd_nf / 1.27;
  }

  // ------------------------------------------------------------- Eq.4/Eq.5
  /**
   * @brief Junction rise over ambient (datasheet Eq.4).
   * @param[in] pd_w dissipated power, in watts.
   * @return TRISE = PD * thetaJA (89 C/W, TSOT26), in degrees C.
   */
  [[nodiscard]] static constexpr double temp_rise_c(double pd_w) noexcept {
    return pd_w * kThetaJaCpw;
  }

  /**
   * @brief Junction temperature (datasheet Eq.5).
   * @param[in] pd_w dissipated power, in watts.
   * @param[in] ta_c ambient temperature, in degrees C.
   * @return TJ = TA + TRISE, in degrees C.
   */
  [[nodiscard]] static constexpr double junction_temp_c(double pd_w, double ta_c) noexcept {
    return ta_c + temp_rise_c(pd_w);
  }

  // Switch conduction-loss estimate (standard buck D*(Rhs)+(1-D)*(Rls)
  // model, NOT a datasheet equation): lets `thermal` run from VIN/IOUT
  // when no measured PD is at hand.
  /**
   * @brief Switch conduction-loss estimate (NOT a datasheet equation).
   *
   * Standard buck model D*RonHs + (1-D)*RonLs at the load current —
   * labeled as such at call sites so `thermal` can run from VIN/IOUT
   * when no measured PD is at hand.
   *
   * @param[in] vin_v input voltage, in volts.
   * @param[in] vout_v output voltage, in volts.
   * @param[in] iout_a load current, in amperes.
   * @return Conduction floor IOUT^2 * (D*RonHs + (1-D)*RonLs), in watts.
   */
  [[nodiscard]] static constexpr double conduction_loss_w(double vin_v, double vout_v, double iout_a) noexcept {
    const double d = duty(vin_v, vout_v);
    return iout_a * iout_a * (d * kRdsHsOhm + (1.0 - d) * kRdsLsOhm);
  }

  /**
   * @brief Thermal check for one dissipation (Eq.4/Eq.5 + target).
   * @param[in] pd_w dissipated power, in watts (measured, or the
   *            conduction-loss estimate above).
   * @param[in] ta_c ambient temperature, in degrees C.
   * @return Rise, junction temperature, and the ok verdict against the
   *         +125 C design target.
   */
  [[nodiscard]] static constexpr Ap63205ThermalResult solve_thermal(double pd_w, double ta_c) noexcept {
    Ap63205ThermalResult r{};
    r.pd_w = pd_w;
    r.rise_c = temp_rise_c(pd_w);
    r.tj_c = ta_c + r.rise_c;
    r.ok = r.tj_c <= kTjDesignMaxC;
    return r;
  }
};

} // namespace wengine::solvers::ic
