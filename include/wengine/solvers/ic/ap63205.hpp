#pragma once

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

struct Ap63205InductorResult {
  double delta_il_a{0.0};
  double l_h{0.0};
  double peak_a{0.0};
  double ton_s{0.0};
  bool peak_ok{false}; // peak <= HS peak-limit minimum (2.5A)
  bool ton_ok{false};  // on-time >= 80ns minimum
};

struct Ap63205CoutResult {
  double ripple_v{0.0};
  double cout_min_f{0.0};
};

struct Ap63205UvloResult {
  double r5_ohm{0.0};
  double r6_ohm{0.0};
};

struct Ap63205ThermalResult {
  double pd_w{0.0};
  double rise_c{0.0};
  double tj_c{0.0};
  bool ok{false}; // TJ <= +125 C thermal-design target
};

class Ap63205Solver {
public:
  Ap63205Solver() = delete;

  // Fixed part parameters (ordering table + electrical tables).
  static constexpr double kVoutV = 5.0;
  static constexpr double kFswHz = 1100000.0;
  static constexpr double kMaxLoadA = 2.0;
  static constexpr double kVinMinV = 3.8;
  static constexpr double kVinMaxV = 32.0;
  static constexpr double kVinUvloV = 3.1; // internal UVLO disable level
  static constexpr double kThetaJaCpw = 89.0; // TSOT26
  static constexpr double kTjDesignMaxC = 125.0;
  static constexpr double kPeakLimitMinA = 2.5; // HS limit, worst-case column
  static constexpr double kTonMinS = 80e-9;
  static constexpr double kRdsHsOhm = 0.125;
  static constexpr double kRdsLsOhm = 0.068;
  static constexpr double kTssMs = 4.0; // built-in soft-start (info only)

  // Datasheet Table 3 recommendation for the AP63205 (5V, 1.1MHz).
  static constexpr double kTableLh = 4.7e-6;
  static constexpr double kTableCinF = 10e-6;
  static constexpr double kTableCoutF = 44e-6; // 2 x 22uF
  static constexpr double kTableBstF = 100e-9;

  // Design ripple band: 30%..50% of the 2A maximum load (section 10).
  static constexpr double kRippleLoA = 0.6;
  static constexpr double kRippleHiA = 1.0;
  static constexpr double kRippleDefaultFrac = 0.4;

  // ------------------------------------------------------------- ripple
  [[nodiscard]] static constexpr double default_ripple_a(double frac) noexcept {
    return frac * kMaxLoadA;
  }

  // ------------------------------------------------------------------ Eq.7
  [[nodiscard]] static constexpr double inductor_h(double vin_v, double vout_v, double delta_il_a, double fsw_hz) noexcept {
    return vout_v * (vin_v - vout_v) / (vin_v * delta_il_a * fsw_hz);
  }

  // ------------------------------------------------------------------ Eq.8
  [[nodiscard]] static constexpr double peak_current_a(double iload_a, double delta_il_a) noexcept {
    return iload_a + delta_il_a / 2.0;
  }

  [[nodiscard]] static constexpr double duty(double vin_v, double vout_v) noexcept {
    return vout_v / vin_v;
  }

  [[nodiscard]] static constexpr double on_time_s(double vin_v, double vout_v, double fsw_hz) noexcept {
    return duty(vin_v, vout_v) / fsw_hz;
  }

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
  [[nodiscard]] static constexpr double cout_ripple_v(double delta_il_a, double esr_ohm) noexcept {
    return delta_il_a * esr_ohm;
  }

  // ----------------------------------------------------------------- Eq.10
  [[nodiscard]] static constexpr double cout_min_f(double l_h, double iout_a, double delta_il_a, double vout_v, double delta_v_v) noexcept {
    const double i_peak = iout_a + delta_il_a / 2.0;
    const double denom = (delta_v_v + vout_v) * (delta_v_v + vout_v) - vout_v * vout_v;
    return l_h * i_peak * i_peak / denom;
  }

  [[nodiscard]] static constexpr Ap63205CoutResult solve_cout(double l_h, double iout_a, double delta_il_a, double esr_ohm, double delta_v_v) noexcept {
    Ap63205CoutResult r{};
    r.ripple_v = cout_ripple_v(delta_il_a, esr_ohm);
    r.cout_min_f = cout_min_f(l_h, iout_a, delta_il_a, kVoutV, delta_v_v);
    return r;
  }

  // ------------------------------------------------------------- Eq.1/Eq.2
  [[nodiscard]] static constexpr double uvlo_r5_ohm(double von_v, double voff_v) noexcept {
    return (0.932 * von_v - voff_v) / 4.1e-6;
  }

  [[nodiscard]] static constexpr double uvlo_r6_ohm(double voff_v, double r5_ohm) noexcept {
    return 1.1 * r5_ohm / (voff_v - 1.1 + 5.5e-6 * r5_ohm);
  }

  [[nodiscard]] static constexpr Ap63205UvloResult solve_uvlo(double von_v, double voff_v) noexcept {
    Ap63205UvloResult r{};
    r.r5_ohm = uvlo_r5_ohm(von_v, voff_v);
    r.r6_ohm = uvlo_r6_ohm(voff_v, r.r5_ohm);
    return r;
  }

  // ------------------------------------------------------------------ Eq.3
  [[nodiscard]] static constexpr double en_delay_cap_nf(double ts_ms) noexcept {
    return 1.27 * ts_ms;
  }

  [[nodiscard]] static constexpr double en_delay_time_ms(double cd_nf) noexcept {
    return cd_nf / 1.27;
  }

  // ------------------------------------------------------------- Eq.4/Eq.5
  [[nodiscard]] static constexpr double temp_rise_c(double pd_w) noexcept {
    return pd_w * kThetaJaCpw;
  }

  [[nodiscard]] static constexpr double junction_temp_c(double pd_w, double ta_c) noexcept {
    return ta_c + temp_rise_c(pd_w);
  }

  // Switch conduction-loss estimate (standard buck D*(Rhs)+(1-D)*(Rls)
  // model, NOT a datasheet equation): lets `thermal` run from VIN/IOUT
  // when no measured PD is at hand.
  [[nodiscard]] static constexpr double conduction_loss_w(double vin_v, double vout_v, double iout_a) noexcept {
    const double d = duty(vin_v, vout_v);
    return iout_a * iout_a * (d * kRdsHsOhm + (1.0 - d) * kRdsLsOhm);
  }

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
