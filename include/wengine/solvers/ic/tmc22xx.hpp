#pragma once

// TMC2202 / TMC2208 / TMC2224 stepper driver design solver.
//
// Reference: TRINAMIC TMC2202/TMC2208/TMC2224 datasheet Rev. 1.14
// (2023-FEB-16), B. Dwersteg. All three parts share one die/behavior;
// 2208/2224 are QFN28, 2202 is QFN32; only the standalone MS1/MS2 pin
// decode and the SPREAD pin (2224 only) differ.
//
// Covered equations (SI base V / A / Ohm / Hz / s / W / Nm inside;
// register codes, counts and clocks are plain ints/doubles, never
// physical quantities):
//   Eq.1  IRMS = VFS/(RSENSE+30mOhm)/sqrt(2) * VVREF/2.5V   (standalone,
//         Ch.9: VFS=325mV vsense=0, 180mV vsense=1)
//   Eq.2  IRMS = (CS+1)/32 * VFS'/(RSENSE+30mOhm)/sqrt(2)   (CS=IRUN/IHOLD)
//   Eq.3  VFS' = VFS * VVREF/2.5V                            (analog scaling)
//   Eq.4  IPEAK(RREF) = 3000*5V/(RREF+700Ohm) + 0.12A,       (RDSon mode,
//         IRMS = IPEAK/sqrt(2); Ch.10: 0.7k VREF resistance, 40uA offset
//         (~120mA adder), 3000x gain; matches the RREF table to ~1%)
//   Eq.5  TOFF = (tOFF*fCLK - 24)/32, tOFF = (24+32*TOFF)/fCLK (Ch.7:
//         25kHz/50%-duty golden -> tOFF=10us -> TOFF=3 @12MHz, 4.25 @16MHz)
//   Eq.6  tBLANK = {16,24,32,40}/fCLK for TBL 0..3 (Ch.7)
//   Eq.7  fPWM = 2*fCLK/{1024,683,512,410} for PWM_FREQ 0..3 (Table 6.1:
//         12MHz -> 23.4/35.1/46.9/58.5kHz)
//   Eq.8  ILowerLimit = tBLANK*fPWM*VM/RCOIL                 (Sec. 6.3.1:
//         24clk, 2/1024, 24V, 5Ohm -> 225mA golden)
//   Eq.9  PWM_AMPL = 374*RCOIL*ICOIL/VM                      (Sec. 6.4)
//   Eq.10 PWM_SCALE_SUM = PWM_OFS*(CS+1)/32 + PWM_GRAD*256/TSTEP (Sec. 6.4)
//   Eq.11 PWM_GRAD = CBEMF*2*pi*fCLK*1.46/(VM*MSPR)          (Sec. 6.4)
//   Eq.12 CBEMF[V/(rad/s)] = HoldingTorque[Nm]/(2*ICOILNOM[A]) (Sec. 6.4)
//   Eq.13 v[Hz] = VACTUAL*fCLK/2^24 (=VACTUAL*0.715Hz @12MHz, Ch.12)
//   Eq.14 TSTEP = fCLK/f256STEP = 2^24/(VACTUAL*256/USC)     (Ch.12)
//   Eq.15 tPW = TPOWERDOWN*2^18/fCLK (0..5.6s @12MHz, Ch.5.2: TP=20 ->
//         0.437s); per-step tIH = IHOLDDELAY*2^18/fCLK (Ch.5.2 golden:
//         4*15 steps @16MHz -> 0.983s); standstill at 2^20 clocks (Ch.11.3)
//   Eq.16 Pcond = 2*IRMS^2*(RONH+RONL)                        (standard buck/
//         bridge estimate, NOT a datasheet equation: conduction floor only;
//         the 1A/24V row of Table 18.1 reports 2.5W incl. switching loss)
//   Eq.17 CRC8-ATM: CRC = x^8+x^2+x+1 (0x07), init 0, Sec. 4.2 C reference
//
// Tables carried as data: Ch.8 RSENSE->IRMS spot rows, Ch.10 RREF table,
// OTP_PWM_GRAD (16) / OTP_TPWMTHRS (8) / OTP_IHOLD / OTP_IHOLDDELAY maps,
// MRES codes, standalone MS1/MS2 decodes, OTTRIM thresholds, SENDDELAY map,
// UART frame sizes, electrical limits (Ch.17/18), thermal resistances.
//
// Worked goldens (re-derived in decimal, see tests/test_tmc22xx.cpp):
//   RSENSE=0.22Ohm, VREF=2.5V -> IRMS = 0.325/0.25/sqrt(2) = 0.9192A
//   TOFF=(10us*12MHz-24)/32 = 3.0; (10us*16MHz-24)/32 = 4.25
//   fPWM(12MHz, sel=1) = 24e6/683 = 35139.1Hz
//   ILower = 24 * 2/1024 * 24/5 = 0.225A
//   PWM_AMPL = 374*5*0.5/24 = 38.96; CBEMF = 0.4/(2*1) = 0.2 -> GRAD 17.92
//   VACTUAL=1000 @12MHz -> v = 715.26Hz, TSTEP = 16777.2
//   TPWMTHRS=500 @12MHz -> 24kHz/93.75FS/s/28.125rpm (200FS motor)
//   TPOWERDOWN=20 @12MHz -> 0.4369s
//
// All functions are pure, header-only, constexpr + noexcept.
// Input validation (ranges, > 0, ...) is the caller's job (CLI layer).

namespace wengine::solvers::ic {

struct Tmc22xxCurrentResult {
  double vfs_v{0.0};      // Eq.3 full-scale sense voltage in use
  double irms_run_a{0.0}; // Eq.1/Eq.2 run current
  double ipeak_run_a{0.0};
  double irms_hold_a{0.0}; // Eq.2 hold current (CS=IHOLD)
  double sense_drop_v{0.0}; // peak sense voltage at run current
  double sense_p_w{0.0};    // IRMS^2 * RSENSE per resistor (design value)
  bool peak_ok{false};      // peak <= 2.0A absolute operating max
  bool rms_ok{false};       // run RMS <= 1.2A continuous design guide
  bool ok{false};
};

struct Tmc22xxChopResult {
  double toff_s{0.0};   // Eq.5 slow-decay time
  double tblank_s{0.0}; // Eq.6 blank time
  unsigned nclk{0};     // 24 + 32*TOFF comparator clocks
  bool ok{false};
};

struct Tmc22xxHystResult {
  int hend_eff{0}; // register minus 3 (-3..12)
  int hstrt_eff{0}; // register plus 1 (1..8)
  int total_eff{0}; // effective hysteresis span
  bool ok{false};   // total <= 16 (Ch.7 attention box)
};

struct Tmc22xxVelocityResult {
  double usteps_hz{0.0}; // 1/256-step rate (Eq.13/Eq.14 domain)
  double fsteps_hz{0.0}; // fullstep rate (usteps / 256)
  double rps{0.0};
  double rpm{0.0};
};

struct Tmc22xxStandbyResult {
  double standstill_s{0.0}; // 2^20 clocks to stst flag
  double tpowerdown_s{0.0}; // Eq.15 TPOWERDOWN delay
  double decay_s{0.0};      // IHOLDDELAY ramp after TPOWERDOWN
  double total_s{0.0};
};

struct Tmc22xxThermalResult {
  double pcond_w{0.0}; // Eq.16 conduction floor (not datasheet)
  double rise_c{0.0};
  double tj_c{0.0};
  bool ok{false}; // TJ <= +125 C operating max
};

class Tmc22xxSolver {
 public:
  Tmc22xxSolver() = delete;

  // Fixed part parameters (ordering + electrical tables, Ch.17/18).
  static constexpr double kVsMinV = 4.75;   // operating with inductive load
  static constexpr double kVsMaxV = 36.0;   // 39V abs max w/ ringing margin
  static constexpr double kVsMinRegV = 5.5;  // using the internal 5V regulator
  static constexpr double kVioMinV = 3.0;
  static constexpr double kVioMaxV = 5.25;
  static constexpr double kIrmsContA = 1.2;  // continuous design guideline
  static constexpr double kIrmsDutyA = 1.4;  // duty-limited (1s on / 1s standby)
  static constexpr double kIpeakMaxA = 2.0;  // sine peak operating max
  static constexpr double kVfsLowV = 0.325;  // vsense=0 (VSRTL)
  static constexpr double kVfsHighV = 0.180; // vsense=1 (VSRTH)
  static constexpr double kRintOhm = 0.030;  // BRxy internal 30 mOhm
  static constexpr double kVrefNomV = 2.5;
  static constexpr double kSqrt2 = 1.4142135623730951;
  static constexpr double kFclkIntHz = 12.0e6; // factory-trimmed internal clock
  static constexpr double kRonLsOhm = 0.28;    // typ. at 25 C, 100mA
  static constexpr double kRonHsOhm = 0.29;
  static constexpr double kRsensePowerW = 0.5; // sufficient to ~1.2A RMS (Ch.8)
  static constexpr double kRthJaKpw = 30.0;    // QFN28 multilayer board
  static constexpr double kRthJcKpw = 6.0;     // junction to slug
  static constexpr double kTjMaxC = 125.0;
  static constexpr double kStepClocks = 1048576.0; // 2^20 standstill/MicroPlyer
  static constexpr double kTimingClocks = 262144.0; // 2^18 TPOWERDOWN unit
  static constexpr double kVactScale = 16777216.0;  // 2^24 VACTUAL divisor
  static constexpr unsigned kUstepsPerFullstep = 256;
  static constexpr int kFullstepsPerRev = 200; // classic 1.8-degree motor

  // RDSon reference model (Ch.10 principle of operation).
  static constexpr double kRrefVddV = 5.0;   // 5VOUT across RREF
  static constexpr double kRrefRinOhm = 700.0; // VREF input resistance
  static constexpr double kRrefGain = 3000.0;
  static constexpr double kRrefOffsetA = 0.12; // 40uA * 3000

  // UART (Ch.4): 9kBd min at 20MHz clock; max fCLK/16.
  static constexpr double kUartMinBd = 9000.0;
  static constexpr unsigned kUartWriteLen = 8; // sync..CRC bytes
  static constexpr unsigned kUartReadReqLen = 4;
  static constexpr unsigned kUartReadReplyLen = 8;

  // Ch.8 spot rows: RSENSE -> RMS at VREF=2.5V, IRUN=31, vsense=0.
  static constexpr int kRsenseRows = 6;
  static constexpr double kRsenseOhm[kRsenseRows] = {1.0, 0.50, 0.22, 0.15, 0.12, 0.10};
  static constexpr double kRsenseIrmsA[kRsenseRows] = {0.22, 0.43, 0.92, 1.28, 1.53, 1.77};

  // Ch.10 RREF table (CS=31, vsense=0): 11 rows.
  static constexpr int kRrefRows = 11;
  static constexpr double kRrefOhm[kRrefRows] = {6800.0,  7500.0,  8200.0,  9100.0, 10000.0,
                                                12000.0, 15000.0, 18000.0, 22000.0, 27000.0,
                                                33000.0};
  static constexpr double kRrefPeakA[kRrefRows] = {2.11, 1.94, 1.80, 1.64, 1.51, 1.29, 1.07, 0.91, 0.77, 0.65, 0.56};
  static constexpr double kRrefRmsA[kRrefRows] = {1.49, 1.37, 1.27, 1.16, 1.07, 0.91, 0.75, 0.65, 0.55, 0.46, 0.39};

  // OTP shadow maps (Sec. 5.1.1).
  static constexpr int kOtpPwmGradN = 16;
  static constexpr double kOtpPwmGrad[kOtpPwmGradN] = {14.0, 16.0, 18.0, 21.0, 24.0, 27.0,
                                                      31.0, 35.0, 40.0, 46.0, 52.0, 59.0,
                                                      67.0, 77.0, 88.0, 100.0};
  static constexpr int kOtpTpwmthrsN = 8;
  static constexpr double kOtpTpwmthrs[kOtpTpwmthrsN] = {0.0,    200.0,  300.0,  400.0, 500.0,  800.0,  1200.0, 4000.0};
  static constexpr int kOtpIholdN = 4;
  static constexpr int kOtpIhold[kOtpIholdN] = {16, 2, 8, 24};
  static constexpr int kOtpIholdDelayN = 4;
  static constexpr int kOtpIholdDelay[kOtpIholdDelayN] = {1, 2, 4, 8};

  // CHOPCONF helpers.
  static constexpr int kBlankClocks[4] = {16, 24, 32, 40};          // TBL 0..3
  static constexpr int kPwmDiv[4] = {1024, 683, 512, 410};           // PWM_FREQ 0..3

  // ------------------------------------------------------------ Eq.1/Eq.3
  [[nodiscard]] static constexpr double fullscale_v(int vsense) noexcept {
    return (vsense != 0) ? kVfsHighV : kVfsLowV;
  }

  [[nodiscard]] static constexpr double scaled_fullscale_v(double vfs_v, double vvref_v) noexcept {
    return vfs_v * vvref_v / kVrefNomV;
  }

  [[nodiscard]] static constexpr double irms_standalone_a(double rsense_ohm, double vvref_v, int vsense) noexcept {
    if (!(rsense_ohm > 0.0) || !(vvref_v > 0.0)) {
      return 0.0;
    }
    return scaled_fullscale_v(fullscale_v(vsense), vvref_v) / (rsense_ohm + kRintOhm) / kSqrt2;
  }

  // ----------------------------------------------------------------- Eq.2
  [[nodiscard]] static constexpr double irms_cs_a(int cs, double rsense_ohm, double vfs_v, double vvref_v) noexcept {
    if (cs < 0 || cs > 31 || !(rsense_ohm > 0.0) || !(vfs_v > 0.0) || !(vvref_v > 0.0)) {
      return 0.0;
    }
    return (static_cast<double>(cs) + 1.0) / 32.0 * scaled_fullscale_v(vfs_v, vvref_v) / (rsense_ohm + kRintOhm) / kSqrt2;
  }

  [[nodiscard]] static constexpr double ipeak_of_rms_a(double irms_a) noexcept {
    return irms_a * kSqrt2;
  }

  [[nodiscard]] static constexpr double irms_of_peak_a(double ipeak_a) noexcept {
    return ipeak_a / kSqrt2;
  }

  // Exact sense value for a target RMS (invert Eq.1); <= 0 when infeasible.
  [[nodiscard]] static constexpr double rsense_for_irms_ohm(double irms_a, double vvref_v, int vsense) noexcept {
    if (!(irms_a > 0.0) || !(vvref_v > 0.0)) {
      return -1.0;
    }
    return scaled_fullscale_v(fullscale_v(vsense), vvref_v) / (irms_a * kSqrt2) - kRintOhm;
  }

  // Smallest CS 0..31 reaching the target, -1 when even CS=31 falls short.
  [[nodiscard]] static constexpr int cs_for_irms(double irms_a, double rsense_ohm, double vfs_v, double vvref_v) noexcept {
    if (!(irms_a > 0.0) || !(rsense_ohm > 0.0) || !(vfs_v > 0.0) || !(vvref_v > 0.0)) {
      return -1;
    }
    const double exact = irms_a * 32.0 * (rsense_ohm + kRintOhm) * kSqrt2 / scaled_fullscale_v(vfs_v, vvref_v) - 1.0;
    long long cs = static_cast<long long>(exact);
    if (exact > static_cast<double>(cs)) {
      ++cs; // ceil without libm
    }
    if (cs < 0) {
      return 0;
    }
    if (cs > 31) {
      return -1;
    }
    return static_cast<int>(cs);
  }

  [[nodiscard]] static constexpr double sense_power_w(double irms_a, double rsense_ohm) noexcept {
    return irms_a * irms_a * rsense_ohm;
  }

  // ----------------------------------------------------------------- Eq.4
  [[nodiscard]] static constexpr double rref_ipeak_a(double rref_ohm, int vsense) noexcept {
    if (!(rref_ohm > 0.0)) {
      return 0.0;
    }
    const double peak = kRrefGain * kRrefVddV / (rref_ohm + kRrefRinOhm) + kRrefOffsetA;
    return (vsense != 0) ? peak * kVfsHighV / kVfsLowV : peak;
  }

  [[nodiscard]] static constexpr double rref_for_ipeak_ohm(double ipeak_a, int vsense) noexcept {
    if (!(ipeak_a > kRrefOffsetA)) {
      return -1.0;
    }
    double target = ipeak_a;
    if (vsense != 0) {
      target = ipeak_a * kVfsLowV / kVfsHighV;
    }
    return kRrefGain * kRrefVddV / (target - kRrefOffsetA) - kRrefRinOhm;
  }

  // Nearest RREF table row for a target peak current (row index, -1 empty).
  [[nodiscard]] static constexpr int rref_row_for_ipeak(double ipeak_a) noexcept {
    int best = -1;
    double best_err = 0.0;
    for (int i = 0; i < kRrefRows; ++i) {
      double err = kRrefPeakA[i] - ipeak_a;
      if (err < 0.0) {
        err = -err;
      }
      if (best < 0 || err < best_err) {
        best = i;
        best_err = err;
      }
    }
    return best;
  }

  // ------------------------------------------------------------- Eq.5/Eq.6
  [[nodiscard]] static constexpr double toff_time_s(int toff, double fclk_hz) noexcept {
    if (toff < 0 || toff > 15 || !(fclk_hz > 0.0)) {
      return -1.0;
    }
    return (24.0 + 32.0 * static_cast<double>(toff)) / fclk_hz;
  }

  [[nodiscard]] static constexpr double toff_for_time(double toff_s, double fclk_hz) noexcept {
    if (!(toff_s > 0.0) || !(fclk_hz > 0.0)) {
      return -1.0;
    }
    return (toff_s * fclk_hz - 24.0) / 32.0;
  }

  // TOFF setting from a chopper-frequency target: two slow-decay phases
  // make up `slow_frac` of the cycle (datasheet example: 0.5 at 25kHz).
  [[nodiscard]] static constexpr double toff_for_chop_freq(double fchop_hz, double slow_frac, double fclk_hz) noexcept {
    if (!(fchop_hz > 0.0) || !(slow_frac > 0.0) || !(fclk_hz > 0.0)) {
      return -1.0;
    }
    return toff_for_time(slow_frac / (2.0 * fchop_hz), fclk_hz);
  }

  [[nodiscard]] static constexpr int blank_clocks(int tbl) noexcept {
    return (tbl >= 0 && tbl < 4) ? kBlankClocks[tbl] : -1;
  }

  [[nodiscard]] static constexpr double blank_time_s(int tbl, double fclk_hz) noexcept {
    const int n = blank_clocks(tbl);
    if (n < 0 || !(fclk_hz > 0.0)) {
      return -1.0;
    }
    return static_cast<double>(n) / fclk_hz;
  }

  // ----------------------------------------------------------------- Eq.7
  [[nodiscard]] static constexpr double pwm_freq_hz(int sel, double fclk_hz) noexcept {
    if (sel < 0 || sel > 3 || !(fclk_hz > 0.0)) {
      return -1.0;
    }
    return 2.0 * fclk_hz / static_cast<double>(kPwmDiv[sel]);
  }

  // ------------------------------------------------------- HEND / HSTRT
  // Register 0..2 -> -3..-1, 3 -> 0, 4..15 -> 1..12 (offset by 3).
  [[nodiscard]] static constexpr int hend_effective(int hend_reg) noexcept {
    if (hend_reg < 0 || hend_reg > 15) {
      return -99;
    }
    return hend_reg - 3;
  }

  // Register 0..7 -> start offset 1..8 added to HEND.
  [[nodiscard]] static constexpr int hstrt_effective(int hstrt_reg) noexcept {
    if (hstrt_reg < 0 || hstrt_reg > 7) {
      return -99;
    }
    return hstrt_reg + 1;
  }

  [[nodiscard]] static constexpr Tmc22xxHystResult solve_hysteresis(int hend_reg, int hstrt_reg) noexcept {
    Tmc22xxHystResult r{};
    r.hend_eff = hend_effective(hend_reg);
    r.hstrt_eff = hstrt_effective(hstrt_reg);
    if (r.hend_eff == -99 || r.hstrt_eff == -99) {
      return r;
    }
    r.total_eff = r.hend_eff + r.hstrt_eff;
    r.ok = r.total_eff <= 16;
    return r;
  }

  // ------------------------------------------------------------- MRES
  // Code 0 -> native 256 microsteps; 1..8 halve down to FULLSTEP.
  [[nodiscard]] static constexpr int usteps_for_mres(int mres) noexcept {
    if (mres < 0 || mres > 8) {
      return -1;
    }
    return static_cast<int>(256u >> static_cast<unsigned>(mres));
  }

  [[nodiscard]] static constexpr int mres_for_usteps(int usteps) noexcept {
    for (int m = 0; m <= 8; ++m) {
      if (usteps_for_mres(m) == usteps) {
        return m;
      }
    }
    return -1;
  }

  // Standalone pin decode (Sec. 3.4): index is MS2*2+MS1.
  [[nodiscard]] static constexpr int standalone_usteps(bool is_220x, int ms1, int ms2) noexcept {
    if ((ms1 != 0 && ms1 != 1) || (ms2 != 0 && ms2 != 1)) {
      return -1;
    }
    const int idx = ms2 * 2 + ms1;
    if (is_220x) {
      constexpr int kMap[4] = {8, 2, 4, 16};
      return kMap[idx];
    }
    constexpr int kMap[4] = {4, 8, 16, 32};
    return kMap[idx];
  }

  // ------------------------------------------------------- Eq.13/Eq.14
  [[nodiscard]] static constexpr double vactual_to_usteps_hz(double vactual, double fclk_hz) noexcept {
    if (!(fclk_hz > 0.0)) {
      return 0.0;
    }
    return vactual * fclk_hz / kVactScale;
  }

  [[nodiscard]] static constexpr double usteps_hz_to_vactual(double v_hz, double fclk_hz) noexcept {
    if (!(fclk_hz > 0.0)) {
      return 0.0;
    }
    return v_hz * kVactScale / fclk_hz;
  }

  [[nodiscard]] static constexpr double tstep_to_usteps_hz(double tstep, double fclk_hz) noexcept {
    if (!(tstep > 0.0) || !(fclk_hz > 0.0)) {
      return 0.0;
    }
    return fclk_hz / tstep;
  }

  [[nodiscard]] static constexpr double usteps_hz_to_tstep(double v_hz, double fclk_hz) noexcept {
    if (!(v_hz > 0.0) || !(fclk_hz > 0.0)) {
      return 0.0;
    }
    return fclk_hz / v_hz;
  }

  // Motor shaft speed (rps, e.g. from --vel 28rpm) to the 1/256-step
  // rate the TSTEP/VACTUAL domain counts in.
  [[nodiscard]] static constexpr double motor_rps_to_usteps_hz(double rps, int fullsteps_per_rev) noexcept {
    if (!(rps >= 0.0) || fullsteps_per_rev <= 0) {
      return 0.0;
    }
    return rps * static_cast<double>(fullsteps_per_rev) *
           static_cast<double>(kUstepsPerFullstep);
  }

  [[nodiscard]] static constexpr Tmc22xxVelocityResult solve_velocity_usteps( double usteps_hz, int fullsteps_per_rev) noexcept {    
    Tmc22xxVelocityResult r{};
    if (!(usteps_hz >= 0.0) || fullsteps_per_rev <= 0) {
      return r;
    }
    r.usteps_hz = usteps_hz;
    r.fsteps_hz = usteps_hz / static_cast<double>(kUstepsPerFullstep);
    r.rps = r.fsteps_hz / static_cast<double>(fullsteps_per_rev);
    r.rpm = r.rps * 60.0;
    return r;
  }

  // ------------------------------------------------------- Eq.8/Eq.9
  [[nodiscard]] static constexpr double stealth_lower_limit_a(double tblank_s, double fpwm_hz, double vm_v, double rcoil_ohm) noexcept {
    if (!(tblank_s > 0.0) || !(fpwm_hz > 0.0) || !(vm_v >= 0.0) || !(rcoil_ohm > 0.0)) {
      return -1.0;
    }
    return tblank_s * fpwm_hz * vm_v / rcoil_ohm;
  }

  [[nodiscard]] static constexpr double pwm_ampl_seed(double rcoil_ohm, double icoil_a, double vm_v) noexcept {
    if (!(rcoil_ohm > 0.0) || !(icoil_a > 0.0) || !(vm_v > 0.0)) {
      return -1.0;
    }
    return 374.0 * rcoil_ohm * icoil_a / vm_v;
  }

  // ------------------------------------------------------ Eq.10/Eq.12
  [[nodiscard]] static constexpr double pwm_scale_sum(double pwm_ofs, int cs_actual, double pwm_grad, double tstep) noexcept {
    if (cs_actual < 0 || cs_actual > 31 || !(tstep > 0.0)) {
      return -1.0;
    }
    return pwm_ofs * (static_cast<double>(cs_actual) + 1.0) / 32.0 + pwm_grad * 256.0 / tstep;
  }

  [[nodiscard]] static constexpr double backemf_const_v_per_rads(double torque_nm, double icoilnom_a) noexcept {
    if (!(torque_nm > 0.0) || !(icoilnom_a > 0.0)) {
      return -1.0;
    }
    return torque_nm / (2.0 * icoilnom_a);
  }

  // ---------------------------------------------------------------- Eq.11
  [[nodiscard]] static constexpr double pwm_grad_seed(double cbemf, double fclk_hz, double vm_v, double mspr = 51200.0) noexcept {
    if (!(cbemf > 0.0) || !(fclk_hz > 0.0) || !(vm_v > 0.0) || !(mspr > 0.0)) {
      return -1.0;
    }
    return cbemf * 2.0 * 3.141592653589793 * fclk_hz * 1.46 / (vm_v * mspr);
  }

  // Nearest OTP table entries (Sec. 5.1.1); -1 on empty/out-of-range input.
  [[nodiscard]] static constexpr int otp_pwm_grad_idx(double value) noexcept {
    if (!(value >= 0.0)) {
      return -1;
    }
    int best = 0;
    double best_err = value - kOtpPwmGrad[0];
    if (best_err < 0.0) {
      best_err = -best_err;
    }
    for (int i = 1; i < kOtpPwmGradN; ++i) {
      double err = value - kOtpPwmGrad[i];
      if (err < 0.0) {
        err = -err;
      }
      if (err < best_err) {
        best = i;
        best_err = err;
      }
    }
    return best;
  }

  [[nodiscard]] static constexpr int otp_tpwmthrs_idx(double value) noexcept {
    if (!(value >= 0.0)) {
      return -1;
    }
    int best = 0;
    double best_err = value - kOtpTpwmthrs[0];
    if (best_err < 0.0) {
      best_err = -best_err;
    }
    for (int i = 1; i < kOtpTpwmthrsN; ++i) {
      double err = value - kOtpTpwmthrs[i];
      if (err < 0.0) {
        err = -err;
      }
      if (err < best_err) {
        best = i;
        best_err = err;
      }
    }
    return best;
  }

  [[nodiscard]] static constexpr int otp_ihold_code(int ihold) noexcept {
    for (int i = 0; i < kOtpIholdN; ++i) {
      if (kOtpIhold[i] == ihold) {
        return i;
      }
    }
    return -1;
  }

  [[nodiscard]] static constexpr int otp_iholddelay_code(int delay) noexcept {
    for (int i = 0; i < kOtpIholdDelayN; ++i) {
      if (kOtpIholdDelay[i] == delay) {
        return i;
      }
    }
    return -1;
  }

  // -------------------------------------------------------------- Eq.15
  [[nodiscard]] static constexpr double standstill_time_s(double fclk_hz) noexcept {
    if (!(fclk_hz > 0.0)) {
      return -1.0;
    }
    return kStepClocks / fclk_hz;
  }

  [[nodiscard]] static constexpr double tpowerdown_time_s(int tpowerdown, double fclk_hz) noexcept {
    if (tpowerdown < 0 || tpowerdown > 255 || !(fclk_hz > 0.0)) {
      return -1.0;
    }
    return static_cast<double>(tpowerdown) * kTimingClocks / fclk_hz;
  }

  [[nodiscard]] static constexpr double iholddelay_step_s(int iholddelay, double fclk_hz) noexcept {
    if (iholddelay < 0 || iholddelay > 15 || !(fclk_hz > 0.0)) {
      return -1.0;
    }
    return static_cast<double>(iholddelay) * kTimingClocks / fclk_hz;
  }

  [[nodiscard]] static constexpr Tmc22xxStandbyResult solve_standby(int tpowerdown, int irun, int ihold, int iholddelay, double fclk_hz) noexcept {
    Tmc22xxStandbyResult r{};
    r.standstill_s = standstill_time_s(fclk_hz);
    r.tpowerdown_s = tpowerdown_time_s(tpowerdown, fclk_hz);
    if (r.standstill_s < 0.0 || r.tpowerdown_s < 0.0 || irun < 0 || irun > 31 || ihold < 0 || ihold > 31) {
      return r;
    }
    const double step = iholddelay_step_s(iholddelay, fclk_hz);
    if (step < 0.0) {
      return r;
    }
    const int steps = (irun > ihold) ? (irun - ihold) : 0;
    r.decay_s = static_cast<double>(steps) * step;
    r.total_s = r.standstill_s + r.tpowerdown_s + r.decay_s;
    return r;
  }

  // -------------------------------------------------------------- Eq.16
  // Conduction floor (both coils, sine RMS); switching loss excluded.
  [[nodiscard]] static constexpr double conduction_loss_w(double irms_a) noexcept {
    if (!(irms_a >= 0.0)) {
      return -1.0;
    }
    return 2.0 * irms_a * irms_a * (kRonHsOhm + kRonLsOhm);
  }

  [[nodiscard]] static constexpr Tmc22xxThermalResult solve_thermal(double pd_w, double ta_c, double rth_kpw) noexcept {
    Tmc22xxThermalResult r{};
    if (!(pd_w >= 0.0) || !(rth_kpw > 0.0)) {
      return r;
    }
    r.pcond_w = pd_w;
    r.rise_c = pd_w * rth_kpw;
    r.tj_c = ta_c + r.rise_c;
    r.ok = r.tj_c <= kTjMaxC;
    return r;
  }

  // OTTRIM codes (FACTORY_CONF, Sec. 5.1): OT / OTPW in degree C.
  [[nodiscard]] static constexpr double ot_shutdown_c(int ottrim) noexcept {
    if (ottrim == 0) {
      return 143.0;
    }
    if (ottrim == 1 || ottrim == 2) {
      return 150.0;
    }
    if (ottrim == 3) {
      return 157.0;
    }
    return -1.0;
  }

  [[nodiscard]] static constexpr double otpw_warn_c(int ottrim) noexcept {
    if (ottrim == 0 || ottrim == 1) {
      return 120.0;
    }
    if (ottrim == 2 || ottrim == 3) {
      return 143.0;
    }
    return -1.0;
  }

  // ---------------------------------------------------------------- UART
  [[nodiscard]] static constexpr double uart_baud_max(double fclk_hz) noexcept {
    if (!(fclk_hz > 0.0)) {
      return -1.0;
    }
    return fclk_hz / 16.0;
  }

  // SENDDELAY code (NODECONF 11..8) -> reply delay in bit times.
  [[nodiscard]] static constexpr int senddelay_bits(int code) noexcept {
    if (code < 0 || code > 15) {
      return -1;
    }
    if (code < 2) {
      return 8;
    }
    return (code | 1) * 8;
  }

  // Eq.17 CRC8-ATM single-byte step (Sec. 4.2 C reference, init 0).
  [[nodiscard]] static constexpr unsigned crc8_step(unsigned crc, unsigned byte) noexcept {
    unsigned cur = byte & 0xFFu;
    crc &= 0xFFu;
    for (int j = 0; j < 8; ++j) {
      const unsigned mix = ((crc >> 7) & 1u) ^ (cur & 1u);
      crc = mix != 0u ? ((crc << 1) ^ 0x07u) & 0xFFu : (crc << 1) & 0xFFu;
      cur >>= 1;
    }
    return crc;
  }

  // ------------------------------------------------------------- design
  [[nodiscard]] static constexpr Tmc22xxCurrentResult solve_current(double rsense_ohm, int irun, int ihold, double vvref_v, int vsense) noexcept {
    Tmc22xxCurrentResult r{};
    if (!(rsense_ohm > 0.0) || irun < 0 || irun > 31 || ihold < 0 || ihold > 31 || !(vvref_v > 0.0) || (vsense != 0 && vsense != 1)) {
      return r;
    }
    r.vfs_v = scaled_fullscale_v(fullscale_v(vsense), vvref_v);
    r.irms_run_a = irms_cs_a(irun, rsense_ohm, fullscale_v(vsense), vvref_v);
    r.ipeak_run_a = ipeak_of_rms_a(r.irms_run_a);
    r.irms_hold_a = irms_cs_a(ihold, rsense_ohm, fullscale_v(vsense), vvref_v);
    r.sense_drop_v = r.ipeak_run_a * (rsense_ohm + kRintOhm);
    r.sense_p_w = sense_power_w(r.irms_run_a, rsense_ohm);
    r.peak_ok = r.ipeak_run_a <= kIpeakMaxA;
    r.rms_ok = r.irms_run_a <= kIrmsContA;
    r.ok = r.peak_ok && r.rms_ok;
    return r;
  }
};

static_assert(Tmc22xxSolver::hend_effective(0) == -3);
static_assert(Tmc22xxSolver::hend_effective(3) == 0);
static_assert(Tmc22xxSolver::hend_effective(6) == 3);
static_assert(Tmc22xxSolver::hend_effective(15) == 12);
static_assert(Tmc22xxSolver::hstrt_effective(0) == 1);
static_assert(Tmc22xxSolver::hstrt_effective(6) == 7);
static_assert(Tmc22xxSolver::usteps_for_mres(0) == 256);
static_assert(Tmc22xxSolver::usteps_for_mres(8) == 1);
static_assert(Tmc22xxSolver::mres_for_usteps(16) == 4);
static_assert(Tmc22xxSolver::standalone_usteps(true, 0, 0) == 8);
static_assert(Tmc22xxSolver::standalone_usteps(true, 1, 0) == 2);
static_assert(Tmc22xxSolver::standalone_usteps(true, 0, 1) == 4);
static_assert(Tmc22xxSolver::standalone_usteps(true, 1, 1) == 16);
static_assert(Tmc22xxSolver::standalone_usteps(false, 0, 0) == 4);
static_assert(Tmc22xxSolver::standalone_usteps(false, 1, 1) == 32);
static_assert(Tmc22xxSolver::blank_clocks(0) == 16);
static_assert(Tmc22xxSolver::blank_clocks(3) == 40);
static_assert(Tmc22xxSolver::blank_clocks(4) == -1);
static_assert(Tmc22xxSolver::senddelay_bits(0) == 8);
static_assert(Tmc22xxSolver::senddelay_bits(3) == 24);
static_assert(Tmc22xxSolver::senddelay_bits(15) == 120);
static_assert(Tmc22xxSolver::otp_pwm_grad_idx(14.0) == 0);
static_assert(Tmc22xxSolver::otp_pwm_grad_idx(100.0) == 15);
static_assert(Tmc22xxSolver::otp_tpwmthrs_idx(500.0) == 4);
static_assert(Tmc22xxSolver::otp_ihold_code(16) == 0);
static_assert(Tmc22xxSolver::otp_iholddelay_code(8) == 3);
static_assert(Tmc22xxSolver::ot_shutdown_c(0) == 143.0);
static_assert(Tmc22xxSolver::ot_shutdown_c(3) == 157.0);
static_assert(Tmc22xxSolver::otpw_warn_c(0) == 120.0);
static_assert(Tmc22xxSolver::otpw_warn_c(3) == 143.0);
static_assert(Tmc22xxSolver::crc8_step(0u, 0u) == 0u);
static_assert(Tmc22xxSolver::fullscale_v(0) == 0.325);
static_assert(Tmc22xxSolver::fullscale_v(1) == 0.180);
static_assert(Tmc22xxSolver::solve_hysteresis(6, 0).total_eff == 4);
static_assert(Tmc22xxSolver::solve_hysteresis(6, 0).ok);
static_assert(!Tmc22xxSolver::solve_hysteresis(15, 7).ok);

} // namespace wengine::solvers::ic
