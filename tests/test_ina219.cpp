#include "wengine/cli/command_registry.hpp"
#include "wengine/cli/ic/ina219.hpp"
#include "wengine/cli/ic_dispatch.hpp"
#include "wengine/cli/ic_registry.hpp"
#include "wengine/cli/shell.hpp"
#include "wengine/solvers/ic/ina219.hpp"

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
using S = wengine::solvers::ic::Ina219Solver;

namespace {

constexpr bool near(double a, double b, double eps = 1e-9) {
  return std::fabs(a - b) <= eps * (1.0 + std::fabs(b));
}

void test_constants() {
  static_assert(S::kShuntLsbV == 10e-6);
  static_assert(S::kBusLsbV == 4e-3);
  static_assert(S::kConfigReset == 0x399Fu);
  static_assert(S::kCalMax == 0xFFFEu);
  static_assert(S::config_word(1, 3, 3, 3, 7) == 0x399Fu);
  static_assert(S::adc_time_us_bits(12) == 532.0);
  static_assert(S::adc_time_us_avg(128) == 68100.0);
  assert(S::kVbusAbsMaxV == 26.0);
}

void test_calibration() {
  // Eq.2 golden (Sec. 9.2.2): 15A -> 15/32768 = 457.763671875uA.
  assert(near(S::min_lsb_a(15.0), 457.763671875e-6, 1e-12));
  // Eq.1 golden: 1mA, 2mOhm -> trunc(0.04096/2e-6) = 20480 (5000h).
  assert(S::cal(0.001, 0.002) == 20480u);
  // Eq.3 golden: 20 * 1mA = 20mW.
  assert(near(S::power_lsb_w(0.001), 0.02, 1e-12));
  assert(near(S::max_current_a(0.001), 32.767, 1e-12));
  assert(near(S::max_power_w(0.02), 1310.7, 1e-9));

  const auto r = S::solve_calibration(15.0, 0.002, 12.0, 0.001);
  assert(near(r.lsb_min_a, 457.763671875e-6, 1e-12));
  assert(r.lsb_used_a == 0.001);
  assert(r.cal == 20480u);
  assert(near(r.power_lsb_w, 0.02, 1e-12));
  assert(r.pga_idx == 0); // 30mV FS fits /1 (40mV)
  assert(r.brng == 0);    // 12V fits the 16V range
  assert(near(r.vshunt_fs_v, 0.03, 1e-12));
  assert(r.ok);

  const auto auto_lsb = S::solve_calibration(15.0, 0.002, 12.0, 0.0);
  assert(auto_lsb.lsb_used_a == auto_lsb.lsb_min_a);
  assert(auto_lsb.cal == (S::cal(auto_lsb.lsb_min_a, 0.002) & ~1u));
  assert(auto_lsb.ok);

  // Over-range shunt: 15A * 100mOhm = 1.5V exceeds the 320mV PGA.
  const auto over = S::solve_calibration(15.0, 0.1, 12.0, 0.001);
  assert(over.pga_idx == -1 && !over.shunt_ok && !over.ok);

  // Bus beyond the 26V absolute maximum fails even inside the 32V ADC range.
  const auto hot = S::solve_calibration(15.0, 0.002, 30.0, 0.001);
  assert(hot.brng == 1 && !hot.bus_ok && !hot.ok);

  // Degenerate inputs never divide by zero: all flags stay false.
  const auto bad = S::solve_calibration(15.0, 0.0, 12.0, 0.001);
  assert(!bad.ok && bad.cal == 0);
}

void test_shunt_bus() {
  // Shunt goldens: 20mV -> reg 2000 (07D0h), LSB 10uV.
  assert(S::shunt_reg(0.02) == 2000);
  assert(near(S::shunt_v_from_reg(2000), 0.02, 1e-12));
  assert(near(S::shunt_v(10.0, 0.002), 0.02, 1e-12));
  assert(S::shunt_reg(-0.04) == -4000);
  // PGA picks: 20mV -> /1, 41mV -> /2, 0.5V -> none.
  assert(S::pga_for_shunt(0.02) == 0);
  assert(S::pga_for_shunt(0.04) == 0);
  assert(S::pga_for_shunt(0.041) == 1);
  assert(S::pga_for_shunt(0.16) == 2);
  assert(S::pga_for_shunt(0.32) == 3);
  assert(S::pga_for_shunt(0.321) == -1);
  assert(near(S::pga_range_v(0), 40e-3));

  // Bus goldens: 11.98V -> shifted 2995 (BB3h), raw 23960 (5D98h).
  assert(S::bus_shifted(11.98) == 2995);
  assert(S::bus_raw_from_shifted(2995) == 0x5D98);
  assert(S::bus_shifted_from_raw(0x5D98) == 2995);
  assert(near(S::bus_v_from_shifted(2995), 11.98, 1e-12));
  assert(S::brng_for_bus(12.0) == 0);
  assert(S::brng_for_bus(16.0) == 0);
  assert(S::brng_for_bus(24.0) == 1);
  assert(S::brng_for_bus(33.0) == -1);
}

void test_readout() {
  // Eq.4 golden: 2000 * 20480 / 4096 = 10000 -> 10.0A at 1mA LSB.
  assert(S::current_reg(2000, 20480u) == 10000);
  assert(near(S::current_a(10000, 0.001), 10.0));
  // Eq.5 golden: 10000 * 2995 / 5000 = 5990 -> 119.8W at 20mW LSB.
  assert(S::power_reg(10000, 2995) == 5990);
  assert(near(S::power_w(5990, 0.02), 119.8, 1e-9));

  const auto r = S::solve_readout(2000, 0x5D98, 20480u, 0.001);
  assert(near(r.vshunt_v, 0.02, 1e-12));
  assert(near(r.vbus_v, 11.98, 1e-12));
  assert(r.current_reg == 10000 && near(r.current_a, 10.0));
  assert(r.power_reg == 5990 && near(r.power_w, 119.8, 1e-9));
}

void test_correct_adc_config() {
  // Eq.6 golden: trunc(20480 * 10 / 9.9) = trunc(20686.86..) = 20686.
  assert(S::correct_cal(20480u, 10.0, 9.9) == 20686u);
  assert(S::correct_cal(20480u, 10.0, 10.0) == 20480u);
  assert(S::correct_cal(20480u, 10.0, 0.0) == 0); // no divide by zero

  // Table 5 spot checks.
  assert(S::adc_time_us_bits(9) == 84.0);
  assert(S::adc_time_us_bits(10) == 148.0);
  assert(S::adc_time_us_bits(11) == 276.0);
  assert(S::adc_time_us_bits(8) == -1.0);
  assert(S::adc_time_us_avg(1) == 532.0);
  assert(S::adc_time_us_avg(2) == 1060.0);
  assert(S::adc_time_us_avg(3) == -1.0);

  // Config word: BRNG16/PGA40/avg128/mode-bus -> 0x05DE.
  assert(S::config_word(0, 0, 11, 11, 6) == 0x05DEu);
}

std::pair<int, std::string> run_chip(std::string line) {
  std::array<std::string_view, kMaxArgs> buf{};
  const std::size_t n = tokenize_into(line, buf);
  const std::size_t m = n < buf.size() ? n : buf.size();
  std::span<const std::string_view> args{buf.data(), m};
  ArgCursor cur{args};
  std::ostringstream out;
  const int rc = ic::Ina219Spec::run(cur, out);
  return {rc, out.str()};
}

void test_cli_cal() {
  {
    auto [rc, s] = run_chip("cal --imax 15A --rshunt 2mOhm --lsb 1mA --vbus 12V");
    assert(rc == 0);
    assert(s.find("Cal = 20480 (0x5000)") != std::string::npos);
    assert(s.find("PGA = /1") != std::string::npos);
    assert(s.find("BRNG = 16V") != std::string::npos);
    assert(s.find("Max current = 32.767 A") != std::string::npos);
    assert(s.find("Max power = 1310.7 W") != std::string::npos);
    assert(s.find("calibration: PASS") != std::string::npos);
  }
  {
    // Auto LSB: the Eq.2 minimum drives Cal.
    auto [rc, s] = run_chip("cal --imax 15A --rshunt 2mOhm --vbus 12V");
    assert(rc == 0 && s.find("LSB(min) = 457.764") != std::string::npos);
  }
  {
    // 1.5V shunt drop exceeds every PGA range: check failure, exit 1.
    auto [rc, s] = run_chip("cal --imax 15A --rshunt 100mOhm --vbus 12V");
    assert(rc == 1 && s.find("FAIL") != std::string::npos);
  }
  {
    // 30V bus exceeds the 26V absolute maximum: exit 1.
    auto [rc, s] = run_chip("cal --imax 15A --rshunt 2mOhm --vbus 30V");
    assert(rc == 1 && s.find("absolute maximum") != std::string::npos);
  }
  {
    auto [rc, s] = run_chip("cal --imax 15A");
    assert(rc == 2); // --rshunt required
  }
  {
    auto [rc, s] = run_chip("cal --imax 0A --rshunt 2mOhm");
    assert(rc == 2);
  }
  {
    auto [rc, s] = run_chip("cal --imax 15A --rshunt 2mOhm extra");
    assert(rc == 2);
  }
}

void test_cli_shunt_bus() {
  {
    auto [rc, s] = run_chip("shunt --vshunt 20mV");
    assert(rc == 0);
    assert(s.find("ShuntReg = 2000 (0x07D0") != std::string::npos);
    assert(s.find("PGA = /1") != std::string::npos);
  }
  {
    auto [rc, s] = run_chip("shunt --current 10A --rshunt 2mOhm");
    assert(rc == 0 && s.find("ShuntReg = 2000 (0x07D0") != std::string::npos);
  }
  {
    auto [rc, s] = run_chip("shunt --vshunt 400mV");
    assert(rc == 1 && s.find("FAIL") != std::string::npos);
  }
  {
    auto [rc, s] = run_chip("shunt --vshunt 20mV --current 10A");
    assert(rc == 2); // mutually exclusive
  }
  {
    auto [rc, s] = run_chip("shunt --current 10A");
    assert(rc == 2); // --current needs --rshunt
  }
  {
    auto [rc, s] = run_chip("bus --vbus 11.98V");
    assert(rc == 0);
    assert(s.find("BusRaw = 23960 (0x5D98") != std::string::npos);
    assert(s.find("BRNG = 16V") != std::string::npos);
  }
  {
    auto [rc, s] = run_chip("bus --vbus 30V");
    assert(rc == 1 && s.find("absolute maximum") != std::string::npos);
  }
  {
    auto [rc, s] = run_chip("bus --vbus 40V");
    assert(rc == 1 && s.find("BRNG = none") != std::string::npos);
  }
  {
    auto [rc, s] = run_chip("bus");
    assert(rc == 2);
  }
}

void test_cli_decode_correct() {
  {
    // Section 9.2.2 register image decodes to 10A / 119.8W.
    auto [rc, s] = run_chip("decode --shunt 0x07D0 --bus 0x5D98 --cal 20480 --lsb 1mA");
    assert(rc == 0);
    assert(s.find("VSHUNT = 20 mV") != std::string::npos);
    assert(s.find("VBUS = 11.98 V") != std::string::npos);
    assert(s.find("CurrentReg = 10000 (0x2710), I = 10 A") != std::string::npos);
    assert(s.find("PowerReg = 5990 (0x1766), P = 119.8 W") != std::string::npos);
  }
  {
    // Decimal codes work too; 0xF060 sign-extends to -4000 (-40mV).
    auto [rc, s] = run_chip("decode --shunt 0xF060 --bus 23960 --cal 20480 --lsb 1mA");
    assert(rc == 0 && s.find("VSHUNT = -40 mV") != std::string::npos);
  }
  {
    auto [rc, s] = run_chip("decode --shunt 0x07D0 --bus 0x5D98 --cal 20481 --lsb 1mA");
    assert(rc == 2); // Cal FS0 is void (must be even)
  }
  {
    auto [rc, s] = run_chip("decode --shunt 0x07D0 --bus 0x5D98 --cal 20480");
    assert(rc == 2); // --lsb required
  }
  {
    auto [rc, s] = run_chip("correct --cal 20480 --imeas 10A --iina 9.9A");
    assert(rc == 0 && s.find("Cal(corr) = 20686 (0x50CE)") != std::string::npos);
  }
  {
    auto [rc, s] = run_chip("correct --cal 20480 --imeas 10A");
    assert(rc == 2);
  }
}

void test_cli_adc_config_spec() {
  {
    auto [rc, s] = run_chip("adc --avg 128");
    assert(rc == 0);
    assert(s.find("tconv = 68100 us") != std::string::npos);
    assert(s.find("tcycle(shunt+bus) = 136200 us") != std::string::npos);
  }
  {
    auto [rc, s] = run_chip("adc --bits 12");
    assert(rc == 0 && s.find("tconv = 532 us") != std::string::npos);
  }
  {
    auto [rc, s] = run_chip("adc --avg 3");
    assert(rc == 2);
  }
  {
    auto [rc, s] = run_chip("adc --bits 12 --avg 128");
    assert(rc == 2);
  }
  {
    auto [rc, s] = run_chip("config");
    assert(rc == 0 && s.find("CFG = 0x399F (reset default)") != std::string::npos);
  }
  {
    auto [rc, s] =
        run_chip("config --brng 16 --pga 40 --avg 128 --mode bus");
    assert(rc == 0 && s.find("CFG = 0x05DE") != std::string::npos);
  }
  {
    auto [rc, s] = run_chip("config --mode bogus");
    assert(rc == 2);
  }
  {
    auto [rc, s] = run_chip("config --pga 50");
    assert(rc == 2);
  }
  {
    auto [rc, s] = run_chip("spec");
    assert(rc == 0 && s.find("INA219") != std::string::npos);
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
  static_assert(IcRegistry::find("ina219") != nullptr);
  static_assert(IcRegistry::find("ap63205") != nullptr);
  {
    std::vector<std::string> out;
    const std::array<std::string_view, 1> prior{"ina219"};
    ic::complete<IcRegistry>(prior, "", out);
    const std::vector<std::string> want{"cal",     "shunt", "bus",  "decode",
                                        "correct", "adc",   "config", "spec"};
    assert(out == want);
  }
  {
    std::vector<std::string> out;
    const std::array<std::string_view, 1> prior{"ina219"};
    ic::complete<IcRegistry>(prior, "c", out);
    const std::vector<std::string> want{"cal", "correct", "config"};
    assert(out == want);
  }
  {
    std::vector<std::string> out;
    const std::array<std::string_view, 1> prior{"ina219"};
    ic::complete<IcRegistry>(prior, "--", out);
    assert(!out.empty() && out.front() == "--imax");
  }
  {
    Shell sh;
    sh.set_history_file("");
    std::ostringstream out;
    assert(sh.execute("ic ina219 spec", out) == 0);
    assert(out.str().find("INA219") != std::string::npos);
    assert(sh.execute("ic ina219 cal --imax 15A --rshunt 2mOhm --vbus 12V", out) == 0);
    assert(sh.execute("ic ina219 bogus", out) == 2);
  }
}

} // namespace

int main() {
  test_constants();
  test_calibration();
  test_shunt_bus();
  test_readout();
  test_correct_adc_config();
  test_cli_cal();
  test_cli_shunt_bus();
  test_cli_decode_correct();
  test_cli_adc_config_spec();
  test_cli_complete_shell();
  std::cout << "test_ina219: OK\n";
  return 0;
}
