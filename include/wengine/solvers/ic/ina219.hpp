#pragma once

// INA219 zero-drift bidirectional current/power monitor design solver.
//
// Reference: Texas Instruments INA219 datasheet SBOS448G (August 2008,
// revised December 2015), sections 8.5 (Programming) and 8.6 (Register
// Maps), plus the section 9.2.2 worked example.
//
// Covered equations (SI base V / A / Ohm / W inside; register codes are
// plain ints, never physical quantities):
//   Eq.1  Cal = trunc(0.04096 / (Current_LSB * RSHUNT))      (FS0 forced 0)
//   Eq.2  Current_LSB(min) = MaxExpectedCurrent / 2^15
//   Eq.3  Power_LSB = 20 * Current_LSB
//   Eq.4  CurrentReg = ShuntReg * Cal / 4096;  I = CurrentReg * Current_LSB
//   Eq.5  PowerReg = CurrentReg * BusShifted / 5000; P = PowerReg * Power_LSB
//   Eq.6  Cal(corr) = trunc(Cal * I_meas / I_ina219)          (system trim)
//   Shunt: VSHUNT = ShuntReg * 10uV (LSB fixed, all PGA settings)
//   Bus:   VBUS = (BusRaw >> 3) * 4mV (bits D2:D0 are CNVR/OVF, Sec. 8.6.3.2)
//   PGA:   +/-40/80/160/320 mV for /1, /2, /4, /8 (Table 4)
//   BRNG:  16 V (0) or 32 V (1); absolute bus maximum is 26 V (Sec. 7.1)
//   ADC:   Table 5 conversion times (9..12 bit, 1..128-sample averaging)
//   CFG:   reset 399Fh: BRNG=32V, PGA /8, 12-bit shunt+bus, cont. both
//
// Worked goldens (section 9.2.2 example, re-derived in decimal here):
//   IMAX=15A -> LSB(min) = 15/32768 = 457.763671875uA (ds: 457.78uA)
//   LSB=1mA, RSHUNT=2mOhm -> Cal = trunc(0.04096/2e-6) = 20480 (5000h)
//   10A load -> VSHUNT=20mV -> ShuntReg=2000 (07D0h), PGA /1 (40mV FS)
//   VBUS=11.98V -> shifted=2995 (BB3h), raw=23960 (5D98h), BRNG=16V
//   CurrentReg = 2000*20480/4096 = 10000 (2710h) -> 10.0A
//   PowerReg = 10000*2995/5000 = 5990 (1766h); PowerLSB=20mW -> 119.8W
//
// Notes: binary doubles cannot represent decimals like 0.04096 exactly, so
// Eq.1/Eq.6 apply truncation with a 1e-9 relative guard: decimal-exact
// quotients (e.g. the 20480 golden) survive FP dust while genuine
// fractions still truncate per the datasheet. Volt-to-code encodes use
// round-to-nearest (quantization choice, not a datasheet equation).
//
// All functions are pure, header-only, constexpr + noexcept.
// Input validation (ranges, > 0, ...) is the caller's job (CLI layer).

namespace wengine::solvers::ic {

struct Ina219Calibration {
  double lsb_min_a{0.0};
  double lsb_used_a{0.0};
  unsigned cal{0};
  double power_lsb_w{0.0};
  double max_current_a{0.0};
  double max_power_w{0.0};
  int pga_idx{0}; // 0..3 -> /1, /2, /4, /8 (Table 4); -1 = over range
  int brng{0};    // 0 = 16V, 1 = 32V; -1 = over range
  double vshunt_fs_v{0.0};
  bool shunt_ok{false};
  bool bus_ok{false};
  bool cal_ok{false};
  bool ok{false};
};

struct Ina219Readout {
  double vshunt_v{0.0};
  double vbus_v{0.0};
  double current_a{0.0};
  double power_w{0.0};
  int current_reg{0};
  int power_reg{0};
};

class Ina219Solver {
public:
  Ina219Solver() = delete;

  // Fixed part parameters (electrical tables + register maps).
  static constexpr double kShuntLsbV = 10e-6;
  static constexpr double kBusLsbV = 4e-3;
  static constexpr double kCalScale = 0.04096;
  static constexpr double kPowerRatio = 20.0;
  static constexpr double kCurDiv = 4096.0;
  static constexpr double kPowDiv = 5000.0;
  static constexpr double kCodeFull = 32768.0; // 2^15 (Eq.2 divisor)
  static constexpr double kVbusAbsMaxV = 26.0; // Sec. 7.1 absolute maximum
  static constexpr double kVsMinV = 3.0;
  static constexpr double kVsMaxV = 5.5;
  static constexpr unsigned kConfigReset = 0x399Fu;
  static constexpr unsigned kCalMax = 0xFFFEu; // FS0 is void, always 0

  // Table 4 full-scale shunt ranges for PGA /1, /2, /4, /8.
  static constexpr double kPgaRangesV[4] = {40e-3, 80e-3, 160e-3, 320e-3};

  // Table 5 conversion times [us]: 9/10/11/12-bit, then 1/2/4/.../128 avg.
  static constexpr double kAdcTimeUs[12] = {84.0,   148.0,  276.0,  532.0,  532.0,  1060.0,
                                            2130.0, 4260.0, 8510.0, 17020.0, 34050.0, 68100.0};

  // ------------------------------------------------------------ Eq.2/Eq.1
  [[nodiscard]] static constexpr double min_lsb_a(double imax_a) noexcept {
    return imax_a / kCodeFull;
  }

  // Datasheet trunc() with a 1e-9 relative guard against binary FP dust.
  [[nodiscard]] static constexpr unsigned trunc_cal(double x) noexcept {
    if (!(x > 0.0)) {
      return 0;
    }
    return static_cast<unsigned>(x + x * 1e-9);
  }

  [[nodiscard]] static constexpr unsigned cal_raw(double lsb_a, double rshunt_ohm) noexcept {
    if (!(lsb_a > 0.0) || !(rshunt_ohm > 0.0)) {
      return 0;
    }
    return trunc_cal(kCalScale / (lsb_a * rshunt_ohm));
  }

  // FS0 is void (always 0): the programmed value is even.
  [[nodiscard]] static constexpr unsigned cal(double lsb_a, double rshunt_ohm) noexcept {
    return static_cast<unsigned>(cal_raw(lsb_a, rshunt_ohm) & ~1u);
  }

  // ----------------------------------------------------------------- Eq.3
  [[nodiscard]] static constexpr double power_lsb_w(double lsb_a) noexcept {
    return kPowerRatio * lsb_a;
  }

  // ---------------------------------------------------------- shunt/bus
  [[nodiscard]] static constexpr double shunt_v(double current_a, double rshunt_ohm) noexcept {
    return current_a * rshunt_ohm;
  }

  [[nodiscard]] static constexpr long long llround_half_away(double x) noexcept {
    return x >= 0.0 ? static_cast<long long>(x + 0.5) : static_cast<long long>(x - 0.5);
  }

  [[nodiscard]] static constexpr int shunt_reg(double vshunt_v) noexcept {
    return static_cast<int>(llround_half_away(vshunt_v / kShuntLsbV));
  }

  [[nodiscard]] static constexpr double shunt_v_from_reg(int reg) noexcept {
    return static_cast<double>(reg) * kShuntLsbV;
  }

  // Smallest PGA index whose range covers |VSHUNT|; -1 above 320 mV.
  [[nodiscard]] static constexpr int pga_for_shunt(double vshunt_abs_v) noexcept {
    if (!(vshunt_abs_v >= 0.0)) {
      return -1;
    }
    for (int i = 0; i < 4; ++i) {
      if (vshunt_abs_v <= kPgaRangesV[i] * (1.0 + 1e-9)) {
        return i;
      }
    }
    return -1;
  }

  [[nodiscard]] static constexpr double pga_range_v(int idx) noexcept {
    return (idx >= 0 && idx < 4) ? kPgaRangesV[idx] : 0.0;
  }

  // 0 = 16V range, 1 = 32V range, -1 above the 32V ADC range.
  [[nodiscard]] static constexpr int brng_for_bus(double vbus_v) noexcept {
    if (!(vbus_v >= 0.0)) {
      return -1;
    }
    if (vbus_v <= 16.0 * (1.0 + 1e-9)) {
      return 0;
    }
    if (vbus_v <= 32.0 * (1.0 + 1e-9)) {
      return 1;
    }
    return -1;
  }

  [[nodiscard]] static constexpr int bus_shifted(double vbus_v) noexcept {
    return static_cast<int>(llround_half_away(vbus_v / kBusLsbV));
  }

  [[nodiscard]] static constexpr double bus_v_from_shifted(int shifted) noexcept {
    return static_cast<double>(shifted) * kBusLsbV;
  }

  [[nodiscard]] static constexpr int bus_raw_from_shifted(int shifted) noexcept {
    return shifted << 3;
  }

  [[nodiscard]] static constexpr int bus_shifted_from_raw(int raw) noexcept {
    return (raw >> 3) & 0x1FFF;
  }

  // ----------------------------------------------------------------- Eq.4
  [[nodiscard]] static constexpr int current_reg(int shunt_reg_value, unsigned cal_value) noexcept {
    return static_cast<int>(static_cast<long long>(shunt_reg_value) *
                            static_cast<long long>(cal_value) /
                            static_cast<long long>(kCurDiv));
  }

  [[nodiscard]] static constexpr double current_a(int current_reg_value, double lsb_a) noexcept {
    return static_cast<double>(current_reg_value) * lsb_a;
  }

  // ----------------------------------------------------------------- Eq.5
  [[nodiscard]] static constexpr int power_reg(int current_reg_value, int bus_shifted_value) noexcept {
    return static_cast<int>(static_cast<long long>(current_reg_value) *
                            static_cast<long long>(bus_shifted_value) /
                            static_cast<long long>(kPowDiv));
  }

  [[nodiscard]] static constexpr double power_w(int power_reg_value, double power_lsb) noexcept {
    return static_cast<double>(power_reg_value) * power_lsb;
  }

  [[nodiscard]] static constexpr double max_current_a(double lsb_a) noexcept {
    return 32767.0 * lsb_a;
  }

  [[nodiscard]] static constexpr double max_power_w(double power_lsb) noexcept {
    return 65535.0 * power_lsb;
  }

  // ----------------------------------------------------------------- Eq.6
  [[nodiscard]] static constexpr unsigned correct_cal(unsigned cal_value, double i_meas_a,
                                                      double i_ina_a) noexcept {
    if (!(i_ina_a > 0.0) || !(i_meas_a > 0.0)) {
      return 0;
    }
    return static_cast<unsigned>(
        trunc_cal(static_cast<double>(cal_value) * i_meas_a / i_ina_a) & ~1u);
  }

  // ------------------------------------------------------------ Table 5
  [[nodiscard]] static constexpr double adc_time_us_bits(int bits) noexcept {
    if (bits == 9) {
      return kAdcTimeUs[0];
    }
    if (bits == 10) {
      return kAdcTimeUs[1];
    }
    if (bits == 11) {
      return kAdcTimeUs[2];
    }
    if (bits == 12) {
      return kAdcTimeUs[3];
    }
    return -1.0;
  }

  [[nodiscard]] static constexpr double adc_time_us_avg(int avg) noexcept {
    if (avg == 1) {
      return kAdcTimeUs[4];
    }
    if (avg == 2) {
      return kAdcTimeUs[5];
    }
    if (avg == 4) {
      return kAdcTimeUs[6];
    }
    if (avg == 8) {
      return kAdcTimeUs[7];
    }
    if (avg == 16) {
      return kAdcTimeUs[8];
    }
    if (avg == 32) {
      return kAdcTimeUs[9];
    }
    if (avg == 64) {
      return kAdcTimeUs[10];
    }
    if (avg == 128) {
      return kAdcTimeUs[11];
    }
    return -1.0;
  }

  // ------------------------------------------------------- config word
  // brng: 0/1, pga: 0..3, badc/sadc: 4-bit Table 5 codes, mode: 0..7.
  [[nodiscard]] static constexpr unsigned config_word(int brng, int pga_idx, int badc_code,
                                                      int sadc_code, int mode) noexcept {
    return static_cast<unsigned>(((brng & 1) << 13) | ((pga_idx & 3) << 11) |
                                 ((badc_code & 15) << 7) | ((sadc_code & 15) << 3) | (mode & 7));
  }

  // ------------------------------------------------------------- design
  // lsb_req_a <= 0 selects the Eq.2 minimum (highest resolution).
  [[nodiscard]] static constexpr Ina219Calibration solve_calibration(double imax_a, double rshunt_ohm,
                                                                    double vbus_max_v,
                                                                    double lsb_req_a) noexcept {
    Ina219Calibration r{};
    if (!(imax_a > 0.0) || !(rshunt_ohm > 0.0) || !(vbus_max_v >= 0.0)) {
      return r;
    }
    r.lsb_min_a = min_lsb_a(imax_a);
    r.lsb_used_a = (lsb_req_a > 0.0) ? lsb_req_a : r.lsb_min_a;
    r.cal = cal(r.lsb_used_a, rshunt_ohm);
    r.power_lsb_w = power_lsb_w(r.lsb_used_a);
    r.max_current_a = max_current_a(r.lsb_used_a);
    r.max_power_w = max_power_w(r.power_lsb_w);
    r.vshunt_fs_v = shunt_v(imax_a, rshunt_ohm);
    const double vabs = (r.vshunt_fs_v >= 0.0) ? r.vshunt_fs_v : -r.vshunt_fs_v;
    r.pga_idx = pga_for_shunt(vabs);
    r.brng = brng_for_bus(vbus_max_v);
    r.shunt_ok = r.pga_idx >= 0;
    r.bus_ok = r.brng >= 0 && vbus_max_v <= kVbusAbsMaxV;
    r.cal_ok = r.cal > 0 && r.cal <= kCalMax;
    r.ok = r.shunt_ok && r.bus_ok && r.cal_ok;
    return r;
  }

  [[nodiscard]] static constexpr Ina219Readout solve_readout(int shunt_reg_value, int bus_raw,
                                                             unsigned cal_value,
                                                             double lsb_a) noexcept {
    Ina219Readout r{};
    r.vshunt_v = shunt_v_from_reg(shunt_reg_value);
    r.vbus_v = bus_v_from_shifted(bus_shifted_from_raw(bus_raw));
    r.current_reg = current_reg(shunt_reg_value, cal_value);
    r.current_a = current_a(r.current_reg, lsb_a);
    r.power_reg = power_reg(r.current_reg, bus_shifted_from_raw(bus_raw));
    r.power_w = power_w(r.power_reg, power_lsb_w(lsb_a));
    return r;
  }
};

static_assert(Ina219Solver::min_lsb_a(15.0) == 15.0 / 32768.0);
static_assert(Ina219Solver::cal(0.001, 0.002) == 20480u);
static_assert(Ina219Solver::cal_raw(0.001, 0.002) == 20480u);
static_assert(Ina219Solver::current_reg(2000, 20480u) == 10000);
static_assert(Ina219Solver::power_reg(10000, 2995) == 5990);
static_assert(Ina219Solver::shunt_reg(0.02) == 2000);
static_assert(Ina219Solver::bus_shifted(11.98) == 2995);
static_assert(Ina219Solver::bus_raw_from_shifted(2995) == 0x5D98);
static_assert(Ina219Solver::bus_shifted_from_raw(0x5D98) == 2995);
static_assert(Ina219Solver::pga_for_shunt(0.02) == 0);
static_assert(Ina219Solver::pga_for_shunt(0.04) == 0);
static_assert(Ina219Solver::pga_for_shunt(0.041) == 1);
static_assert(Ina219Solver::pga_for_shunt(0.5) == -1);
static_assert(Ina219Solver::brng_for_bus(12.0) == 0);
static_assert(Ina219Solver::brng_for_bus(24.0) == 1);
static_assert(Ina219Solver::brng_for_bus(33.0) == -1);
static_assert(Ina219Solver::adc_time_us_bits(12) == 532.0);
static_assert(Ina219Solver::adc_time_us_avg(128) == 68100.0);
static_assert(Ina219Solver::config_word(1, 3, 3, 3, 7) == 0x399Fu);
static_assert(Ina219Solver::correct_cal(20480u, 10.0, 10.0) == 20480u);

} // namespace wengine::solvers::ic
