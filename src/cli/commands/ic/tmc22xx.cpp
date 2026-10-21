#include "wengine/cli/ic/tmc22xx.hpp"
#include "wengine/cli/status.hpp"
#include "wengine/solvers/ic/tmc22xx.hpp"

#include <array>
#include <cstdio>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wengine::cli::ic {

namespace {

using S = solvers::ic::Tmc22xxSolver;

int usage_error(std::ostream &out, std::string_view msg) {
  return cli::usage_error(out, msg, Tmc22xxSpec::kUsage);
}

int unexpected_error(std::ostream &out, std::string_view tok) {
  return cli::unexpected_error(out, tok, Tmc22xxSpec::kUsage);
}

// --fclk option, internal 12MHz oscillator default (factory-trimmed, Ch.16).
core::Result<double> parse_fclk(ArgCursor &args) noexcept {
  auto raw = args.take_option({"--fclk"});
  if (!raw) {
    return core::Result<double>::success(S::kFclkIntHz);
  }
  auto p = parse_frequency(*raw);
  if (!p.ok()) {
    return core::Result<double>::failure("invalid --fclk value (e.g. 12MHz)");
  }
  if (!(p.value >= 4e6) || !(p.value <= 18e6)) {
    return core::Result<double>::failure("--fclk must be within 4..18MHz");
  }
  return core::Result<double>::success(p.value);
}

// --vel spells a step rate in Hz-family units (usteps/s at 1/256
// resolution) or a motor shaft speed in rpm/rps (converted via --fsc).
// The value always comes from the units-owned Frequency table; only the
// mode branches on the spelling, which is this op's contract.
bool vel_is_motor_speed(std::string_view text) noexcept {
  return text.ends_with("rpm") || text.ends_with("rps");
}

core::Result<int> parse_vsense(ArgCursor &args) noexcept {
  auto raw = args.take_option({"--vsense"});
  if (!raw) {
    return core::Result<int>::success(0);
  }
  auto p = parse_i64(*raw);
  if (!p.ok() || (p.value != 0 && p.value != 1)) {
    return core::Result<int>::failure("--vsense must be 0|1");
  }
  return core::Result<int>::success(static_cast<int>(p.value));
}

core::Result<int> parse_cs(ArgCursor &args, std::string_view name, int dflt) noexcept {
  auto raw = args.take_option({name});
  if (!raw) {
    return core::Result<int>::success(dflt);
  }
  auto p = parse_i64(*raw);
  if (!p.ok() || p.value < 0 || p.value > 31) {
    return core::Result<int>::failure("current scale must be 0..31");
  }
  return core::Result<int>::success(static_cast<int>(p.value));
}

int run_current(ArgCursor &args, std::ostream &out) {
  auto rs_raw = args.take_option({"--rsense"});
  if (!rs_raw) {
    return usage_error(out, "current needs --rsense");
  }
  auto rs = parse_resistance(*rs_raw);
  if (!rs.ok() || !(rs.value > 0.0)) {
    return usage_error(out, "invalid --rsense value (must be > 0)");
  }
  auto irun = parse_cs(args, "--irun", 31);
  if (!irun.ok()) {
    return usage_error(out, irun.error);
  }
  auto ihold = parse_cs(args, "--ihold", 16);
  if (!ihold.ok()) {
    return usage_error(out, ihold.error);
  }
  double vref = S::kVrefNomV;
  if (auto o = args.take_option({"--vref"})) {
    auto p = parse_voltage(*o);
    if (!p.ok() || !(p.value > 0.0)) {
      return usage_error(out, "invalid --vref value (must be > 0)");
    }
    vref = p.value;
  }
  auto vs = parse_vsense(args);
  if (!vs.ok()) {
    return usage_error(out, vs.error);
  }
  if (args.has_remaining()) {
    return unexpected_error(out, *args.first_remaining());
  }
  const auto r = S::solve_current(rs.value, irun.value, ihold.value, vref, vs.value);
  out << "RSENSE = " << rs.value << " Ohm (+30mOhm internal), VFS = " << r.vfs_v * 1e3
      << " mV (vsense=" << vs.value << "), VREF = " << vref << " V\n";
  out << "IRUN = " << irun.value << "/32 -> IRMS = " << r.irms_run_a << " A (peak "
      << r.ipeak_run_a << " A)\n";
  out << "IHOLD = " << ihold.value << "/32 -> IRMS(hold) = " << r.irms_hold_a << " A\n";
  out << "Sense drop(peak) = " << r.sense_drop_v * 1e3 << " mV, P(RSENSE) = " << r.sense_p_w
      << " W per resistor\n";
  out << "Ratings (per coil): peak " << (r.peak_ok ? "OK (<= 2A)" : "FAIL (> 2A peak max)")
      << ", RMS " << (r.rms_ok ? "OK (<= 1.2A cont.)" : "OVER 1.2A cont. guide") << '\n';
  if (r.irms_run_a > S::kIrmsDutyA) {
    out << "WARN: above the 1.4A duty-limited guideline (1s on / 1s standby)\n";
  }
  if (r.sense_p_w > S::kRsensePowerW) {
    out << "WARN: sense dissipation above the 0.5W to-1.2A rule of thumb (Ch.8)\n";
  }
  out << "current: " << (r.ok ? "PASS" : "CHECK") << '\n';
  return r.ok ? core::kExitOk : core::kExitRuntime;
}

int run_rsense(ArgCursor &args, std::ostream &out) {
  auto imax_raw = args.take_option({"--imax"});
  if (!imax_raw) {
    return usage_error(out, "rsense needs --imax (target RMS per coil)");
  }
  auto imax = parse_current(*imax_raw);
  if (!imax.ok() || !(imax.value > 0.0)) {
    return usage_error(out, "invalid --imax value (must be > 0)");
  }
  double vref = S::kVrefNomV;
  if (auto o = args.take_option({"--vref"})) {
    auto p = parse_voltage(*o);
    if (!p.ok() || !(p.value > 0.0)) {
      return usage_error(out, "invalid --vref value (must be > 0)");
    }
    vref = p.value;
  }
  auto vs = parse_vsense(args);
  if (!vs.ok()) {
    return usage_error(out, vs.error);
  }
  if (args.has_remaining()) {
    return unexpected_error(out, *args.first_remaining());
  }
  const double need = S::rsense_for_irms_ohm(imax.value, vref, vs.value);
  out << "IMAX = " << imax.value << " A RMS, VREF = " << vref << " V, vsense=" << vs.value
      << '\n';
  if (!(need > 0.0)) {
    out << "FAIL: target current unreachable (need RSENSE <= 0)\n";
    return core::kExitRuntime;
  }
  out << "RSENSE = " << need << " Ohm (exact, incl. 30mOhm internal)\n";
  const int cs = S::cs_for_irms(imax.value, need, S::fullscale_v(vs.value), vref);
  out << "CS for this RSENSE at full scale: " << cs << "/32\n";
  // Nearest Ch.8 table row (design cross-check, not a pick rule).
  int row = -1;
  double row_err = 0.0;
  for (int i = 0; i < S::kRsenseRows; ++i) {
    double err = S::kRsenseOhm[i] - need;
    if (err < 0.0) {
      err = -err;
    }
    if (row < 0 || err < row_err) {
      row = i;
      row_err = err;
    }
  }
  out << "Nearest Ch.8 row: " << S::kRsenseOhm[row] << " Ohm -> "
      << S::kRsenseIrmsA[row] << " A RMS\n";
  return core::kExitOk;
}

int run_rdson(ArgCursor &args, std::ostream &out) {
  auto rref_raw = args.take_option({"--rref"});
  auto ipeak_raw = args.take_option({"--ipeak"});
  auto irms_raw = args.take_option({"--irms"});
  const int n = (rref_raw ? 1 : 0) + (ipeak_raw ? 1 : 0) + (irms_raw ? 1 : 0);
  if (n != 1) {
    return usage_error(out, "rdson needs exactly one of --rref, --ipeak, --irms");
  }
  auto vs = parse_vsense(args);
  if (!vs.ok()) {
    return usage_error(out, vs.error);
  }
  if (args.has_remaining()) {
    return unexpected_error(out, *args.first_remaining());
  }
  const double ratio = (vs.value != 0) ? S::kVfsHighV / S::kVfsLowV : 1.0;
  if (rref_raw) {
    auto p = parse_resistance(*rref_raw);
    if (!p.ok() || !(p.value > 0.0)) {
      return usage_error(out, "invalid --rref value (must be > 0)");
    }
    const double peak = S::rref_ipeak_a(p.value, vs.value);
    const double rms = S::irms_of_peak_a(peak);
    out << "RREF = " << p.value << " Ohm (5VOUT -> VREF, vsense=" << vs.value << ")\n";
    out << "IPEAK = " << peak << " A, IRMS = " << rms << " A\n";
    const int row = S::rref_row_for_ipeak(peak / ratio * 1.0);
    if (row >= 0) {
      out << "Nearest Ch.10 row: RREF " << S::kRrefOhm[row] << " Ohm -> peak "
          << S::kRrefPeakA[row] << " A / RMS " << S::kRrefRmsA[row] << " A\n";
    }
    const bool ok = rms <= S::kIrmsContA && peak <= S::kIpeakMaxA;
    out << "rdson: " << (ok ? "PASS" : "CHECK (over 1.2A cont. / 2A peak)") << '\n';
    return ok ? core::kExitOk : core::kExitRuntime;
  }
  double target_peak = 0.0;
  if (ipeak_raw) {
    auto p = parse_current(*ipeak_raw);
    if (!p.ok() || !(p.value > 0.0)) {
      return usage_error(out, "invalid --ipeak value (must be > 0)");
    }
    target_peak = p.value;
  } else {
    auto p = parse_current(*irms_raw);
    if (!p.ok() || !(p.value > 0.0)) {
      return usage_error(out, "invalid --irms value (must be > 0)");
    }
    target_peak = S::ipeak_of_rms_a(p.value);
  }
  const double rref = S::rref_for_ipeak_ohm(target_peak, vs.value);
  out << "Target peak = " << target_peak << " A (vsense=" << vs.value << ")\n";
  if (!(rref > 0.0)) {
    out << "FAIL: target below the 120mA RREF offset floor\n";
    return core::kExitRuntime;
  }
  out << "RREF = " << rref << " Ohm (exact, 5V/(RREF+700) * 3000 + 0.12A)\n";
  const int row = S::rref_row_for_ipeak(target_peak / ratio);
  if (row >= 0) {
    out << "Nearest Ch.10 row: RREF " << S::kRrefOhm[row] << " Ohm -> peak "
        << S::kRrefPeakA[row] << " A / RMS " << S::kRrefRmsA[row] << " A\n";
  }
  return core::kExitOk;
}

int run_chop(ArgCursor &args, std::ostream &out) {
  auto toff_raw = args.take_option({"--toff"});
  auto freq_raw = args.take_option({"--freq"});
  double duty = 0.5;
  if (auto o = args.take_option({"--duty"})) {
    auto p = parse_double(*o);
    if (!p.ok() || !(p.value > 0.0) || !(p.value < 1.0)) {
      return usage_error(out, "invalid --duty value (fraction in (0, 1))");
    }
    duty = p.value;
  }
  auto fclk = parse_fclk(args);
  if (!fclk.ok()) {
    return usage_error(out, fclk.error);
  }
  int tbl = 2;
  if (auto o = args.take_option({"--tbl"})) {
    auto p = parse_i64(*o);
    if (!p.ok() || p.value < 0 || p.value > 3) {
      return usage_error(out, "--tbl must be 0..3 (16/24/32/40 clocks)");
    }
    tbl = static_cast<int>(p.value);
  }
  if (args.has_remaining()) {
    return unexpected_error(out, *args.first_remaining());
  }
  double toff_set = 0.0;
  if (toff_raw && freq_raw) {
    return usage_error(out, "use either --toff or --freq");
  }
  if (toff_raw) {
    auto p = parse_i64(*toff_raw);
    if (!p.ok() || p.value < 0 || p.value > 15) {
      return usage_error(out, "--toff must be 0..15 (0 disables the driver)");
    }
    toff_set = static_cast<double>(p.value);
  } else if (freq_raw) {
    auto p = parse_frequency(*freq_raw);
    if (!p.ok() || !(p.value > 0.0)) {
      return usage_error(out, "invalid --freq value (target chopper frequency)");
    }
    toff_set = S::toff_for_chop_freq(p.value, duty, fclk.value);
    out << "Target fchop = " << p.value << " Hz (slow-decay share " << duty << ")\n";
  } else {
    return usage_error(out, "chop needs --toff or --freq");
  }
  if (!(toff_set >= 1.0) || !(toff_set <= 15.0)) {
    out << "TOFF = " << toff_set << " FAIL (usable range 1..15, 0 = driver off)\n";
    return core::kExitRuntime;
  }
  const int toff_i = static_cast<int>(toff_set);
  const auto rc = S::toff_time_s(toff_i, fclk.value);
  const auto rb = S::blank_time_s(tbl, fclk.value);
  out << "TOFF = " << toff_i << " (exact " << toff_set << "), NCLK = "
      << (24 + 32 * toff_i) << " clocks\n";
  out << "tOFF = " << rc * 1e6 << " us, tBLANK = " << rb * 1e6 << " us (TBL=" << tbl
      << ", " << S::blank_clocks(tbl) << " clocks)\n";
  if (toff_i == 1 && tbl < 2) {
    out << "WARN: TOFF=1 requires TBL >= 2 (register comment)\n";
  }
  return core::kExitOk;
}

int run_hyst(ArgCursor &args, std::ostream &out) {
  auto hend_raw = args.take_option({"--hend"});
  auto hstrt_raw = args.take_option({"--hstrt"});
  if (!hend_raw || !hstrt_raw) {
    return usage_error(out, "hyst needs --hend (0..15) and --hstrt (0..7)");
  }
  auto he = parse_i64(*hend_raw);
  auto hs = parse_i64(*hstrt_raw);
  if (!he.ok() || he.value < 0 || he.value > 15) {
    return usage_error(out, "--hend must be 0..15");
  }
  if (!hs.ok() || hs.value < 0 || hs.value > 7) {
    return usage_error(out, "--hstrt must be 0..7");
  }
  if (args.has_remaining()) {
    return unexpected_error(out, *args.first_remaining());
  }
  const auto r = S::solve_hysteresis(static_cast<int>(he.value), static_cast<int>(hs.value));
  out << "HEND = " << he.value << " -> " << r.hend_eff << ", HSTRT = " << hs.value << " -> +"
      << r.hstrt_eff << '\n';
  out << "Effective hysteresis = " << r.total_eff << " (limit HEND+HSTRT <= 16) "
      << (r.ok ? "OK" : "FAIL") << '\n';
  return r.ok ? core::kExitOk : core::kExitRuntime;
}

int run_pwmfreq(ArgCursor &args, std::ostream &out) {
  auto fclk = parse_fclk(args);
  if (!fclk.ok()) {
    return usage_error(out, fclk.error);
  }
  auto sel_raw = args.take_option({"--sel"});
  int sel = -1;
  if (sel_raw) {
    auto p = parse_i64(*sel_raw);
    if (!p.ok() || p.value < 0 || p.value > 3) {
      return usage_error(out, "--sel must be 0..3");
    }
    sel = static_cast<int>(p.value);
  }
  if (args.has_remaining()) {
    return unexpected_error(out, *args.first_remaining());
  }
  if (sel >= 0) {
    out << "PWM_FREQ = " << sel << " (2/" << S::kPwmDiv[sel] << " fCLK)\n";
    out << "fPWM = " << S::pwm_freq_hz(sel, fclk.value) / 1e3 << " kHz at fCLK = "
        << fclk.value / 1e6 << " MHz\n";
    return core::kExitOk;
  }
  out << "fCLK = " << fclk.value / 1e6 << " MHz\n";
  for (int i = 0; i < 4; ++i) {
    out << "  PWM_FREQ = " << i << " (2/" << S::kPwmDiv[i]
        << "): fPWM = " << S::pwm_freq_hz(i, fclk.value) / 1e3 << " kHz\n";
  }
  return core::kExitOk;
}

core::Result<solvers::ic::Tmc22xxVelocityResult> velocity_from_args(ArgCursor &args, double fclk,
                                                       int usc, int fsc, std::ostream &out) noexcept {
  auto vactual_raw = args.take_option({"--vactual"});
  auto tstep_raw = args.take_option({"--tstep"});
  auto tpwm_raw = args.take_option({"--tpwmthrs"});
  auto vel_raw = args.take_option({"--vel"});
  const int n = (vactual_raw ? 1 : 0) + (tstep_raw ? 1 : 0) + (tpwm_raw ? 1 : 0) +
                (vel_raw ? 1 : 0);
  if (n != 1) {
    return core::Result<solvers::ic::Tmc22xxVelocityResult>::failure(
        "velocity needs exactly one of --vactual, --tstep, --tpwmthrs, --vel");
  }
  double usteps = 0.0;
  if (vactual_raw) {
    auto p = parse_i64(*vactual_raw);
    if (!p.ok() || p.value < 0 || p.value > 0x7FFFFF) {
      return core::Result<solvers::ic::Tmc22xxVelocityResult>::failure("--vactual must be 0..2^23-1");
    }
    out << "VACTUAL = " << p.value << '\n';
    usteps = S::vactual_to_usteps_hz(static_cast<double>(p.value), fclk);
  } else if (tstep_raw || tpwm_raw) {
    const std::string_view sv = tstep_raw ? *tstep_raw : *tpwm_raw;
    auto p = parse_i64(sv);
    if (!p.ok() || p.value <= 0 || p.value > 0xFFFFF) {
      return core::Result<solvers::ic::Tmc22xxVelocityResult>::failure("TSTEP/TPWMTHRS must be 1..2^20-1");
    }
    out << (tstep_raw ? "TSTEP = " : "TPWMTHRS = ") << p.value << '\n';
    usteps = S::tstep_to_usteps_hz(static_cast<double>(p.value), fclk);
  } else if (vel_raw) {
    auto p = parse_frequency(*vel_raw);
    if (!p.ok() || !(p.value > 0.0)) {
      return core::Result<solvers::ic::Tmc22xxVelocityResult>::failure(
          "invalid --vel value (e.g. 24kHz, 28rpm, 0.5rps)");
    }
    if (vel_is_motor_speed(*vel_raw)) {
      out << "VMOTOR = " << p.value << " rps\n";
      usteps = S::motor_rps_to_usteps_hz(p.value, fsc);
    } else {
      usteps = p.value;
    }
  }
  // TSTEP/TPWMTHRS always count 1/256 steps; rescale only for display when
  // the caller overrides --usc (velocity at the configured resolution).
  if (usc != 256) {
    usteps = usteps * 256.0 / static_cast<double>(usc);
  }
  return core::Result<solvers::ic::Tmc22xxVelocityResult>::success(S::solve_velocity_usteps(usteps, fsc));
}

int run_velocity(ArgCursor &args, std::ostream &out) {
  auto fclk = parse_fclk(args);
  if (!fclk.ok()) {
    return usage_error(out, fclk.error);
  }
  int usc = 256;
  if (auto o = args.take_option({"--usc"})) {
    auto p = parse_i64(*o);
    if (!p.ok() || S::mres_for_usteps(static_cast<int>(p.value)) < 0) {
      return usage_error(out, "--usc must be 1|2|4|8|16|32|64|128|256");
    }
    usc = static_cast<int>(p.value);
  }
  int fsc = S::kFullstepsPerRev;
  if (auto o = args.take_option({"--fsc"})) {
    auto p = parse_i64(*o);
    if (!p.ok() || p.value <= 0 || p.value > 1000) {
      return usage_error(out, "invalid --fsc value (motor fullsteps/rev)");
    }
    fsc = static_cast<int>(p.value);
  }
  auto vr = velocity_from_args(args, fclk.value, usc, fsc, out);
  if (!vr.ok()) {
    return usage_error(out, vr.error);
  }
  if (args.has_remaining()) {
    return unexpected_error(out, *args.first_remaining());
  }
  const auto v = vr.value;
  out << "v = " << v.usteps_hz << " usteps/s (1/256), " << v.fsteps_hz << " fullsteps/s\n";
  out << "v = " << v.rps << " rps = " << v.rpm << " rpm (fsc=" << fsc << ")\n";
  out << "VACTUAL = " << S::usteps_hz_to_vactual(v.usteps_hz, fclk.value) << ", TSTEP = "
      << S::usteps_hz_to_tstep(v.usteps_hz, fclk.value) << " (fCLK = " << fclk.value / 1e6
      << " MHz)\n";
  return core::kExitOk;
}

int run_stealth(ArgCursor &args, std::ostream &out) {
  auto vm_raw = args.take_option({"--vm"});
  auto rcoil_raw = args.take_option({"--rcoil"});
  auto icoil_raw = args.take_option({"--icoil"});
  if (!vm_raw || !rcoil_raw || !icoil_raw) {
    return usage_error(out, "stealth needs --vm, --rcoil and --icoil (target RMS)");
  }
  auto vm = parse_voltage(*vm_raw);
  auto rcoil = parse_resistance(*rcoil_raw);
  auto icoil = parse_current(*icoil_raw);
  if (!vm.ok() || !(vm.value > 0.0)) {
    return usage_error(out, "invalid --vm value (must be > 0)");
  }
  if (!rcoil.ok() || !(rcoil.value > 0.0)) {
    return usage_error(out, "invalid --rcoil value (must be > 0)");
  }
  if (!icoil.ok() || !(icoil.value > 0.0)) {
    return usage_error(out, "invalid --icoil value (must be > 0)");
  }
  auto fclk = parse_fclk(args);
  if (!fclk.ok()) {
    return usage_error(out, fclk.error);
  }
  int tbl = 1;
  if (auto o = args.take_option({"--tbl"})) {
    auto p = parse_i64(*o);
    if (!p.ok() || p.value < 0 || p.value > 3) {
      return usage_error(out, "--tbl must be 0..3");
    }
    tbl = static_cast<int>(p.value);
  }
  int pwmfreq = 0;
  if (auto o = args.take_option({"--pwmfreq"})) {
    auto p = parse_i64(*o);
    if (!p.ok() || p.value < 0 || p.value > 3) {
      return usage_error(out, "--pwmfreq must be 0..3");
    }
    pwmfreq = static_cast<int>(p.value);
  }
  if (args.has_remaining()) {
    return unexpected_error(out, *args.first_remaining());
  }
  const double tblank = S::blank_time_s(tbl, fclk.value);
  const double fpwm = S::pwm_freq_hz(pwmfreq, fclk.value);
  const double ilower = S::stealth_lower_limit_a(tblank, fpwm, vm.value, rcoil.value);
  const double ampl = S::pwm_ampl_seed(rcoil.value, icoil.value, vm.value);
  out << "VM = " << vm.value << " V, RCOIL = " << rcoil.value << " Ohm, ICOIL = " << icoil.value
      << " A RMS\n";
  out << "fPWM = " << fpwm / 1e3 << " kHz (sel " << pwmfreq << "), tBLANK = " << tblank * 1e6
      << " us (TBL=" << tbl << ")\n";
  out << "ILowerLimit = " << ilower * 1e3 << " mA (AT#1 run current must exceed this)\n";
  out << "PWM_AMPL seed = " << ampl << " (374*RCOIL*ICOIL/VM)\n";
  const bool ok = icoil.value > ilower && ampl > 0.0 && ampl < 255.0;
  out << "stealth: " << (ok ? "PASS (tunable)" : "CHECK (over range or below limit)") << '\n';
  return ok ? core::kExitOk : core::kExitRuntime;
}

int run_grad(ArgCursor &args, std::ostream &out) {
  auto cbemf_raw = args.take_option({"--cbemf"});
  auto torque_raw = args.take_option({"--torque"});
  auto icoilnom_raw = args.take_option({"--icoilnom"});
  double cbemf = 0.0;
  if (cbemf_raw && (torque_raw || icoilnom_raw)) {
    return usage_error(out, "use either --cbemf or --torque/--icoilnom");
  }
  if (cbemf_raw) {
    auto p = parse_double(*cbemf_raw);
    if (!p.ok() || !(p.value > 0.0)) {
      return usage_error(out, "invalid --cbemf value (V per rad/s, must be > 0)");
    }
    cbemf = p.value;
  } else {
    if (!torque_raw || !icoilnom_raw) {
      return usage_error(out, "grad needs --cbemf or --torque + --icoilnom");
    }
    auto t = parse_double(*torque_raw);
    auto i = parse_current(*icoilnom_raw);
    if (!t.ok() || !(t.value > 0.0)) {
      return usage_error(out, "invalid --torque value (Nm, must be > 0)");
    }
    if (!i.ok() || !(i.value > 0.0)) {
      return usage_error(out, "invalid --icoilnom value (must be > 0)");
    }
    cbemf = S::backemf_const_v_per_rads(t.value, i.value);
    out << "Holding torque = " << t.value << " Nm at " << i.value << " A -> CBEMF = " << cbemf
        << " V/(rad/s)\n";
  }
  auto vm_raw = args.take_option({"--vm"});
  if (!vm_raw) {
    return usage_error(out, "grad needs --vm");
  }
  auto vm = parse_voltage(*vm_raw);
  if (!vm.ok() || !(vm.value > 0.0)) {
    return usage_error(out, "invalid --vm value (must be > 0)");
  }
  auto fclk = parse_fclk(args);
  if (!fclk.ok()) {
    return usage_error(out, fclk.error);
  }
  double mspr = 51200.0;
  if (auto o = args.take_option({"--mspr"})) {
    auto p = parse_double(*o);
    if (!p.ok() || !(p.value > 0.0)) {
      return usage_error(out, "invalid --mspr value (must be > 0)");
    }
    mspr = p.value;
  }
  if (args.has_remaining()) {
    return unexpected_error(out, *args.first_remaining());
  }
  const double grad = S::pwm_grad_seed(cbemf, fclk.value, vm.value, mspr);
  out << "CBEMF = " << cbemf << " V/(rad/s), VM = " << vm.value << " V, MSPR = " << mspr << '\n';
  out << "PWM_GRAD seed = " << grad << '\n';
  const int idx = S::otp_pwm_grad_idx(grad);
  out << "Nearest OTP_PWM_GRAD[" << idx << "] = " << S::kOtpPwmGrad[idx] << '\n';
  const bool ok = grad > 0.0 && grad <= 255.0;
  out << "grad: " << (ok ? "PASS" : "CHECK (over the 8-bit range)") << '\n';
  return ok ? core::kExitOk : core::kExitRuntime;
}

int run_standby(ArgCursor &args, std::ostream &out) {
  auto tp_raw = args.take_option({"--tpowerdown"});
  if (!tp_raw) {
    return usage_error(out, "standby needs --tpowerdown (0..255)");
  }
  auto tp = parse_i64(*tp_raw);
  if (!tp.ok() || tp.value < 0 || tp.value > 255) {
    return usage_error(out, "--tpowerdown must be 0..255");
  }
  auto fclk = parse_fclk(args);
  if (!fclk.ok()) {
    return usage_error(out, fclk.error);
  }
  auto irun = parse_cs(args, "--irun", 31);
  if (!irun.ok()) {
    return usage_error(out, irun.error);
  }
  auto ihold = parse_cs(args, "--ihold", 16);
  if (!ihold.ok()) {
    return usage_error(out, ihold.error);
  }
  int iholddelay = 4;
  if (auto o = args.take_option({"--iholddelay"})) {
    auto p = parse_i64(*o);
    if (!p.ok() || p.value < 0 || p.value > 15) {
      return usage_error(out, "--iholddelay must be 0..15");
    }
    iholddelay = static_cast<int>(p.value);
  }
  if (args.has_remaining()) {
    return unexpected_error(out, *args.first_remaining());
  }
  const auto r = S::solve_standby(static_cast<int>(tp.value), irun.value, ihold.value,
                                  iholddelay, fclk.value);
  out << "Standstill detect = " << r.standstill_s * 1e3 << " ms (2^20 clocks)\n";
  out << "TPOWERDOWN = " << tp.value << " -> " << r.tpowerdown_s << " s\n";
  if (tp.value < 2) {
    out << "WARN: TPOWERDOWN < 2 blocks StealthChop PWM_OFS_AUTO tuning (Ch.5.2)\n";
  }
  out << "IHOLDDELAY = " << iholddelay << ", IRUN " << irun.value << " -> IHOLD "
      << ihold.value << ": decay = " << r.decay_s << " s\n";
  out << "Total to hold current = " << r.total_s << " s\n";
  return core::kExitOk;
}

int run_thermal(ArgCursor &args, std::ostream &out) {
  auto irms_raw = args.take_option({"--irms"});
  if (!irms_raw) {
    return usage_error(out, "thermal needs --irms (RMS per coil)");
  }
  auto irms = parse_current(*irms_raw);
  if (!irms.ok() || !(irms.value >= 0.0)) {
    return usage_error(out, "invalid --irms value (must be >= 0)");
  }
  double ta = 25.0;
  if (auto o = args.take_option({"--ta"})) {
    auto p = parse_double(*o);
    if (!p.ok()) {
      return usage_error(out, "invalid --ta value (C, e.g. 85)");
    }
    ta = p.value;
  }
  double rja = S::kRthJaKpw;
  if (auto o = args.take_option({"--rja"})) {
    auto p = parse_double(*o);
    if (!p.ok() || !(p.value > 0.0)) {
      return usage_error(out, "invalid --rja value (K/W, must be > 0)");
    }
    rja = p.value;
  }
  if (args.has_remaining()) {
    return unexpected_error(out, *args.first_remaining());
  }
  const double pd = S::conduction_loss_w(irms.value);
  const auto r = S::solve_thermal(pd, ta, rja);
  out << "IRMS = " << irms.value << " A per coil (RON HS 0.29 / LS 0.28 Ohm)\n";
  out << "Pcond = " << r.pcond_w << " W (conduction floor, no switching loss)\n";
  out << "TJ = " << r.tj_c << " C at TA = " << ta << " C (RthJA " << rja << " K/W) "
      << (r.ok ? "PASS (<= +125 C)" : "FAIL (> +125 C)") << '\n';
  if (pd > 3.3) {
    out << "WARN: above ~3.3W the 30K/W board exceeds +125 C at TA=25 C (Sec. 18.3)\n";
  }
  return r.ok ? core::kExitOk : core::kExitRuntime;
}

int run_mres(ArgCursor &args, std::ostream &out) {
  auto code_raw = args.take_option({"--code"});
  auto usteps_raw = args.take_option({"--usteps"});
  auto pins_raw = args.take_option({"--pins"});
  const int n = (code_raw ? 1 : 0) + (usteps_raw ? 1 : 0) + (pins_raw ? 1 : 0);
  if (n != 1) {
    return usage_error(out, "mres needs exactly one of --code, --usteps, --pins");
  }
  if (pins_raw) {
    const bool is_220x = (*pins_raw == "220x");
    const bool is_222x = (*pins_raw == "222x");
    if (!is_220x && !is_222x) {
      return usage_error(out, "--pins must be 220x|222x");
    }
    auto ms1_raw = args.take_option({"--ms1"});
    auto ms2_raw = args.take_option({"--ms2"});
    if (!ms1_raw || !ms2_raw) {
      return usage_error(out, "--pins needs --ms1 0|1 and --ms2 0|1");
    }
    auto ms1 = parse_i64(*ms1_raw);
    auto ms2 = parse_i64(*ms2_raw);
    if (!ms1.ok() || (ms1.value != 0 && ms1.value != 1)) {
      return usage_error(out, "--ms1 must be 0|1");
    }
    if (!ms2.ok() || (ms2.value != 0 && ms2.value != 1)) {
      return usage_error(out, "--ms2 must be 0|1");
    }
    if (args.has_remaining()) {
      return unexpected_error(out, *args.first_remaining());
    }
    const int u = S::standalone_usteps(is_220x, static_cast<int>(ms1.value),
                                       static_cast<int>(ms2.value));
    out << "MS2=" << ms2.value << " MS1=" << ms1.value << " (" << *pins_raw
        << ") -> " << u << " microsteps\n";
    const int mres = S::mres_for_usteps(u);
    out << "MRES code = " << mres << " (UART/OTP; pins apply unless mstep_reg_select)\n";
    return core::kExitOk;
  }
  if (code_raw) {
    auto p = parse_i64(*code_raw);
    if (!p.ok() || S::usteps_for_mres(static_cast<int>(p.value)) < 0) {
      return usage_error(out, "--code must be 0..8 (0 = native 256)");
    }
    if (args.has_remaining()) {
      return unexpected_error(out, *args.first_remaining());
    }
    out << "MRES = " << p.value << " -> " << S::usteps_for_mres(static_cast<int>(p.value))
        << " microsteps per STEP pulse\n";
    return core::kExitOk;
  }
  auto p = parse_i64(*usteps_raw);
  if (!p.ok()) {
    return usage_error(out, "invalid --usteps value");
  }
  if (args.has_remaining()) {
    return unexpected_error(out, *args.first_remaining());
  }
  const int mres = S::mres_for_usteps(static_cast<int>(p.value));
  if (mres < 0) {
    out << "FAIL: " << p.value << " is not a 256>>m resolution (1..256 powers of two)\n";
    return core::kExitRuntime;
  }
  out << p.value << " microsteps -> MRES = " << mres << '\n';
  return core::kExitOk;
}

int run_otp(ArgCursor &args, std::ostream &out) {
  auto grad_raw = args.take_option({"--pwmgrad"});
  auto tth_raw = args.take_option({"--tpwmthrs"});
  auto ihold_raw = args.take_option({"--ihold"});
  auto ihd_raw = args.take_option({"--iholddelay"});
  const int n =
      (grad_raw ? 1 : 0) + (tth_raw ? 1 : 0) + (ihold_raw ? 1 : 0) + (ihd_raw ? 1 : 0);
  if (n != 1) {
    return usage_error(out, "otp needs one of --pwmgrad, --tpwmthrs, --ihold, --iholddelay");
  }
  if (args.has_remaining()) {
    return unexpected_error(out, *args.first_remaining());
  }
  if (grad_raw) {
    auto p = parse_double(*grad_raw);
    if (!p.ok() || !(p.value >= 0.0)) {
      return usage_error(out, "invalid --pwmgrad value (must be >= 0)");
    }
    const int idx = S::otp_pwm_grad_idx(p.value);
    out << "PWM_GRAD " << p.value << " -> OTP_PWM_GRAD[" << idx
        << "] = " << S::kOtpPwmGrad[idx] << '\n';
    return core::kExitOk;
  }
  if (tth_raw) {
    auto p = parse_double(*tth_raw);
    if (!p.ok() || !(p.value >= 0.0)) {
      return usage_error(out, "invalid --tpwmthrs value (must be >= 0)");
    }
    const int idx = S::otp_tpwmthrs_idx(p.value);
    out << "TPWMTHRS " << p.value << " -> OTP_TPWMTHRS[" << idx
        << "] = " << S::kOtpTpwmthrs[idx] << '\n';
    return core::kExitOk;
  }
  if (ihold_raw) {
    auto p = parse_i64(*ihold_raw);
    if (!p.ok()) {
      return usage_error(out, "invalid --ihold value");
    }
    const int code = S::otp_ihold_code(static_cast<int>(p.value));
    if (code < 0) {
      out << "FAIL: IHOLD " << p.value << " not in OTP map (16|2|8|24)\n";
      return core::kExitRuntime;
    }
    out << "IHOLD " << p.value << " -> OTP_IHOLD code %" << code << '\n';
    return core::kExitOk;
  }
  auto p = parse_i64(*ihd_raw);
  if (!p.ok()) {
    return usage_error(out, "invalid --iholddelay value");
  }
  const int code = S::otp_iholddelay_code(static_cast<int>(p.value));
  if (code < 0) {
    out << "FAIL: IHOLDDELAY " << p.value << " not in OTP map (1|2|4|8)\n";
    return core::kExitRuntime;
  }
  out << "IHOLDDELAY " << p.value << " -> OTP_IHOLDDELAY code %" << code << '\n';
  return core::kExitOk;
}

int run_uart(ArgCursor &args, std::ostream &out) {
  auto fclk = parse_fclk(args);
  if (!fclk.ok()) {
    return usage_error(out, fclk.error);
  }
  int sd = -1;
  if (auto o = args.take_option({"--senddelay"})) {
    auto p = parse_i64(*o);
    if (!p.ok() || S::senddelay_bits(static_cast<int>(p.value)) < 0) {
      return usage_error(out, "--senddelay must be 0..15");
    }
    sd = static_cast<int>(p.value);
  }
  if (args.has_remaining()) {
    return unexpected_error(out, *args.first_remaining());
  }
  out << "UART single-wire: 8N1 + CRC8-ATM (poly 0x07, init 0), fixed node 0\n";
  out << "Baud: " << S::kUartMinBd << ".." << S::uart_baud_max(fclk.value) << " Bd at fCLK = "
      << fclk.value / 1e6 << " MHz (max fCLK/16)\n";
  out << "Frames: write " << S::kUartWriteLen << "B, read-req " << S::kUartReadReqLen
      << "B, read-reply " << S::kUartReadReplyLen << "B (sync nibble 1010)\n";
  if (sd >= 0) {
    out << "SENDDELAY = " << sd << " -> " << S::senddelay_bits(sd) << " bit times\n";
  } else {
    out << "SENDDELAY default 8 bit times (codes 0|1); max 120 (codes 14|15)\n";
  }
  return core::kExitOk;
}

// Hex bytes accept 0x prefix or bare hex (CRC datagrams are byte strings).
core::Result<unsigned> parse_hex_byte(std::string_view s) noexcept {
  std::string_view d = s;
  if (d.size() > 2 && d[0] == '0' && (d[1] == 'x' || d[1] == 'X')) {
    d = d.substr(2);
  }
  if (d.empty() || d.size() > 2) {
    return core::Result<unsigned>::failure("invalid hex byte");
  }
  unsigned acc = 0;
  for (char c : d) {
    unsigned digit = 16;
    if (c >= '0' && c <= '9') {
      digit = static_cast<unsigned>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      digit = static_cast<unsigned>(c - 'a' + 10);
    } else if (c >= 'A' && c <= 'F') {
      digit = static_cast<unsigned>(c - 'A' + 10);
    } else {
      return core::Result<unsigned>::failure("invalid hex byte");
    }
    acc = acc * 16u + digit;
  }
  return core::Result<unsigned>::success(acc);
}

int run_crc(ArgCursor &args, std::ostream &out) {
  unsigned crc = 0;
  int n = 0;
  while (auto tok = args.next_positional()) {
    auto p = parse_hex_byte(*tok);
    if (!p.ok()) {
      return usage_error(out, "crc needs hex bytes (e.g. crc 05 00 6C ..)");
    }
    crc = S::crc8_step(crc, p.value);
    ++n;
  }
  if (n == 0) {
    return usage_error(out, "crc needs at least one hex byte");
  }
  if (args.has_remaining()) {
    return unexpected_error(out, *args.first_remaining());
  }
  char buf[8]{};
  const int w = std::snprintf(buf, sizeof buf, "0x%02X", crc);
  (void)w;
  out << "CRC8-ATM(" << n << " bytes) = " << buf << " (" << crc << ")\n";
  return core::kExitOk;
}

int run_spec(ArgCursor &args, std::ostream &out) {
  if (args.has_remaining()) {
    return unexpected_error(out, *args.first_remaining());
  }
  out << "TMC2202/2208/2224: 2-phase stepper driver, STEP/DIR + single-wire UART\n";
  out << "VS 4.75..36 V (5.5..36 with internal 5V), VIO 3.0..5.25 V, QFN28/32\n";
  out << "IRMS 1.2A cont. (1.4A duty-limited), IPEAK 2A; RON LS 0.28 / HS 0.29 Ohm\n";
  out << "VFS 325mV (vsense=0) / 180mV (vsense=1), +30mOhm internal; fCLK 12MHz int.\n";
  out << "StealthChop2 + SpreadCycle (TPWMTHRS switch), 256 MicroPlyer, VACTUAL motion\n";
  out << "OT 143/150/157 C, OTPW 120/143 C; RthJA 30 K/W (multilayer), RthJC 6 K/W\n";
  return core::kExitOk;
}

constexpr std::array<std::string_view, 16> kTmc22xxOps{{
    "current",
    "rsense",
    "rdson",
    "chop",
    "hyst",
    "pwmfreq",
    "velocity",
    "stealth",
    "grad",
    "standby",
    "thermal",
    "mres",
    "otp",
    "uart",
    "crc",
    "spec",
}};

constexpr std::array<std::string_view, 33> kTmc22xxOptions{{
    "--rsense", "--irun",   "--ihold",  "--vref",   "--vsense", "--imax",   "--rref",
    "--ipeak",  "--irms",   "--toff",   "--freq",   "--duty",   "--fclk",   "--tbl",
    "--hend",   "--hstrt",  "--sel",    "--vactual", "--tstep", "--tpwmthrs", "--vel",
    "--usc",    "--fsc",    "--vm",     "--rcoil",  "--icoil",
    "--pwmfreq", "--cbemf", "--torque", "--icoilnom", "--mspr", "--tpowerdown", "--iholddelay",
}};

constexpr std::array<std::string_view, 9> kTmc22xxTailOptions{{
    "--ta", "--rja", "--code", "--usteps", "--pins", "--ms1", "--ms2",
    "--pwmgrad", "--senddelay",
}};

constexpr std::array<std::string_view, 2> kTmc22xxHelp{{"--help", "-h"}};

void push_matches(std::string_view partial, std::span<const std::string_view> options,
                  std::vector<std::string> &out) {
  for (std::string_view o : options) {
    if (o.substr(0, partial.size()) == partial) {
      out.emplace_back(o);
    }
  }
}

} // namespace

int Tmc22xxSpec::run(ArgCursor &args, std::ostream &out) {
  if (args.take_flag({"--help", "-h"})) {
    out << kName << " - " << kDescription << "\nUsage:\n" << kUsage << '\n';
    return core::kExitOk;
  }
  std::optional<std::string_view> op = args.next_positional();
  if (!op) {
    return usage_error(
        out, "missing op (current|rsense|rdson|chop|hyst|pwmfreq|velocity|stealth|grad|"
             "standby|thermal|mres|otp|uart|crc|spec)");
  }
  if (*op == "current") {
    return run_current(args, out);
  }
  if (*op == "rsense") {
    return run_rsense(args, out);
  }
  if (*op == "rdson") {
    return run_rdson(args, out);
  }
  if (*op == "chop") {
    return run_chop(args, out);
  }
  if (*op == "hyst") {
    return run_hyst(args, out);
  }
  if (*op == "pwmfreq") {
    return run_pwmfreq(args, out);
  }
  if (*op == "velocity") {
    return run_velocity(args, out);
  }
  if (*op == "stealth") {
    return run_stealth(args, out);
  }
  if (*op == "grad") {
    return run_grad(args, out);
  }
  if (*op == "standby") {
    return run_standby(args, out);
  }
  if (*op == "thermal") {
    return run_thermal(args, out);
  }
  if (*op == "mres") {
    return run_mres(args, out);
  }
  if (*op == "otp") {
    return run_otp(args, out);
  }
  if (*op == "uart") {
    return run_uart(args, out);
  }
  if (*op == "crc") {
    return run_crc(args, out);
  }
  if (*op == "spec") {
    return run_spec(args, out);
  }
  return usage_error(
      out, "unknown op (current|rsense|rdson|chop|hyst|pwmfreq|velocity|stealth|grad|"
           "standby|thermal|mres|otp|uart|crc|spec)");
}

void Tmc22xxSpec::complete(std::span<const std::string_view> prior_args, std::string_view partial,
                           std::vector<std::string> &out) {
  if (!partial.empty() && partial.front() == '-') {
    push_matches(partial, kTmc22xxOptions, out);
    push_matches(partial, kTmc22xxTailOptions, out);
    push_matches(partial, kTmc22xxHelp, out);
    return;
  }
  if (prior_args.empty()) {
    push_matches(partial, kTmc22xxOps, out);
  }
  // Later positionals are numeric values (or CRC hex bytes): nothing to complete.
}

} // namespace wengine::cli::ic
