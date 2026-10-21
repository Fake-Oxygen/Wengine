#include "wengine/cli/command_registry.hpp"
#include "wengine/cli/ic/tmc22xx.hpp"
#include "wengine/cli/ic_dispatch.hpp"
#include "wengine/cli/ic_registry.hpp"
#include "wengine/cli/shell.hpp"
#include "wengine/solvers/ic/tmc22xx.hpp"

#include <array>
#include <cassert>
#include <cmath>
#include <iostream>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace wengine::cli;
using S = wengine::solvers::ic::Tmc22xxSolver;

namespace {

constexpr bool near(double a, double b, double eps = 1e-9) {
  return std::fabs(a - b) <= eps * (1.0 + std::fabs(b));
}

void test_current_eqs() {
  // Eq.1 golden (Ch.9 standalone): 0.325/(0.22+0.03)/sqrt(2) = 0.9192A.
  assert(near(S::irms_standalone_a(0.22, 2.5, 0), 0.9192388155, 1e-9));
  assert(near(S::ipeak_of_rms_a(0.9192388155), 1.3, 1e-9));
  // VREF scaling (Eq.3): half VREF halves the current.
  assert(near(S::irms_standalone_a(0.22, 1.25, 0), 0.9192388155 / 2.0, 1e-9));
  // vsense=1 full scale is 180mV: 0.180/0.325 of the vsense=0 current.
  assert(near(S::irms_standalone_a(0.22, 2.5, 1), 0.5091168825, 1e-9));
  // Eq.2 golden: CS=16 of 31 scales run current by 17/32.
  assert(near(S::irms_cs_a(16, 0.22, 0.325, 2.5), 0.4883456208, 1e-9));
  assert(near(S::irms_cs_a(31, 0.22, 0.325, 2.5), S::irms_standalone_a(0.22, 2.5, 0),
              1e-12));
  // Inverse (Eq.1): 0.9192A needs 0.22 Ohm.
  assert(near(S::rsense_for_irms_ohm(0.9192388155, 2.5, 0), 0.22, 1e-9));
  assert(S::rsense_for_irms_ohm(0.0, 2.5, 0) < 0.0);
  // CS picker: 0.9192A at 0.22 Ohm needs full scale; 1A is unreachable.
  assert(S::cs_for_irms(0.9192388155, 0.22, 0.325, 2.5) == 31);
  assert(S::cs_for_irms(1.0, 0.22, 0.325, 2.5) == -1);
  assert(S::cs_for_irms(0.1, 0.22, 0.325, 2.5) >= 0);
  // Sense dissipation: 0.9192^2 * 0.22 = 0.1859W.
  assert(near(S::sense_power_w(0.9192388155, 0.22), 0.1859, 1e-9));

  const auto r = S::solve_current(0.22, 31, 16, 2.5, 0);
  assert(near(r.vfs_v, 0.325, 1e-12));
  assert(near(r.irms_run_a, 0.9192388155, 1e-9));
  assert(near(r.ipeak_run_a, 1.3, 1e-9));
  assert(near(r.irms_hold_a, 0.4883456208, 1e-9));
  assert(near(r.sense_drop_v, 0.325, 1e-9));
  assert(r.peak_ok && r.rms_ok && r.ok);

  const auto hot = S::solve_current(0.10, 31, 16, 2.5, 0);
  assert(!hot.rms_ok && !hot.peak_ok && !hot.ok); // 1.77A RMS, 2.5A peak

  const auto bad = S::solve_current(0.0, 31, 16, 2.5, 0);
  assert(!bad.ok);
}

void test_rdson_eq() {
  // Eq.4 golden (Ch.10): 10k -> 3000*5/10700 + 0.12 = 1.5219A peak.
  assert(near(S::rref_ipeak_a(10000.0, 0), 1.5218691589, 1e-9));
  assert(near(S::irms_of_peak_a(1.5218691589), 1.0761240023, 1e-9));
  assert(near(S::rref_ipeak_a(6800.0, 0), 2.12, 1e-9)); // table top row
  // vsense=1 scales by 180/325 (~55%, Ch.10).
  assert(near(S::rref_ipeak_a(10000.0, 1), 1.5218691589 * 0.180 / 0.325, 1e-9));
  // Inverse: 1.5A peak needs ~10.2k.
  assert(near(S::rref_for_ipeak_ohm(1.5, 0), 10169.5652, 1e-6));
  assert(S::rref_for_ipeak_ohm(0.1, 0) < 0.0); // below the offset floor
  // Table lookup: 1.5219A is nearest the 10k / 1.51A row.
  assert(S::rref_row_for_ipeak(1.5218692) == 4);
  assert(S::kRrefOhm[4] == 10000.0);
}

void test_chop_eqs() {
  // Eq.5 golden (Ch.7): 10us slow decay -> TOFF=3 @12MHz, 4.25 @16MHz.
  assert(near(S::toff_for_time(10e-6, 12e6), 3.0, 1e-9));
  assert(near(S::toff_for_time(10e-6, 16e6), 4.25, 1e-9));
  assert(near(S::toff_for_chop_freq(25e3, 0.5, 12e6), 3.0, 1e-9));
  assert(near(S::toff_time_s(3, 12e6), 10e-6, 1e-12));
  // Eq.6: TBL=2 -> 32 clocks -> 2.6667us @12MHz.
  assert(near(S::blank_time_s(2, 12e6), 32.0 / 12e6, 1e-12));
  assert(S::blank_time_s(4, 12e6) < 0.0);
  // Eq.7 golden (Table 6.1): sel=1 -> 2/683 * 12MHz = 35.139kHz.
  assert(near(S::pwm_freq_hz(1, 12e6), 35139.09224, 1e-9));
  assert(near(S::pwm_freq_hz(0, 12e6), 23437.5, 1e-9));
  assert(S::pwm_freq_hz(4, 12e6) < 0.0);
  // Hysteresis golden (Ch.7 example): HEND=6/HSTRT=0 -> 3+1 = 4.
  const auto h = S::solve_hysteresis(6, 0);
  assert(h.hend_eff == 3 && h.hstrt_eff == 1 && h.total_eff == 4 && h.ok);
  // MRES codes and standalone pin decodes.
  assert(S::usteps_for_mres(0) == 256 && S::usteps_for_mres(8) == 1);
  assert(S::mres_for_usteps(32) == 3);
  assert(S::mres_for_usteps(20) == -1);
}

void test_velocity_eqs() {
  // Eq.13 golden: VACTUAL=1000 @12MHz -> 715.256Hz.
  assert(near(S::vactual_to_usteps_hz(1000.0, 12e6), 715.2557373, 1e-9));
  assert(near(S::usteps_hz_to_vactual(715.2557373, 12e6), 1000.0, 1e-6));
  // TSTEP=500 @12MHz -> 24kHz -> 93.75 FS/s -> 28.125rpm (200FS motor).
  assert(near(S::tstep_to_usteps_hz(500.0, 12e6), 24000.0, 1e-9));
  const auto v = S::solve_velocity_usteps(24000.0, 200);
  assert(near(v.fsteps_hz, 93.75, 1e-9));
  assert(near(v.rpm, 28.125, 1e-9));
  assert(near(S::motor_rps_to_usteps_hz(28.125 / 60.0, 200), 24000.0, 1e-9));
  assert(near(S::motor_rps_to_usteps_hz(0.5, 200), 25600.0, 1e-9));
  assert(near(S::usteps_hz_to_tstep(24000.0, 12e6), 500.0, 1e-9));
}

void test_stealth_eqs() {
  // Eq.8 golden (Sec. 6.3.1): 24clk, fPWM=2/1024 fCLK, 24V, 5Ohm -> 225mA.
  const double tblank = 24.0 / 12e6;
  assert(near(S::stealth_lower_limit_a(tblank, 12e6 * 2.0 / 1024.0, 24.0, 5.0), 0.225,
              1e-9));
  // Eq.9 golden (Sec. 6.4): 374*5*0.5/24 = 38.958.
  assert(near(S::pwm_ampl_seed(5.0, 0.5, 24.0), 38.9583333, 1e-9));
  // Eq.10: 36*(31+1)/32 + 18*256/16777.2 = 36.2747.
  assert(near(S::pwm_scale_sum(36.0, 31, 18.0, 16777.216), 36.2746582, 1e-6));
  // Eq.12 golden: 0.4Nm at 2*1A -> 0.2 V/(rad/s).
  assert(near(S::backemf_const_v_per_rads(0.4, 1.0), 0.2, 1e-9));
  // Eq.11: 0.2 CBEMF @12MHz/24V -> 17.917.
  assert(near(S::pwm_grad_seed(0.2, 12e6, 24.0), 17.9168956, 1e-9));
  // OTP nearest picks.
  assert(S::otp_pwm_grad_idx(17.9168956) == 2); // table[2] = 18
  assert(S::otp_tpwmthrs_idx(460.0) == 4);      // table[4] = 500
}

void test_standby_thermal_uart() {
  // Eq.15 goldens: standstill 2^20/12MHz = 87.38ms.
  assert(near(S::standstill_time_s(12e6), 0.0873813333, 1e-9));
  // TPOWERDOWN=20 -> 0.4369s; IHOLDDELAY=4, 15 steps @16MHz -> 0.983s.
  assert(near(S::tpowerdown_time_s(20, 12e6), 0.4369066667, 1e-9));
  assert(near(15.0 * S::iholddelay_step_s(4, 16e6), 0.98304, 1e-9));
  const auto sb = S::solve_standby(20, 31, 16, 4, 12e6);
  assert(near(sb.total_s, sb.standstill_s + sb.tpowerdown_s + sb.decay_s, 1e-12));
  // Eq.16: 1A RMS -> 2*1*0.57 = 1.14W -> TJ = 59.2 C at TA=25.
  assert(near(S::conduction_loss_w(1.0), 1.14, 1e-12));
  const auto th = S::solve_thermal(1.14, 25.0, 30.0);
  assert(near(th.tj_c, 59.2, 1e-9) && th.ok);
  const auto hot = S::solve_thermal(2.5, 85.0, 30.0);
  assert(!hot.ok); // 160 C exceeds the +125 C operating max
  // UART: max fCLK/16, SENDDELAY map, frame sizes.
  assert(near(S::uart_baud_max(12e6), 750000.0, 1e-9));
  assert(S::senddelay_bits(15) == 120 && S::senddelay_bits(1) == 8);
  // Eq.17 CRC8-ATM: {05 00} -> 0x18; 7-byte write frame -> 0xFE.
  unsigned crc = 0;
  for (unsigned b : {0x05u, 0x00u}) {
    crc = S::crc8_step(crc, b);
  }
  assert(crc == 0x18u);
  crc = 0;
  for (unsigned b : {0x05u, 0x00u, 0x6Cu, 0x10u, 0x00u, 0x00u, 0x53u}) {
    crc = S::crc8_step(crc, b);
  }
  assert(crc == 0xFEu);
}

std::pair<int, std::string> run_chip(std::string line) {
  std::array<std::string_view, kMaxArgs> buf{};
  const std::size_t n = tokenize_into(line, buf);
  const std::size_t m = n < buf.size() ? n : buf.size();
  std::span<const std::string_view> args{buf.data(), m};
  ArgCursor cur{args};
  std::ostringstream out;
  const int rc = ic::Tmc22xxSpec::run(cur, out);
  return {rc, out.str()};
}

void test_cli_current_rsense() {
  {
    auto [rc, s] = run_chip("current --rsense 220mOhm");
    assert(rc == 0);
    assert(s.find("IRMS = 0.919239") != std::string::npos);
    assert(s.find("peak 1.3 A") != std::string::npos);
    assert(s.find("PASS") != std::string::npos);
  }
  {
    auto [rc, s] = run_chip("current --rsense 100mOhm");
    assert(rc == 1 && s.find("OVER 1.2A") != std::string::npos); // 1.77A
  }
  {
    auto [rc, s] = run_chip("current --rsense 220mOhm --irun 16 --ihold 8 --vsense 1");
    assert(rc == 0 && s.find("VFS = 180 mV") != std::string::npos);
  }
  {
    auto [rc, s] = run_chip("rsense --imax 1A");
    assert(rc == 0);
    assert(s.find("RSENSE = 0.19981") != std::string::npos);
    assert(s.find("Nearest Ch.8 row: 0.22 Ohm") != std::string::npos);
  }
  {
    auto [rc, s] = run_chip("current");
    assert(rc == 2); // --rsense required
  }
  {
    auto [rc, s] = run_chip("rsense --imax 0A");
    assert(rc == 2);
  }
  {
    auto [rc, s] = run_chip("current --rsense 220mOhm extra");
    assert(rc == 2);
  }
  {
    auto [rc, s] = run_chip("current --rsense 220mOhm --vsense 2");
    assert(rc == 2);
  }
}

void test_cli_rdson_chop_hyst() {
  {
    auto [rc, s] = run_chip("rdson --rref 10kOhm");
    assert(rc == 0);
    assert(s.find("IPEAK = 1.52187") != std::string::npos);
    assert(s.find("IRMS = 1.07612") != std::string::npos);
    assert(s.find("Ch.10 row: RREF 10000") != std::string::npos);
  }
  {
    auto [rc, s] = run_chip("rdson --ipeak 1.5A");
    assert(rc == 0 && s.find("RREF = 10169.6") != std::string::npos);
  }
  {
    auto [rc, s] = run_chip("rdson --irms 1A --vsense 1");
    assert(rc == 0 && s.find("Target peak = 1.41421") != std::string::npos);
  }
  {
    auto [rc, s] = run_chip("rdson");
    assert(rc == 2);
  }
  {
    auto [rc, s] = run_chip("rdson --rref 10kOhm --ipeak 1A");
    assert(rc == 2); // mutually exclusive
  }
  {
    auto [rc, s] = run_chip("chop --toff 3 --fclk 12MHz --tbl 2");
    assert(rc == 0);
    assert(s.find("tOFF = 10 us") != std::string::npos);
    assert(s.find("tBLANK = 2.66667 us") != std::string::npos);
  }
  {
    auto [rc, s] = run_chip("chop --freq 25kHz --duty 0.5 --fclk 12MHz");
    assert(rc == 0 && s.find("TOFF = 3 (exact 3)") != std::string::npos);
  }
  {
    auto [rc, s] = run_chip("chop --freq 25kHz --fclk 16MHz");
    assert(rc == 0 && s.find("exact 4.25") != std::string::npos);
  }
  {
    auto [rc, s] = run_chip("chop --toff 0 --fclk 12MHz");
    assert(rc == 1 && s.find("driver off") != std::string::npos);
  }
  {
    auto [rc, s] = run_chip("chop --toff 3 --freq 25kHz --fclk 12MHz");
    assert(rc == 2);
  }
  {
    auto [rc, s] = run_chip("hyst --hend 6 --hstrt 0");
    assert(rc == 0 && s.find("Effective hysteresis = 4") != std::string::npos);
  }
  {
    auto [rc, s] = run_chip("hyst --hend 15 --hstrt 7");
    assert(rc == 1 && s.find("FAIL") != std::string::npos); // 20 > 16
  }
  {
    auto [rc, s] = run_chip("hyst --hend 6");
    assert(rc == 2);
  }
  {
    auto [rc, s] = run_chip("pwmfreq --fclk 12MHz --sel 1");
    assert(rc == 0 && s.find("35.1391 kHz") != std::string::npos);
  }
  {
    auto [rc, s] = run_chip("pwmfreq");
    assert(rc == 0 && s.find("PWM_FREQ = 3") != std::string::npos);
  }
  {
    auto [rc, s] = run_chip("pwmfreq --sel 4");
    assert(rc == 2);
  }
}

void test_cli_velocity_stealth_grad() {
  {
    auto [rc, s] = run_chip("velocity --tpwmthrs 500 --fclk 12MHz");
    assert(rc == 0);
    assert(s.find("93.75 fullsteps/s") != std::string::npos);
    assert(s.find("28.125 rpm") != std::string::npos);
  }
  {
    auto [rc, s] = run_chip("velocity --vactual 1000 --fclk 12MHz");
    assert(rc == 0 && s.find("715.256 usteps/s") != std::string::npos);
  }
  {
    auto [rc, s] = run_chip("velocity --vel 24kHz --fclk 12MHz");
    assert(rc == 0 && s.find("24000 usteps/s") != std::string::npos);
  }
  {
    auto [rc, s] = run_chip("velocity --vel 28.125rpm --fsc 200 --fclk 12MHz");
    assert(rc == 0);
    assert(s.find("VMOTOR = 0.46875 rps") != std::string::npos);
    assert(s.find("TSTEP = 500") != std::string::npos);
  }
  {
    auto [rc, s] = run_chip("velocity --vel 0.5rps --fsc 200 --fclk 12MHz");
    assert(rc == 0 && s.find("TSTEP = 468.75") != std::string::npos);
  }
  {
    auto [rc, s] = run_chip("velocity --vel 24kHz --vactual 1 --fclk 12MHz");
    assert(rc == 2); // mutually exclusive
  }
  {
    auto [rc, s] = run_chip("velocity --tstep 500 --vactual 1 --fclk 12MHz");
    assert(rc == 2); // mutually exclusive
  }
  {
    auto [rc, s] = run_chip("velocity --fclk 12MHz");
    assert(rc == 2);
  }
  {
    auto [rc, s] = run_chip("stealth --vm 24V --rcoil 5Ohm --icoil 0.5A --tbl 1");
    assert(rc == 0);
    assert(s.find("ILowerLimit = 225 mA") != std::string::npos);
    assert(s.find("PWM_AMPL seed = 38.9583") != std::string::npos);
  }
  {
    auto [rc, s] = run_chip("stealth --vm 24V --rcoil 5Ohm --icoil 0.1A --tbl 1");
    assert(rc == 1 && s.find("CHECK") != std::string::npos); // below 225mA
  }
  {
    auto [rc, s] = run_chip("stealth --vm 24V --rcoil 5Ohm");
    assert(rc == 2);
  }
  {
    auto [rc, s] = run_chip("grad --torque 0.4 --icoilnom 1A --vm 24V");
    assert(rc == 0);
    assert(s.find("CBEMF = 0.2") != std::string::npos);
    assert(s.find("PWM_GRAD seed = 17.9169") != std::string::npos);
    assert(s.find("OTP_PWM_GRAD[2] = 18") != std::string::npos);
  }
  {
    auto [rc, s] = run_chip("grad --cbemf 0.2 --vm 24V");
    assert(rc == 0 && s.find("PWM_GRAD seed = 17.9169") != std::string::npos);
  }
  {
    auto [rc, s] = run_chip("grad --cbemf 0.2 --torque 0.4 --vm 24V");
    assert(rc == 2);
  }
  {
    auto [rc, s] = run_chip("grad --vm 24V");
    assert(rc == 2);
  }
}

void test_cli_standby_thermal_mres() {
  {
    auto [rc, s] = run_chip("standby --tpowerdown 20 --fclk 12MHz");
    assert(rc == 0);
    assert(s.find("TPOWERDOWN = 20 -> 0.436907 s") != std::string::npos);
    assert(s.find("Total to hold current = 1.83501 s") != std::string::npos);
  }
  {
    auto [rc, s] = run_chip("standby --tpowerdown 1 --fclk 12MHz");
    assert(rc == 0 && s.find("WARN") != std::string::npos); // < 2 blocks AT#1
  }
  {
    auto [rc, s] = run_chip("standby --fclk 12MHz");
    assert(rc == 2);
  }
  {
    auto [rc, s] = run_chip("thermal --irms 1A");
    assert(rc == 0);
    assert(s.find("Pcond = 1.14 W") != std::string::npos);
    assert(s.find("TJ = 59.2 C") != std::string::npos);
  }
  {
    auto [rc, s] = run_chip("thermal --irms 2A --ta 85");
    assert(rc == 1 && s.find("FAIL") != std::string::npos);
  }
  {
    auto [rc, s] = run_chip("thermal");
    assert(rc == 2);
  }
  {
    auto [rc, s] = run_chip("mres --code 4");
    assert(rc == 0 && s.find("16 microsteps") != std::string::npos);
  }
  {
    auto [rc, s] = run_chip("mres --usteps 32");
    assert(rc == 0 && s.find("MRES = 3") != std::string::npos);
  }
  {
    auto [rc, s] = run_chip("mres --pins 220x --ms1 0 --ms2 0");
    assert(rc == 0 && s.find("-> 8 microsteps") != std::string::npos);
  }
  {
    auto [rc, s] = run_chip("mres --pins 222x --ms1 1 --ms2 1");
    assert(rc == 0 && s.find("-> 32 microsteps") != std::string::npos);
  }
  {
    auto [rc, s] = run_chip("mres --usteps 20");
    assert(rc == 1 && s.find("FAIL") != std::string::npos);
  }
  {
    auto [rc, s] = run_chip("mres --pins 220x --ms1 2 --ms2 0");
    assert(rc == 2);
  }
  {
    auto [rc, s] = run_chip("mres --code 9");
    assert(rc == 2);
  }
}

void test_cli_otp_uart_crc_spec() {
  {
    auto [rc, s] = run_chip("otp --pwmgrad 18");
    assert(rc == 0 && s.find("OTP_PWM_GRAD[2] = 18") != std::string::npos);
  }
  {
    auto [rc, s] = run_chip("otp --tpwmthrs 500");
    assert(rc == 0 && s.find("OTP_TPWMTHRS[4] = 500") != std::string::npos);
  }
  {
    auto [rc, s] = run_chip("otp --ihold 16");
    assert(rc == 0 && s.find("code %0") != std::string::npos);
  }
  {
    auto [rc, s] = run_chip("otp --iholddelay 4");
    assert(rc == 0 && s.find("code %2") != std::string::npos);
  }
  {
    auto [rc, s] = run_chip("otp --ihold 17");
    assert(rc == 1 && s.find("FAIL") != std::string::npos);
  }
  {
    auto [rc, s] = run_chip("otp");
    assert(rc == 2);
  }
  {
    auto [rc, s] = run_chip("uart --fclk 12MHz --senddelay 15");
    assert(rc == 0);
    assert(s.find("750000 Bd") != std::string::npos);
    assert(s.find("120 bit times") != std::string::npos);
  }
  {
    auto [rc, s] = run_chip("crc 05 00");
    assert(rc == 0 && s.find("0x18 (24)") != std::string::npos);
  }
  {
    auto [rc, s] = run_chip("crc 0x05 0x00 0x6C 0x10 0x00 0x00 0x53");
    assert(rc == 0 && s.find("0xFE (254)") != std::string::npos);
  }
  {
    auto [rc, s] = run_chip("crc 0xZZ");
    assert(rc == 2);
  }
  {
    auto [rc, s] = run_chip("crc");
    assert(rc == 2);
  }
  {
    auto [rc, s] = run_chip("spec");
    assert(rc == 0 && s.find("TMC2202") != std::string::npos);
  }
  {
    auto [rc, s] = run_chip("bogus");
    assert(rc == 2);
  }
  {
    auto [rc, s] = run_chip("");
    assert(rc == 2);
  }
  {
    auto [rc, s] = run_chip("--help");
    assert(rc == 0 && s.find("Usage") != std::string::npos);
  }
}

void test_cli_complete_shell() {
  static_assert(IcRegistry::find("tmc22xx") != nullptr);
  {
    std::vector<std::string> out;
    const std::array<std::string_view, 1> prior{"tmc22xx"};
    ic::complete<IcRegistry>(prior, "", out);
    const std::vector<std::string> want{"current", "rsense",  "rdson",  "chop",
                                        "hyst",    "pwmfreq", "velocity", "stealth",
                                        "grad",    "standby", "thermal", "mres",
                                        "otp",     "uart",    "crc",     "spec"};
    assert(out == want);
  }
  {
    std::vector<std::string> out;
    const std::array<std::string_view, 1> prior{"tmc22xx"};
    ic::complete<IcRegistry>(prior, "c", out);
    const std::vector<std::string> want{"current", "chop", "crc"};
    assert(out == want);
  }
  {
    std::vector<std::string> out;
    const std::array<std::string_view, 1> prior{"tmc22xx"};
    ic::complete<IcRegistry>(prior, "--", out);
    assert(!out.empty() && out.front() == "--rsense");
  }
  {
    Shell sh;
    sh.set_history_file("");
    std::ostringstream out;
    assert(sh.execute("ic tmc22xx spec", out) == 0);
    assert(out.str().find("TMC2202") != std::string::npos);
    assert(sh.execute("ic tmc22xx current --rsense 220mOhm", out) == 0);
    assert(sh.execute("ic tmc22xx bogus", out) == 2);
  }
}

} // namespace

int main() {
  test_current_eqs();
  test_rdson_eq();
  test_chop_eqs();
  test_velocity_eqs();
  test_stealth_eqs();
  test_standby_thermal_uart();
  test_cli_current_rsense();
  test_cli_rdson_chop_hyst();
  test_cli_velocity_stealth_grad();
  test_cli_standby_thermal_mres();
  test_cli_otp_uart_crc_spec();
  test_cli_complete_shell();
  std::cout << "test_tmc22xx: OK\n";
  return 0;
}
