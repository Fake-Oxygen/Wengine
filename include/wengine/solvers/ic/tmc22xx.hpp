#pragma once

/**
 * @file
 * TMC2202 / TMC2208 / TMC2224 stepper driver design solver: closed-form
 * sizing for sense resistors and run/hold current, RREF chopper mode,
 * chopper/blank/PWM timing, velocity (VACTUAL/TSTEP), standby and
 * thermal, plus the UART framing constants and the CRC8-ATM step. Owns
 * the pure datasheet math only — depends on nothing but plain
 * constexpr arithmetic, never CLI types; register codes, counts, and
 * clocks are plain ints/doubles, never physical quantities; input
 * validation (ranges, > 0, ...) is the CLI layer's job per the solver
 * error convention. Reference: TRINAMIC TMC2202/TMC2208/TMC2224
 * datasheet Rev. 1.14 (2023-FEB-16), B. Dwersteg — all three parts
 * share one die; equations, tables, and worked goldens are preserved
 * in the banner below. Consumed by the `ic tmc22xx` CLI glue
 * (src/cli/commands/ic/tmc22xx.cpp) and tests/test_tmc22xx.cpp.
 */

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

/** @brief Run/hold current sizing result for one (RSENSE, VREF) choice (SI units). */
struct Tmc22xxCurrentResult {
  double vfs_v{0.0};      /**< @brief Eq.3 full-scale sense voltage in use, in volts. */
  double irms_run_a{0.0}; /**< @brief Eq.1/Eq.2 run current, in amperes. */
  /** @brief Run sine peak current IRMS * sqrt(2), in amperes. */
  double ipeak_run_a{0.0};
  double irms_hold_a{0.0}; /**< @brief Eq.2 hold current (CS=IHOLD), in amperes. */
  double sense_drop_v{0.0}; /**< @brief Peak sense voltage at run current, in volts. */
  double sense_p_w{0.0};    /**< @brief IRMS^2 * RSENSE per resistor (design value), in watts. */
  bool peak_ok{false};      /**< @brief Peak <= 2.0A absolute operating max. */
  bool rms_ok{false};       /**< @brief Run RMS <= 1.2A continuous design guide. */
  /** @brief peak_ok && rms_ok (design verdict). */
  bool ok{false};
};

/** @brief Chopper timing result (Eq.5/Eq.6, slow decay + blank time). */
struct Tmc22xxChopResult {
  double toff_s{0.0};   /**< @brief Eq.5 slow-decay time, in seconds. */
  double tblank_s{0.0}; /**< @brief Eq.6 blank time, in seconds. */
  unsigned nclk{0};     /**< @brief 24 + 32*TOFF comparator clocks. */
  /** @brief Both settings inside their valid ranges. */
  bool ok{false};
};

/** @brief Effective hysteresis decoding (HEND/HSTRT with offsets). */
struct Tmc22xxHystResult {
  int hend_eff{0}; /**< @brief Effective HEND: register minus 3 (-3..12). */
  int hstrt_eff{0}; /**< @brief Effective HSTRT: register plus 1 (1..8). */
  int total_eff{0}; /**< @brief Effective hysteresis span (hend + hstrt). */
  bool ok{false};   /**< @brief total <= 16 (Ch.7 attention box). */
};

/** @brief Velocity in the 1/256-step domain plus shaft rates (SI units). */
struct Tmc22xxVelocityResult {
  double usteps_hz{0.0}; /**< @brief 1/256-step rate (Eq.13/Eq.14 domain), in hertz. */
  double fsteps_hz{0.0}; /**< @brief Fullstep rate (usteps / 256), in hertz. */
  /** @brief Shaft revolutions per second. */
  double rps{0.0};
  /** @brief Shaft revolutions per minute. */
  double rpm{0.0};
};

/** @brief Standby timing chain (standstill + TPOWERDOWN + IHOLDDELAY ramp). */
struct Tmc22xxStandbyResult {
  double standstill_s{0.0}; /**< @brief 2^20 clocks to the stst flag, in seconds. */
  double tpowerdown_s{0.0}; /**< @brief Eq.15 TPOWERDOWN delay, in seconds. */
  double decay_s{0.0};      /**< @brief IHOLDDELAY ramp after TPOWERDOWN, in seconds. */
  /** @brief standstill + tpowerdown + decay, in seconds. */
  double total_s{0.0};
};

/** @brief Thermal estimate result (Eq.16 conduction floor + TJ check). */
struct Tmc22xxThermalResult {
  double pcond_w{0.0}; /**< @brief Eq.16 conduction floor (not datasheet), in watts. */
  /** @brief Junction rise PD * Rth, in degrees C. */
  double rise_c{0.0};
  /** @brief Junction temperature TA + rise, in degrees C. */
  double tj_c{0.0};
  bool ok{false}; /**< @brief TJ <= +125 C operating max. */
};

/**
 * @brief TMC2202/TMC2208/TMC2224 stepper driver design solver
 *        (static-only utility).
 *
 * @note Pure, header-only, constexpr + noexcept: every formula is one
 *       datasheet equation (numbered per the banner above), exactly
 *       reproducible with no accumulation error. Negative/zero
 *       sentinels (-1.0, 0.0, -1) mark invalid or infeasible inputs and
 *       are domain values — callers validate at the CLI layer.
 */
class Tmc22xxSolver {
 public:
  /** @brief Deleted constructor: every solver here is a static function. */
  Tmc22xxSolver() = delete;

  // Fixed part parameters (ordering + electrical tables, Ch.17/18).
  static constexpr double kVsMinV = 4.75;   /**< @brief Minimum VS operating with inductive load, in volts. */
  static constexpr double kVsMaxV = 36.0;   /**< @brief Maximum VS (39V abs max w/ ringing margin), in volts. */
  static constexpr double kVsMinRegV = 5.5;  /**< @brief Minimum VS when using the internal 5V regulator, in volts. */
  /** @brief Minimum VIO supply, in volts. */
  static constexpr double kVioMinV = 3.0;
  /** @brief Maximum VIO supply, in volts. */
  static constexpr double kVioMaxV = 5.25;
  static constexpr double kIrmsContA = 1.2;  /**< @brief Continuous RMS current design guideline, in amperes. */
  static constexpr double kIrmsDutyA = 1.4;  /**< @brief Duty-limited RMS (1s on / 1s standby), in amperes. */
  static constexpr double kIpeakMaxA = 2.0;  /**< @brief Sine peak operating maximum, in amperes. */
  static constexpr double kVfsLowV = 0.325;  /**< @brief Full-scale sense voltage, vsense=0 (VSRTL), in volts. */
  static constexpr double kVfsHighV = 0.180; /**< @brief Full-scale sense voltage, vsense=1 (VSRTH), in volts. */
  static constexpr double kRintOhm = 0.030;  /**< @brief BRxy internal 30 mOhm resistance, in ohms. */
  /** @brief Nominal VREF for full-scale current, in volts. */
  static constexpr double kVrefNomV = 2.5;
  /** @brief sqrt(2) constant for RMS/peak conversion. */
  static constexpr double kSqrt2 = 1.4142135623730951;
  static constexpr double kFclkIntHz = 12.0e6; /**< @brief Factory-trimmed internal clock, in hertz. */
  static constexpr double kRonLsOhm = 0.28;    /**< @brief LS switch on-resistance (typ. 25 C, 100mA), in ohms. */
  /** @brief HS switch on-resistance (typ. 25 C, 100mA), in ohms. */
  static constexpr double kRonHsOhm = 0.29;
  static constexpr double kRsensePowerW = 0.5; /**< @brief Sufficient RSENSE dissipation to ~1.2A RMS (Ch.8), in watts. */
  static constexpr double kRthJaKpw = 30.0;    /**< @brief Junction-to-ambient thermal resistance (QFN28 multilayer board), in K/W. */
  static constexpr double kRthJcKpw = 6.0;     /**< @brief Junction-to-slug (case) thermal resistance, in K/W. */
  /** @brief Maximum junction operating temperature, in degrees C. */
  static constexpr double kTjMaxC = 125.0;
  static constexpr double kStepClocks = 1048576.0; /**< @brief 2^20 standstill/MicroPlyer clock count. */
  static constexpr double kTimingClocks = 262144.0; /**< @brief 2^18 TPOWERDOWN timing unit, in clocks. */
  static constexpr double kVactScale = 16777216.0;  /**< @brief 2^24 VACTUAL divisor. */
  /** @brief Microsteps per fullstep (native 1/256 stepping). */
  static constexpr unsigned kUstepsPerFullstep = 256;
  static constexpr int kFullstepsPerRev = 200; /**< @brief Classic 1.8-degree motor geometry, fullsteps per revolution. */

  // RDSon reference model (Ch.10 principle of operation).
  static constexpr double kRrefVddV = 5.0;   /**< @brief 5VOUT across RREF, in volts. */
  static constexpr double kRrefRinOhm = 700.0; /**< @brief VREF input resistance, in ohms. */
  /** @brief RDSon reference model current gain (Ch.10), dimensionless. */
  static constexpr double kRrefGain = 3000.0;
  static constexpr double kRrefOffsetA = 0.12; /**< @brief 40uA * 3000 current offset, in amperes. */

  // UART (Ch.4): 9kBd min at 20MHz clock; max fCLK/16.
  /** @brief Minimum UART baud rate (Ch.4), in baud. */
  static constexpr double kUartMinBd = 9000.0;
  static constexpr unsigned kUartWriteLen = 8; /**< @brief Write frame length (sync..CRC bytes). */
  /** @brief Read request frame length, in bytes. */
  static constexpr unsigned kUartReadReqLen = 4;
  /** @brief Read reply frame length, in bytes. */
  static constexpr unsigned kUartReadReplyLen = 8;

  // Ch.8 spot rows: RSENSE -> RMS at VREF=2.5V, IRUN=31, vsense=0.
  /** @brief Number of Ch.8 RSENSE spot rows. */
  static constexpr int kRsenseRows = 6;
  /** @brief Ch.8 RSENSE spot rows, in ohms (by index). */
  static constexpr double kRsenseOhm[kRsenseRows] = {1.0, 0.50, 0.22, 0.15, 0.12, 0.10};
  /** @brief Ch.8 RMS currents at VREF=2.5V, IRUN=31, vsense=0, in amperes (by index). */
  static constexpr double kRsenseIrmsA[kRsenseRows] = {0.22, 0.43, 0.92, 1.28, 1.53, 1.77};

  // Ch.10 RREF table (CS=31, vsense=0): 11 rows.
  /** @brief Number of Ch.10 RREF table rows. */
  static constexpr int kRrefRows = 11;
  /** @brief Ch.10 RREF table resistances, in ohms (by index). */
  static constexpr double kRrefOhm[kRrefRows] = {6800.0,  7500.0,  8200.0,  9100.0, 10000.0,
                                                12000.0, 15000.0, 18000.0, 22000.0, 27000.0,
                                                33000.0};
  /** @brief Ch.10 RREF table peak currents (CS=31, vsense=0), in amperes (by index). */
  static constexpr double kRrefPeakA[kRrefRows] = {2.11, 1.94, 1.80, 1.64, 1.51, 1.29, 1.07, 0.91, 0.77, 0.65, 0.56};
  /** @brief Ch.10 RREF table RMS currents (CS=31, vsense=0), in amperes (by index). */
  static constexpr double kRrefRmsA[kRrefRows] = {1.49, 1.37, 1.27, 1.16, 1.07, 0.91, 0.75, 0.65, 0.55, 0.46, 0.39};

  // OTP shadow maps (Sec. 5.1.1).
  /** @brief Length of the OTP_PWM_GRAD shadow table (Sec. 5.1.1). */
  static constexpr int kOtpPwmGradN = 16;
  /** @brief OTP_PWM_GRAD shadow values (Sec. 5.1.1), by index. */
  static constexpr double kOtpPwmGrad[kOtpPwmGradN] = {14.0, 16.0, 18.0, 21.0, 24.0, 27.0,
                                                      31.0, 35.0, 40.0, 46.0, 52.0, 59.0,
                                                      67.0, 77.0, 88.0, 100.0};
  /** @brief Length of the OTP_TPWMTHRS shadow table (Sec. 5.1.1). */
  static constexpr int kOtpTpwmthrsN = 8;
  /** @brief OTP_TPWMTHRS shadow values (Sec. 5.1.1), by index. */
  static constexpr double kOtpTpwmthrs[kOtpTpwmthrsN] = {0.0,    200.0,  300.0,  400.0, 500.0,  800.0,  1200.0, 4000.0};
  /** @brief Length of the OTP_IHOLD shadow table (Sec. 5.1.1). */
  static constexpr int kOtpIholdN = 4;
  /** @brief OTP_IHOLD shadow values (Sec. 5.1.1), by index. */
  static constexpr int kOtpIhold[kOtpIholdN] = {16, 2, 8, 24};
  /** @brief Length of the OTP_IHOLDDELAY shadow table (Sec. 5.1.1). */
  static constexpr int kOtpIholdDelayN = 4;
  /** @brief OTP_IHOLDDELAY shadow values (Sec. 5.1.1), by index. */
  static constexpr int kOtpIholdDelay[kOtpIholdDelayN] = {1, 2, 4, 8};

  // CHOPCONF helpers.
  static constexpr int kBlankClocks[4] = {16, 24, 32, 40};          /**< @brief TBL 0..3 blank-time comparator clocks (Eq.6). */
  static constexpr int kPwmDiv[4] = {1024, 683, 512, 410};           /**< @brief PWM_FREQ 0..3 divisor values (Eq.7). */

  // ------------------------------------------------------------ Eq.1/Eq.3
  /**
   * @brief Full-scale sense voltage for a vsense setting.
   * @param[in] vsense 0 = VSRTL (0.325 V), nonzero = VSRTH (0.180 V).
   * @return The VFS table value, in volts.
   */
  [[nodiscard]] static constexpr double fullscale_v(int vsense) noexcept {
    return (vsense != 0) ? kVfsHighV : kVfsLowV;
  }

  /**
   * @brief Eq.3 analog scaling of the full-scale sense voltage.
   * @param[in] vfs_v unscaled full-scale sense voltage, in volts.
   * @param[in] vvref_v actual VREF, in volts.
   * @return VFS * VREF / 2.5V, in volts.
   */
  [[nodiscard]] static constexpr double scaled_fullscale_v(double vfs_v, double vvref_v) noexcept {
    return vfs_v * vvref_v / kVrefNomV;
  }

  /**
   * @brief Standalone full-scale run current (Eq.1: CS=31 equivalent).
   * @param[in] rsense_ohm sense resistor, in ohms.
   * @param[in] vvref_v VREF, in volts.
   * @param[in] vsense 0 = VSRTL, nonzero = VSRTH.
   * @return IRMS = scaled VFS / (RSENSE + 30 mOhm) / sqrt(2), in
   *         amperes; 0.0 when RSENSE or VREF is non-positive.
   */
  [[nodiscard]] static constexpr double irms_standalone_a(double rsense_ohm, double vvref_v, int vsense) noexcept {
    if (!(rsense_ohm > 0.0) || !(vvref_v > 0.0)) {
      return 0.0;
    }
    return scaled_fullscale_v(fullscale_v(vsense), vvref_v) / (rsense_ohm + kRintOhm) / kSqrt2;
  }

  // ----------------------------------------------------------------- Eq.2
  /**
   * @brief Scaled run/hold current for a current-scale setting (Eq.2).
   * @param[in] cs current-scale setting 0..31 (IRUN or IHOLD).
   * @param[in] rsense_ohm sense resistor, in ohms.
   * @param[in] vfs_v unscaled full-scale sense voltage, in volts.
   * @param[in] vvref_v actual VREF, in volts.
   * @return IRMS = (CS+1)/32 * scaled VFS / (RSENSE + 30 mOhm) /
   *         sqrt(2), in amperes; 0.0 for any invalid input.
   */
  [[nodiscard]] static constexpr double irms_cs_a(int cs, double rsense_ohm, double vfs_v, double vvref_v) noexcept {
    if (cs < 0 || cs > 31 || !(rsense_ohm > 0.0) || !(vfs_v > 0.0) || !(vvref_v > 0.0)) {
      return 0.0;
    }
    return (static_cast<double>(cs) + 1.0) / 32.0 * scaled_fullscale_v(vfs_v, vvref_v) / (rsense_ohm + kRintOhm) / kSqrt2;
  }

  /**
   * @brief Sine peak from RMS.
   * @param[in] irms_a RMS current, in amperes.
   * @return IRMS * sqrt(2), in amperes.
   */
  [[nodiscard]] static constexpr double ipeak_of_rms_a(double irms_a) noexcept {
    return irms_a * kSqrt2;
  }

  /**
   * @brief RMS from sine peak.
   * @param[in] ipeak_a peak current, in amperes.
   * @return IPEAK / sqrt(2), in amperes.
   */
  [[nodiscard]] static constexpr double irms_of_peak_a(double ipeak_a) noexcept {
    return ipeak_a / kSqrt2;
  }

  // Exact sense value for a target RMS (invert Eq.1); <= 0 when infeasible.
  /**
   * @brief Exact RSENSE for a target run current (inverts Eq.1).
   * @param[in] irms_a target run RMS current, in amperes.
   * @param[in] vvref_v actual VREF, in volts.
   * @param[in] vsense 0 = VSRTL, nonzero = VSRTH.
   * @return The exact sense resistor in ohms (subtract the internal
   *         30 mOhm); -1.0 for non-positive inputs. Non-positive
   *         results mean the target is out of reach at this VREF.
   */
  [[nodiscard]] static constexpr double rsense_for_irms_ohm(double irms_a, double vvref_v, int vsense) noexcept {
    if (!(irms_a > 0.0) || !(vvref_v > 0.0)) {
      return -1.0;
    }
    return scaled_fullscale_v(fullscale_v(vsense), vvref_v) / (irms_a * kSqrt2) - kRintOhm;
  }

  // Smallest CS 0..31 reaching the target, -1 when even CS=31 falls short.
  /**
   * @brief Smallest CS reaching a target run current (inverts Eq.2).
   * @param[in] irms_a target run RMS current, in amperes.
   * @param[in] rsense_ohm sense resistor, in ohms.
   * @param[in] vfs_v unscaled full-scale sense voltage, in volts.
   * @param[in] vvref_v actual VREF, in volts.
   * @return The smallest CS 0..31 whose current reaches the target
   *         (0 when the target is below CS=0); -1 for invalid inputs
   *         or when even CS=31 falls short.
   */
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

  /**
   * @brief RSENSE dissipation at the run current.
   * @param[in] irms_a run RMS current, in amperes.
   * @param[in] rsense_ohm sense resistor, in ohms.
   * @return IRMS^2 * RSENSE per resistor, in watts.
   */
  [[nodiscard]] static constexpr double sense_power_w(double irms_a, double rsense_ohm) noexcept {
    return irms_a * irms_a * rsense_ohm;
  }

  // ----------------------------------------------------------------- Eq.4
  /**
   * @brief Peak current from an RREF in RDSon mode (Eq.4).
   * @param[in] rref_ohm RREF resistor, in ohms.
   * @param[in] vsense 0 = VSRTL, nonzero = VSRTH.
   * @return IPEAK = 3000*5V/(RREF+700Ohm) + 0.12A (rescaled by VSRTH
   *         when vsense is set), in amperes; 0.0 for non-positive RREF.
   */
  [[nodiscard]] static constexpr double rref_ipeak_a(double rref_ohm, int vsense) noexcept {
    if (!(rref_ohm > 0.0)) {
      return 0.0;
    }
    const double peak = kRrefGain * kRrefVddV / (rref_ohm + kRrefRinOhm) + kRrefOffsetA;
    return (vsense != 0) ? peak * kVfsHighV / kVfsLowV : peak;
  }

  /**
   * @brief RREF for a target peak current (inverts Eq.4).
   * @param[in] ipeak_a target peak current, in amperes.
   * @param[in] vsense 0 = VSRTL, nonzero = VSRTH.
   * @return The exact RREF, in ohms; -1.0 when the target is at or
   *         below the 0.12 A model offset (unreachable).
   */
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
  /**
   * @brief Nearest Ch.10 RREF table row for a target peak current.
   * @param[in] ipeak_a target peak current, in amperes.
   * @return The kRrefPeakA row index closest to the target; -1 when
   *         the table is empty (never with the shipped table).
   */
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
  /**
   * @brief Slow-decay time for a TOFF setting (Eq.5).
   * @param[in] toff TOFF register setting 0..15.
   * @param[in] fclk_hz clock frequency, in hertz.
   * @return tOFF = (24 + 32*TOFF) / fCLK, in seconds; -1.0 for an
   *         invalid setting or clock.
   */
  [[nodiscard]] static constexpr double toff_time_s(int toff, double fclk_hz) noexcept {
    if (toff < 0 || toff > 15 || !(fclk_hz > 0.0)) {
      return -1.0;
    }
    return (24.0 + 32.0 * static_cast<double>(toff)) / fclk_hz;
  }

  /**
   * @brief TOFF setting for a wanted slow-decay time (inverts Eq.5).
   * @param[in] toff_s wanted slow-decay time, in seconds.
   * @param[in] fclk_hz clock frequency, in hertz.
   * @return (tOFF*fCLK - 24) / 32, fractional TOFF (round to a
   *         register value at the call site); -1.0 for invalid inputs.
   */
  [[nodiscard]] static constexpr double toff_for_time(double toff_s, double fclk_hz) noexcept {
    if (!(toff_s > 0.0) || !(fclk_hz > 0.0)) {
      return -1.0;
    }
    return (toff_s * fclk_hz - 24.0) / 32.0;
  }

  // TOFF setting from a chopper-frequency target: two slow-decay phases
  // make up `slow_frac` of the cycle (datasheet example: 0.5 at 25kHz).
  /**
   * @brief TOFF setting from a chopper-frequency target.
   *
   * Two slow-decay phases make up slow_frac of the cycle (datasheet
   * example: 0.5 at 25 kHz).
   *
   * @param[in] fchop_hz target chopper frequency, in hertz.
   * @param[in] slow_frac fraction of the cycle spent in slow decay
   *            (0..1).
   * @param[in] fclk_hz clock frequency, in hertz.
   * @return Fractional TOFF for that target; -1.0 for invalid inputs.
   */
  [[nodiscard]] static constexpr double toff_for_chop_freq(double fchop_hz, double slow_frac, double fclk_hz) noexcept {
    if (!(fchop_hz > 0.0) || !(slow_frac > 0.0) || !(fclk_hz > 0.0)) {
      return -1.0;
    }
    return toff_for_time(slow_frac / (2.0 * fchop_hz), fclk_hz);
  }

  /**
   * @brief Blank-time comparator clocks for a TBL setting (Eq.6 table).
   * @param[in] tbl TBL setting 0..3.
   * @return 16 / 24 / 32 / 40 clocks; -1 for any other value.
   */
  [[nodiscard]] static constexpr int blank_clocks(int tbl) noexcept {
    return (tbl >= 0 && tbl < 4) ? kBlankClocks[tbl] : -1;
  }

  /**
   * @brief Blank time for a TBL setting (Eq.6).
   * @param[in] tbl TBL setting 0..3.
   * @param[in] fclk_hz clock frequency, in hertz.
   * @return tBLANK = kBlankClocks[TBL] / fCLK, in seconds; -1.0 for
   *         invalid inputs.
   */
  [[nodiscard]] static constexpr double blank_time_s(int tbl, double fclk_hz) noexcept {
    const int n = blank_clocks(tbl);
    if (n < 0 || !(fclk_hz > 0.0)) {
      return -1.0;
    }
    return static_cast<double>(n) / fclk_hz;
  }

  // ----------------------------------------------------------------- Eq.7
  /**
   * @brief PWM frequency for a PWM_FREQ setting (Eq.7, Table 6.1).
   * @param[in] sel PWM_FREQ setting 0..3.
   * @param[in] fclk_hz clock frequency, in hertz.
   * @return fPWM = 2*fCLK / {1024, 683, 512, 410}, in hertz; -1.0 for
   *         invalid inputs.
   */
  [[nodiscard]] static constexpr double pwm_freq_hz(int sel, double fclk_hz) noexcept {
    if (sel < 0 || sel > 3 || !(fclk_hz > 0.0)) {
      return -1.0;
    }
    return 2.0 * fclk_hz / static_cast<double>(kPwmDiv[sel]);
  }

  // ------------------------------------------------------- HEND / HSTRT
  // Register 0..2 -> -3..-1, 3 -> 0, 4..15 -> 1..12 (offset by 3).
  /**
   * @brief Effective hysteresis offset from a HEND register value.
   * @param[in] hend_reg HEND register value 0..15.
   * @return Register minus 3 (-3..12); -99 for an out-of-range
   *         register value.
   */
  [[nodiscard]] static constexpr int hend_effective(int hend_reg) noexcept {
    if (hend_reg < 0 || hend_reg > 15) {
      return -99;
    }
    return hend_reg - 3;
  }

  // Register 0..7 -> start offset 1..8 added to HEND.
  /**
   * @brief Effective hysteresis start offset from a HSTRT register value.
   * @param[in] hstrt_reg HSTRT register value 0..7.
   * @return Register plus 1 (1..8); -99 for an out-of-range register
   *         value.
   */
  [[nodiscard]] static constexpr int hstrt_effective(int hstrt_reg) noexcept {
    if (hstrt_reg < 0 || hstrt_reg > 7) {
      return -99;
    }
    return hstrt_reg + 1;
  }

  /**
   * @brief Decode and validate a (HEND, HSTRT) register pair.
   * @param[in] hend_reg HEND register value 0..15.
   * @param[in] hstrt_reg HSTRT register value 0..7.
   * @return Effective offsets, their span, and the ok verdict
   *         (span <= 16, Ch.7 attention box); -99 sentinels on
   *         out-of-range register values.
   */
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
  /**
   * @brief Microsteps per fullstep for a MRES code.
   * @param[in] mres MRES code 0..8.
   * @return 256 >> mres (256 native down to 1 fullstep); -1 for any
   *         other code.
   */
  [[nodiscard]] static constexpr int usteps_for_mres(int mres) noexcept {
    if (mres < 0 || mres > 8) {
      return -1;
    }
    return static_cast<int>(256u >> static_cast<unsigned>(mres));
  }

  /**
   * @brief MRES code for a microstep count.
   * @param[in] usteps microsteps per fullstep (a power of two 1..256).
   * @return The matching MRES code 0..8; -1 when no code yields
   *         usteps.
   */
  [[nodiscard]] static constexpr int mres_for_usteps(int usteps) noexcept {
    for (int m = 0; m <= 8; ++m) {
      if (usteps_for_mres(m) == usteps) {
        return m;
      }
    }
    return -1;
  }

  // Standalone pin decode (Sec. 3.4): index is MS2*2+MS1.
  /**
   * @brief Standalone MS1/MS2 pin decode (Sec. 3.4).
   * @param[in] is_220x true for TMC2202/TMC2208 (QFN28) rows, false
   *            for TMC2224 rows.
   * @param[in] ms1 MS1 pin level (0 or 1).
   * @param[in] ms2 MS2 pin level (0 or 1).
   * @return Microsteps per fullstep from the pin table (index
   *         MS2*2+MS1); -1 for any non-binary pin value.
   */
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
  /**
   * @brief VACTUAL register to 1/256-step rate (Eq.13).
   * @param[in] vactual VACTUAL register value.
   * @param[in] fclk_hz clock frequency, in hertz.
   * @return v = VACTUAL * fCLK / 2^24, in microsteps per second;
   *         0.0 for a non-positive clock.
   */
  [[nodiscard]] static constexpr double vactual_to_usteps_hz(double vactual, double fclk_hz) noexcept {
    if (!(fclk_hz > 0.0)) {
      return 0.0;
    }
    return vactual * fclk_hz / kVactScale;
  }

  /**
   * @brief 1/256-step rate to the VACTUAL register (inverts Eq.13).
   * @param[in] v_hz microstep rate, in hertz.
   * @param[in] fclk_hz clock frequency, in hertz.
   * @return VACTUAL = v * 2^24 / fCLK; 0.0 for a non-positive clock.
   */
  [[nodiscard]] static constexpr double usteps_hz_to_vactual(double v_hz, double fclk_hz) noexcept {
    if (!(fclk_hz > 0.0)) {
      return 0.0;
    }
    return v_hz * kVactScale / fclk_hz;
  }

  /**
   * @brief TSTEP register to 1/256-step rate (Eq.14).
   * @param[in] tstep TSTEP register value (> 0).
   * @param[in] fclk_hz clock frequency, in hertz.
   * @return v = fCLK / TSTEP, in microsteps per second; 0.0 for
   *         invalid inputs.
   */
  [[nodiscard]] static constexpr double tstep_to_usteps_hz(double tstep, double fclk_hz) noexcept {
    if (!(tstep > 0.0) || !(fclk_hz > 0.0)) {
      return 0.0;
    }
    return fclk_hz / tstep;
  }

  /**
   * @brief 1/256-step rate to the TSTEP register (inverts Eq.14).
   * @param[in] v_hz microstep rate, in hertz.
   * @param[in] fclk_hz clock frequency, in hertz.
   * @return TSTEP = fCLK / v; 0.0 for invalid inputs.
   */
  [[nodiscard]] static constexpr double usteps_hz_to_tstep(double v_hz, double fclk_hz) noexcept {
    if (!(v_hz > 0.0) || !(fclk_hz > 0.0)) {
      return 0.0;
    }
    return fclk_hz / v_hz;
  }

  // Motor shaft speed (rps, e.g. from --vel 28rpm) to the 1/256-step
  // rate the TSTEP/VACTUAL domain counts in.
  /**
   * @brief Shaft speed to the 1/256-step rate of the
   *        TSTEP/VACTUAL domain.
   * @param[in] rps shaft speed, in revolutions per second.
   * @param[in] fullsteps_per_rev motor geometry (200 for the classic
   *            1.8-degree motor).
   * @return rps * fullsteps_per_rev * 256, in microsteps per second;
   *         0.0 for invalid inputs.
   */
  [[nodiscard]] static constexpr double motor_rps_to_usteps_hz(double rps, int fullsteps_per_rev) noexcept {
    if (!(rps >= 0.0) || fullsteps_per_rev <= 0) {
      return 0.0;
    }
    return rps * static_cast<double>(fullsteps_per_rev) *
           static_cast<double>(kUstepsPerFullstep);
  }

  /**
   * @brief Decode a microstep rate into fullstep/shaft rates.
   * @param[in] usteps_hz 1/256-step rate, in hertz.
   * @param[in] fullsteps_per_rev motor geometry (200 for the classic
   *            1.8-degree motor).
   * @return Microstep and fullstep rates plus shaft rps and rpm;
   *         all-zero struct for invalid inputs.
   */
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
  /**
   * @brief StealthChop lower current limit (Eq.8, Sec. 6.3.1).
   * @param[in] tblank_s blank time, in seconds (Eq.6).
   * @param[in] fpwm_hz PWM frequency, in hertz (Eq.7).
   * @param[in] vm_v motor supply voltage, in volts.
   * @param[in] rcoil_ohm coil resistance, in ohms.
   * @return ILowerLimit = tBLANK*fPWM*VM/RCOIL, in amperes (golden:
   *         24 clk, 2/1024, 24 V, 5 Ohm -> 225 mA); -1.0 for invalid
   *         inputs.
   */
  [[nodiscard]] static constexpr double stealth_lower_limit_a(double tblank_s, double fpwm_hz, double vm_v, double rcoil_ohm) noexcept {
    if (!(tblank_s > 0.0) || !(fpwm_hz > 0.0) || !(vm_v >= 0.0) || !(rcoil_ohm > 0.0)) {
      return -1.0;
    }
    return tblank_s * fpwm_hz * vm_v / rcoil_ohm;
  }

  /**
   * @brief PWM amplitude autoscale seed (Eq.9, Sec. 6.4).
   * @param[in] rcoil_ohm coil resistance, in ohms.
   * @param[in] icoil_a coil current, in amperes.
   * @param[in] vm_v motor supply voltage, in volts.
   * @return PWM_AMPL = 374*RCOIL*ICOIL/VM, dimensionless; -1.0 for
   *         invalid inputs.
   */
  [[nodiscard]] static constexpr double pwm_ampl_seed(double rcoil_ohm, double icoil_a, double vm_v) noexcept {
    if (!(rcoil_ohm > 0.0) || !(icoil_a > 0.0) || !(vm_v > 0.0)) {
      return -1.0;
    }
    return 374.0 * rcoil_ohm * icoil_a / vm_v;
  }

  // ------------------------------------------------------ Eq.10/Eq.12
  /**
   * @brief PWM_SCALE_SUM offset plus back-EMF gradient (Eq.10, Sec. 6.4).
   * @param[in] pwm_ofs PWM_OFS autoscale offset, dimensionless.
   * @param[in] cs_actual actual current scale 0..31.
   * @param[in] pwm_grad PWM_GRAD value, dimensionless.
   * @param[in] tstep current TSTEP value (> 0).
   * @return PWM_OFS*(CS+1)/32 + PWM_GRAD*256/TSTEP, dimensionless;
   *         -1.0 for invalid inputs.
   */
  [[nodiscard]] static constexpr double pwm_scale_sum(double pwm_ofs, int cs_actual, double pwm_grad, double tstep) noexcept {
    if (cs_actual < 0 || cs_actual > 31 || !(tstep > 0.0)) {
      return -1.0;
    }
    return pwm_ofs * (static_cast<double>(cs_actual) + 1.0) / 32.0 + pwm_grad * 256.0 / tstep;
  }

  /**
   * @brief Motor back-EMF constant from holding torque (Eq.12, Sec. 6.4).
   * @param[in] torque_nm holding torque, in newton-meters.
   * @param[in] icoilnom_a nominal coil current, in amperes.
   * @return CBEMF = HoldingTorque / (2*ICOILNOM), in V/(rad/s);
   *         -1.0 for invalid inputs.
   */
  [[nodiscard]] static constexpr double backemf_const_v_per_rads(double torque_nm, double icoilnom_a) noexcept {
    if (!(torque_nm > 0.0) || !(icoilnom_a > 0.0)) {
      return -1.0;
    }
    return torque_nm / (2.0 * icoilnom_a);
  }

  // ---------------------------------------------------------------- Eq.11
  /**
   * @brief PWM_GRAD seed from the back-EMF constant (Eq.11, Sec. 6.4).
   * @param[in] cbemf back-EMF constant, in V/(rad/s) (Eq.12).
   * @param[in] fclk_hz clock frequency, in hertz.
   * @param[in] vm_v motor supply voltage, in volts.
   * @param[in] mspr microsteps per revolution (default 51200 =
   *            256 * 200).
   * @return PWM_GRAD = CBEMF*2*pi*fCLK*1.46/(VM*MSPR), dimensionless;
   *         -1.0 for invalid inputs.
   */
  [[nodiscard]] static constexpr double pwm_grad_seed(double cbemf, double fclk_hz, double vm_v, double mspr = 51200.0) noexcept {
    if (!(cbemf > 0.0) || !(fclk_hz > 0.0) || !(vm_v > 0.0) || !(mspr > 0.0)) {
      return -1.0;
    }
    return cbemf * 2.0 * 3.141592653589793 * fclk_hz * 1.46 / (vm_v * mspr);
  }

  // Nearest OTP table entries (Sec. 5.1.1); -1 on empty/out-of-range input.
  /**
   * @brief Nearest OTP_PWM_GRAD shadow row (Sec. 5.1.1).
   * @param[in] value the wanted PWM_GRAD, dimensionless.
   * @return The kOtpPwmGrad row index closest to value; -1 for
   *         negative input.
   */
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

  /**
   * @brief Nearest OTP_TPWMTHRS shadow row (Sec. 5.1.1).
   * @param[in] value the wanted TPWMTHRS, dimensionless.
   * @return The kOtpTpwmthrs row index closest to value; -1 for
   *         negative input.
   */
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

  /**
   * @brief Exact OTP_IHOLD shadow row (Sec. 5.1.1).
   * @param[in] ihold the wanted IHOLD register value.
   * @return The kOtpIhold row index holding exactly ihold; -1 when
   *         absent.
   */
  [[nodiscard]] static constexpr int otp_ihold_code(int ihold) noexcept {
    for (int i = 0; i < kOtpIholdN; ++i) {
      if (kOtpIhold[i] == ihold) {
        return i;
      }
    }
    return -1;
  }

  /**
   * @brief Exact OTP_IHOLDDELAY shadow row (Sec. 5.1.1).
   * @param[in] delay the wanted IHOLDDELAY register value.
   * @return The kOtpIholdDelay row index holding exactly delay; -1
   *         when absent.
   */
  [[nodiscard]] static constexpr int otp_iholddelay_code(int delay) noexcept {
    for (int i = 0; i < kOtpIholdDelayN; ++i) {
      if (kOtpIholdDelay[i] == delay) {
        return i;
      }
    }
    return -1;
  }

  // -------------------------------------------------------------- Eq.15
  /**
   * @brief Standstill detection time (Ch.11.3).
   * @param[in] fclk_hz clock frequency, in hertz.
   * @return 2^20 / fCLK, in seconds; -1.0 for a non-positive clock.
   */
  [[nodiscard]] static constexpr double standstill_time_s(double fclk_hz) noexcept {
    if (!(fclk_hz > 0.0)) {
      return -1.0;
    }
    return kStepClocks / fclk_hz;
  }

  /**
   * @brief TPOWERDOWN delay (Eq.15, Ch.5.2).
   * @param[in] tpowerdown TPOWERDOWN register value 0..255.
   * @param[in] fclk_hz clock frequency, in hertz.
   * @return tPW = TPOWERDOWN * 2^18 / fCLK, in seconds; -1.0 for
   *         invalid inputs (golden: TP=20 @12MHz -> 0.437 s).
   */
  [[nodiscard]] static constexpr double tpowerdown_time_s(int tpowerdown, double fclk_hz) noexcept {
    if (tpowerdown < 0 || tpowerdown > 255 || !(fclk_hz > 0.0)) {
      return -1.0;
    }
    return static_cast<double>(tpowerdown) * kTimingClocks / fclk_hz;
  }

  /**
   * @brief Per-step IHOLDDELAY ramp unit (Eq.15, Ch.5.2).
   * @param[in] iholddelay IHOLDDELAY register value 0..15.
   * @param[in] fclk_hz clock frequency, in hertz.
   * @return IHOLDDELAY * 2^18 / fCLK, in seconds per ramp step; -1.0
   *         for invalid inputs.
   */
  [[nodiscard]] static constexpr double iholddelay_step_s(int iholddelay, double fclk_hz) noexcept {
    if (iholddelay < 0 || iholddelay > 15 || !(fclk_hz > 0.0)) {
      return -1.0;
    }
    return static_cast<double>(iholddelay) * kTimingClocks / fclk_hz;
  }

  /**
   * @brief Full standby timing chain for a (TPOWERDOWN, IRUN, IHOLD,
   *        IHOLDDELAY) setting.
   *
   * @param[in] tpowerdown TPOWERDOWN register value 0..255.
   * @param[in] irun run current scale 0..31.
   * @param[in] ihold hold current scale 0..31.
   * @param[in] iholddelay IHOLDDELAY register value 0..15.
   * @param[in] fclk_hz clock frequency, in hertz.
   * @return Standstill detection time, TPOWERDOWN delay, the
   *         (IRUN-IHOLD)-step decay ramp, and their total, in seconds;
   *         partial-zero struct for invalid inputs.
   */
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
  /**
   * @brief Bridge conduction-loss floor (Eq.16, NOT a datasheet
   *        equation).
   *
   * Standard bridge estimate 2*IRMS^2*(RonHs + RonLs) over both coils
   * at sine RMS — a conduction floor only, switching loss excluded
   * (the 1 A/24 V row of Table 18.1 reports 2.5 W including switching
   * loss).
   *
   * @param[in] irms_a run RMS current, in amperes.
   * @return The conduction floor, in watts; -1.0 for negative input.
   */
  [[nodiscard]] static constexpr double conduction_loss_w(double irms_a) noexcept {
    if (!(irms_a >= 0.0)) {
      return -1.0;
    }
    return 2.0 * irms_a * irms_a * (kRonHsOhm + kRonLsOhm);
  }

  /**
   * @brief Thermal check for one dissipation (TJ vs the 125 C limit).
   * @param[in] pd_w dissipated power, in watts (measured, or the
   *            Eq.16 conduction-floor estimate).
   * @param[in] ta_c ambient temperature, in degrees C.
   * @param[in] rth_kpw thermal resistance, in K/W (kRthJaKpw or
   *            kRthJcKpw plus heatsink).
   * @return Rise, junction temperature, and the ok verdict; partial
   *         zero struct for invalid inputs.
   */
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
  /**
   * @brief Overtemperature shutdown threshold for an OTTRIM code.
   * @param[in] ottrim OTTRIM code 0..3 (FACTORY_CONF, Sec. 5.1).
   * @return OT threshold, in degrees C (143/150/150/157); -1.0 for any
   *         other code.
   */
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

  /**
   * @brief Overtemperature prewarning threshold for an OTTRIM code.
   * @param[in] ottrim OTTRIM code 0..3 (FACTORY_CONF, Sec. 5.1).
   * @return OTPW threshold, in degrees C (120/120/143/143); -1.0 for
   *         any other code.
   */
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
  /**
   * @brief Maximum UART baud rate for a clock (Ch.4).
   * @param[in] fclk_hz clock frequency, in hertz.
   * @return fCLK / 16, in baud; -1.0 for a non-positive clock.
   */
  [[nodiscard]] static constexpr double uart_baud_max(double fclk_hz) noexcept {
    if (!(fclk_hz > 0.0)) {
      return -1.0;
    }
    return fclk_hz / 16.0;
  }

  // SENDDELAY code (NODECONF 11..8) -> reply delay in bit times.
  /**
   * @brief Reply delay for a SENDDELAY code (NODECONF 11..8, Ch.4).
   * @param[in] code SENDDELAY code 0..15.
   * @return Reply delay in bit times: 8 for codes 0..1, else
   *         (code|1)*8 (8..120); -1 for any other code.
   */
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
  /**
   * @brief CRC8-ATM single-byte step (Eq.17, Sec. 4.2 C reference).
   * @param[in] crc running CRC state (init 0), low 8 bits used.
   * @param[in] byte the next data byte.
   * @return The updated CRC state (polynomial x^8+x^2+x+1 = 0x07).
   */
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
  /**
   * @brief Full run/hold current sizing for one
   *        (RSENSE, VREF, vsense) choice (Eq.1/Eq.2/Eq.3).
   *
   * @param[in] rsense_ohm sense resistor, in ohms.
   * @param[in] irun run current scale 0..31.
   * @param[in] ihold hold current scale 0..31.
   * @param[in] vvref_v actual VREF, in volts.
   * @param[in] vsense 0 = VSRTL, 1 = VSRTH.
   * @return Scaled VFS, run/hold RMS and peak currents, sense drop
   *         and dissipation, and the peak_ok / rms_ok verdicts against
   *         the part limits; all-zero struct for invalid inputs.
   */
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
