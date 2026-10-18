#include "wengine/cli/ic/ina219.hpp"
#include "wengine/cli/status.hpp"
#include "wengine/solvers/ic/ina219.hpp"

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

using S = solvers::ic::Ina219Solver;

int usage_error(std::ostream &out, std::string_view msg) {
  return cli::usage_error(out, msg, Ina219Spec::kUsage);
}

int unexpected_error(std::ostream &out, std::string_view tok) {
  return cli::unexpected_error(out, tok, Ina219Spec::kUsage);
}

void print_hex4(std::ostream &out, int v) {
  char buf[8]{};
  const unsigned u = static_cast<unsigned>(v) & 0xFFFFu;
  std::snprintf(buf, sizeof buf, "0x%04X", u);
  out << buf;
}

void print_hex_cal(std::ostream &out, unsigned v) {
  char buf[8]{};
  std::snprintf(buf, sizeof buf, "0x%04X", v & 0xFFFFu);
  out << buf;
}

// Register codes accept 0x hex or decimal (0..65535, no sign, no suffix).
core::Result<int> parse_reg16(std::string_view s) noexcept {
  if (s.empty()) {
    return core::Result<int>::failure("expected register code, got empty");
  }
  std::string_view d = s;
  unsigned base = 10;
  if (d.size() > 2 && d[0] == '0' && (d[1] == 'x' || d[1] == 'X')) {
    base = 16;
    d = d.substr(2);
  }
  if (d.empty()) {
    return core::Result<int>::failure("invalid register code");
  }
  unsigned acc = 0;
  for (char c : d) {
    unsigned digit = 16;
    if (c >= '0' && c <= '9') {
      digit = static_cast<unsigned>(c - '0');
    } else if (base == 16 && c >= 'a' && c <= 'f') {
      digit = static_cast<unsigned>(c - 'a' + 10);
    } else if (base == 16 && c >= 'A' && c <= 'F') {
      digit = static_cast<unsigned>(c - 'A' + 10);
    } else {
      return core::Result<int>::failure("invalid register code");
    }
    if (digit >= base) {
      return core::Result<int>::failure("invalid register code");
    }
    acc = acc * base + digit;
    if (acc > 0xFFFFu) {
      return core::Result<int>::failure("register code out of range (0..0xFFFF)");
    }
  }
  return core::Result<int>::success(static_cast<int>(acc));
}

const char *pga_name(int idx) {
  switch (idx) {
    case 0:
      return "/1";
    case 1:
      return "/2";
    case 2:
      return "/4";
    default:
      return "/8";
  }
}

int run_cal(ArgCursor &args, std::ostream &out) {
  auto imax_raw = args.take_option({"--imax"});
  auto rshunt_raw = args.take_option({"--rshunt"});
  if (!imax_raw || !rshunt_raw) {
    return usage_error(out, "cal needs --imax and --rshunt");
  }
  auto imax = parse_current(*imax_raw);
  auto rshunt = parse_resistance(*rshunt_raw);
  if (!imax.ok() || !rshunt.ok()) {
    return usage_error(out, "invalid --imax/--rshunt value");
  }
  if (!(imax.value > 0.0) || !(rshunt.value > 0.0)) {
    return usage_error(out, "--imax and --rshunt must be > 0");
  }
  double lsb_req = 0.0;
  if (auto o = args.take_option({"--lsb"})) {
    auto p = parse_current(*o);
    if (!p.ok()) {
      return usage_error(out, "invalid --lsb value (e.g. 1mA)");
    }
    if (!(p.value > 0.0)) {
      return usage_error(out, "--lsb must be > 0");
    }
    lsb_req = p.value;
  }
  double vbus_max = S::kVbusAbsMaxV;
  if (auto o = args.take_option({"--vbus"})) {
    auto p = parse_voltage(*o);
    if (!p.ok()) {
      return usage_error(out, "invalid --vbus value");
    }
    if (!(p.value >= 0.0)) {
      return usage_error(out, "--vbus must be >= 0");
    }
    vbus_max = p.value;
  }
  if (args.has_remaining()) {
    return unexpected_error(out, *args.first_remaining());
  }
  const auto r = S::solve_calibration(imax.value, rshunt.value, vbus_max, lsb_req);
  out << "IMAX = " << imax.value << " A, RSHUNT = " << rshunt.value << " Ohm\n";
  out << "VSHUNT(FS) = " << r.vshunt_fs_v * 1e3 << " mV";
  if (r.shunt_ok) {
    out << ", PGA = " << pga_name(r.pga_idx) << " (FS " << S::pga_range_v(r.pga_idx) * 1e3
        << " mV) OK\n";
  } else {
    out << " (over 320 mV FS) FAIL\n";
  }
  out << "VBUS(max) = " << vbus_max << " V";
  if (r.brng >= 0) {
    out << ", BRNG = " << (r.brng == 0 ? "16V" : "32V");
  } else {
    out << ", BRNG = none (over 32V range)";
  }
  out << (r.bus_ok ? " OK\n" : " FAIL\n");
  if (vbus_max > S::kVbusAbsMaxV) {
    out << "WARN: above the 26V absolute maximum (Sec. 7.1)\n";
  }
  out << "LSB(min) = " << r.lsb_min_a * 1e6 << " uA";
  if (lsb_req > 0.0 && lsb_req < r.lsb_min_a) {
    out << " WARN: --lsb below the Eq.2 minimum";
  }
  out << '\n';
  out << "LSB(used) = " << r.lsb_used_a * 1e6 << " uA\n";
  out << "Cal = " << r.cal << " (";
  print_hex_cal(out, r.cal);
  out << ")" << (r.cal_ok ? " OK\n" : " FAIL (must fit 1..0xFFFE)\n");
  out << "Power LSB = " << r.power_lsb_w * 1e3 << " mW\n";
  out << "Max current = " << r.max_current_a << " A (32767 * LSB)\n";
  out << "Max power = " << r.max_power_w << " W (65535 * PowerLSB)\n";
  out << "calibration: " << (r.ok ? "PASS" : "FAIL") << '\n';
  return r.ok ? core::kExitOk : core::kExitRuntime;
}

int run_shunt(ArgCursor &args, std::ostream &out) {
  auto vshunt_raw = args.take_option({"--vshunt"});
  auto current_raw = args.take_option({"--current"});
  auto rshunt_raw = args.take_option({"--rshunt"});
  if (static_cast<bool>(vshunt_raw) == static_cast<bool>(current_raw)) {
    return usage_error(out, "shunt needs either --vshunt or --current/--rshunt");
  }
  double vshunt = 0.0;
  if (vshunt_raw) {
    if (rshunt_raw) {
      return usage_error(out, "--rshunt only applies with --current");
    }
    auto p = parse_voltage(*vshunt_raw);
    if (!p.ok()) {
      return usage_error(out, "invalid --vshunt value");
    }
    vshunt = p.value;
  } else {
    if (!rshunt_raw) {
      return usage_error(out, "--current needs --rshunt");
    }
    auto ci = parse_current(*current_raw);
    auto rs = parse_resistance(*rshunt_raw);
    if (!ci.ok() || !rs.ok()) {
      return usage_error(out, "invalid --current/--rshunt value");
    }
    if (!(rs.value > 0.0)) {
      return usage_error(out, "--rshunt must be > 0");
    }
    vshunt = S::shunt_v(ci.value, rs.value);
    out << "I = " << ci.value << " A, RSHUNT = " << rs.value << " Ohm\n";
  }
  if (args.has_remaining()) {
    return unexpected_error(out, *args.first_remaining());
  }
  const double vabs = (vshunt >= 0.0) ? vshunt : -vshunt;
  const int pga = S::pga_for_shunt(vabs);
  const int reg = S::shunt_reg(vshunt);
  out << "VSHUNT = " << vshunt * 1e3 << " mV\n";
  out << "ShuntReg = " << reg << " (";
  print_hex4(out, reg);
  out << ", LSB 10 uV)\n";
  if (pga >= 0) {
    out << "PGA = " << pga_name(pga) << " (FS " << S::pga_range_v(pga) * 1e3 << " mV) OK\n";
    return core::kExitOk;
  }
  out << "PGA = none (over 320 mV FS) FAIL\n";
  return core::kExitRuntime;
}

int run_bus(ArgCursor &args, std::ostream &out) {
  auto vbus_raw = args.take_option({"--vbus"});
  if (!vbus_raw) {
    return usage_error(out, "bus needs --vbus");
  }
  auto p = parse_voltage(*vbus_raw);
  if (!p.ok()) {
    return usage_error(out, "invalid --vbus value");
  }
  if (!(p.value >= 0.0)) {
    return usage_error(out, "--vbus must be >= 0");
  }
  if (args.has_remaining()) {
    return unexpected_error(out, *args.first_remaining());
  }
  const double vbus = p.value;
  const int shifted = S::bus_shifted(vbus);
  const int raw = S::bus_raw_from_shifted(shifted);
  const int brng = S::brng_for_bus(vbus);
  out << "VBUS = " << vbus << " V\n";
  out << "BusShifted = " << shifted << ", BusRaw = " << raw << " (";
  print_hex4(out, raw);
  out << ", LSB 4 mV)\n";
  bool ok = true;
  if (brng >= 0) {
    out << "BRNG = " << (brng == 0 ? "16V" : "32V") << " OK\n";
  } else {
    out << "BRNG = none (over 32V range) FAIL\n";
    ok = false;
  }
  if (vbus > S::kVbusAbsMaxV) {
    out << "WARN: above the 26V absolute maximum (Sec. 7.1) FAIL\n";
    ok = false;
  }
  return ok ? core::kExitOk : core::kExitRuntime;
}

int run_decode(ArgCursor &args, std::ostream &out) {
  auto shunt_raw = args.take_option({"--shunt"});
  auto bus_raw = args.take_option({"--bus"});
  auto cal_raw = args.take_option({"--cal"});
  auto lsb_raw = args.take_option({"--lsb"});
  if (!shunt_raw || !bus_raw || !cal_raw || !lsb_raw) {
    return usage_error(out, "decode needs --shunt --bus --cal --lsb");
  }
  auto sh = parse_reg16(*shunt_raw);
  auto bu = parse_reg16(*bus_raw);
  auto ca = parse_reg16(*cal_raw);
  auto lsb = parse_current(*lsb_raw);
  if (!sh.ok() || !bu.ok() || !ca.ok()) {
    return usage_error(out, "invalid register code (0x hex or decimal, 0..0xFFFF)");
  }
  if (!lsb.ok() || !(lsb.value > 0.0)) {
    return usage_error(out, "invalid --lsb value (must be > 0)");
  }
  if (ca.value % 2 != 0) {
    return usage_error(out, "Cal FS0 is void (value must be even)");
  }
  if (args.has_remaining()) {
    return unexpected_error(out, *args.first_remaining());
  }
  // Shunt register is signed (two's complement); bus raw is unsigned.
  const int shunt_reg_value =
      (sh.value > 0x7FFF) ? sh.value - 0x10000 : sh.value;
  const auto r = S::solve_readout(shunt_reg_value, bu.value,
                                  static_cast<unsigned>(ca.value), lsb.value);
  out << "VSHUNT = " << r.vshunt_v * 1e3 << " mV (ShuntReg = " << shunt_reg_value << ")\n";
  out << "VBUS = " << r.vbus_v << " V (BusRaw = ";
  print_hex4(out, bu.value);
  out << ", shifted = " << S::bus_shifted_from_raw(bu.value) << ")\n";
  out << "CurrentReg = " << r.current_reg << " (";
  print_hex4(out, r.current_reg);
  out << "), I = " << r.current_a << " A\n";
  out << "PowerReg = " << r.power_reg << " (";
  print_hex4(out, r.power_reg);
  out << "), P = " << r.power_w << " W (LSB " << S::power_lsb_w(lsb.value) * 1e3 << " mW)\n";
  return core::kExitOk;
}

int run_correct(ArgCursor &args, std::ostream &out) {
  auto cal_raw = args.take_option({"--cal"});
  auto imeas_raw = args.take_option({"--imeas"});
  auto iina_raw = args.take_option({"--iina"});
  if (!cal_raw || !imeas_raw || !iina_raw) {
    return usage_error(out, "correct needs --cal --imeas --iina");
  }
  auto ca = parse_reg16(*cal_raw);
  auto imeas = parse_current(*imeas_raw);
  auto iina = parse_current(*iina_raw);
  if (!ca.ok()) {
    return usage_error(out, "invalid --cal value (0x hex or decimal)");
  }
  if (!imeas.ok() || !iina.ok()) {
    return usage_error(out, "invalid --imeas/--iina value");
  }
  if (!(imeas.value > 0.0) || !(iina.value > 0.0)) {
    return usage_error(out, "--imeas and --iina must be > 0");
  }
  if (args.has_remaining()) {
    return unexpected_error(out, *args.first_remaining());
  }
  const unsigned corrected =
      S::correct_cal(static_cast<unsigned>(ca.value), imeas.value, iina.value);
  out << "Cal = " << ca.value << " (";
  print_hex_cal(out, static_cast<unsigned>(ca.value));
  out << ")\n";
  out << "Imeas = " << imeas.value << " A, Iina = " << iina.value << " A\n";
  out << "Cal(corr) = " << corrected << " (";
  print_hex_cal(out, corrected);
  if (corrected == 0 || corrected > S::kCalMax) {
    out << ") FAIL (must fit 1..0xFFFE)\n";
    return core::kExitRuntime;
  }
  out << ") OK\n";
  return core::kExitOk;
}

int run_adc(ArgCursor &args, std::ostream &out) {
  auto bits_raw = args.take_option({"--bits"});
  auto avg_raw = args.take_option({"--avg"});
  if (static_cast<bool>(bits_raw) == static_cast<bool>(avg_raw)) {
    return usage_error(out, "adc needs exactly one of --bits or --avg");
  }
  double tus = -1.0;
  if (bits_raw) {
    auto p = parse_i64(*bits_raw);
    if (!p.ok()) {
      return usage_error(out, "invalid --bits value (9|10|11|12)");
    }
    tus = S::adc_time_us_bits(static_cast<int>(p.value));
    if (!(tus > 0.0)) {
      return usage_error(out, "--bits must be 9|10|11|12");
    }
    out << "ADC = " << p.value << "-bit\n";
  } else {
    auto p = parse_i64(*avg_raw);
    if (!p.ok()) {
      return usage_error(out, "invalid --avg value (1|2|4|8|16|32|64|128)");
    }
    tus = S::adc_time_us_avg(static_cast<int>(p.value));
    if (!(tus > 0.0)) {
      return usage_error(out, "--avg must be 1|2|4|8|16|32|64|128");
    }
    out << "ADC = " << p.value << "-sample average\n";
  }
  if (args.has_remaining()) {
    return unexpected_error(out, *args.first_remaining());
  }
  out << "tconv = " << tus << " us (" << tus / 1e3 << " ms) per channel\n";
  out << "tcycle(shunt+bus) = " << 2.0 * tus << " us (" << 2.0 * tus / 1e3 << " ms)\n";
  return core::kExitOk;
}

core::Result<int> parse_pga_idx(std::string_view s) noexcept {
  auto p = parse_double(s);
  if (!p.ok()) {
    return core::Result<int>::failure("invalid --pga value (40|80|160|320)");
  }
  if (p.value == 40.0) {
    return core::Result<int>::success(0);
  }
  if (p.value == 80.0) {
    return core::Result<int>::success(1);
  }
  if (p.value == 160.0) {
    return core::Result<int>::success(2);
  }
  if (p.value == 320.0) {
    return core::Result<int>::success(3);
  }
  return core::Result<int>::failure("--pga must be 40|80|160|320 (mV FS)");
}

core::Result<int> parse_adc_code(std::string_view bits_raw, std::string_view avg_raw,
                                 bool have_bits) noexcept {
  if (have_bits) {
    auto p = parse_i64(bits_raw);
    if (!p.ok() || p.value < 9 || p.value > 12) {
      return core::Result<int>::failure("--bits must be 9|10|11|12");
    }
    return core::Result<int>::success(static_cast<int>(p.value - 9));
  }
  auto p = parse_i64(avg_raw);
  if (!p.ok()) {
    return core::Result<int>::failure("--avg must be 1|2|4|8|16|32|64|128");
  }
  const long long avg = p.value;
  if (avg == 1) {
    return core::Result<int>::success(4);
  }
  if (avg == 2) {
    return core::Result<int>::success(5);
  }
  if (avg == 4) {
    return core::Result<int>::success(6);
  }
  if (avg == 8) {
    return core::Result<int>::success(7);
  }
  if (avg == 16) {
    return core::Result<int>::success(8);
  }
  if (avg == 32) {
    return core::Result<int>::success(9);
  }
  if (avg == 64) {
    return core::Result<int>::success(10);
  }
  if (avg == 128) {
    return core::Result<int>::success(11);
  }
  return core::Result<int>::failure("--avg must be 1|2|4|8|16|32|64|128");
}

core::Result<int> parse_mode(std::string_view s) noexcept {
  if (s == "powerdown") {
    return core::Result<int>::success(0);
  }
  if (s == "shunt-trig") {
    return core::Result<int>::success(1);
  }
  if (s == "bus-trig") {
    return core::Result<int>::success(2);
  }
  if (s == "both-trig") {
    return core::Result<int>::success(3);
  }
  if (s == "adcoff") {
    return core::Result<int>::success(4);
  }
  if (s == "shunt") {
    return core::Result<int>::success(5);
  }
  if (s == "bus") {
    return core::Result<int>::success(6);
  }
  if (s == "both") {
    return core::Result<int>::success(7);
  }
  auto p = parse_i64(s);
  if (p.ok() && p.value >= 0 && p.value <= 7) {
    return core::Result<int>::success(static_cast<int>(p.value));
  }
  return core::Result<int>::failure(
      "--mode must be powerdown|shunt-trig|bus-trig|both-trig|adcoff|shunt|bus|both (or 0..7)");
}

const char *mode_name(int mode) {
  switch (mode) {
    case 0:
      return "power-down";
    case 1:
      return "shunt triggered";
    case 2:
      return "bus triggered";
    case 3:
      return "shunt+bus triggered";
    case 4:
      return "ADC off";
    case 5:
      return "shunt continuous";
    case 6:
      return "bus continuous";
    default:
      return "shunt+bus continuous";
  }
}

int run_config(ArgCursor &args, std::ostream &out) {
  int brng = 1;
  if (auto o = args.take_option({"--brng"})) {
    auto p = parse_i64(*o);
    if (!p.ok() || (p.value != 16 && p.value != 32)) {
      return usage_error(out, "--brng must be 16|32");
    }
    brng = (p.value == 32) ? 1 : 0;
  }
  int pga = 3;
  if (auto o = args.take_option({"--pga"})) {
    auto p = parse_pga_idx(*o);
    if (!p.ok()) {
      return usage_error(out, p.error);
    }
    pga = p.value;
  }
  auto bits_raw = args.take_option({"--bits"});
  auto avg_raw = args.take_option({"--avg"});
  if (bits_raw && avg_raw) {
    return usage_error(out, "use either --bits or --avg");
  }
  int adc_code = 3; // 12-bit default (reset value)
  if (bits_raw || avg_raw) {
    const std::string_view b = bits_raw ? *bits_raw : std::string_view{};
    const std::string_view a = avg_raw ? *avg_raw : std::string_view{};
    auto p = parse_adc_code(b, a, static_cast<bool>(bits_raw));
    if (!p.ok()) {
      return usage_error(out, p.error);
    }
    adc_code = p.value;
  }
  int mode = 7;
  if (auto o = args.take_option({"--mode"})) {
    auto p = parse_mode(*o);
    if (!p.ok()) {
      return usage_error(out, p.error);
    }
    mode = p.value;
  }
  if (args.has_remaining()) {
    return unexpected_error(out, *args.first_remaining());
  }
  const unsigned word = S::config_word(brng, pga, adc_code, adc_code, mode);
  out << "BRNG = " << (brng == 0 ? "16V" : "32V") << ", PGA = " << pga_name(pga) << " ("
      << S::pga_range_v(pga) * 1e3 << " mV)\n";
  out << "BADC = SADC = code " << adc_code << ", MODE = " << mode_name(mode) << '\n';
  out << "CFG = ";
  print_hex_cal(out, word);
  if (word == S::kConfigReset) {
    out << " (reset default)\n";
  } else {
    out << " (reset is 0x399F)\n";
  }
  return core::kExitOk;
}

int run_spec(ArgCursor &args, std::ostream &out) {
  if (args.has_remaining()) {
    return unexpected_error(out, *args.first_remaining());
  }
  out << "INA219: zero-drift bidirectional current/power monitor, I2C/SMBus, 16 addresses\n";
  out << "VS 3..5.5 V, VBUS 0..26 V (ADC ranges 16/32V), VSHUNT PGA /1 /2 /4 /8 (40..320 mV)\n";
  out << "LSBs: shunt 10 uV, bus 4 mV (D2:D0 = CNVR/OVF); Cal = trunc(0.04096/(LSB*R))\n";
  out << "PowerLSB = 20*LSB, CurrentReg = Shunt*Cal/4096, PowerReg = Current*Bus/5000\n";
  out << "CFG reset 0x399F, ADC 9..12 bit / avg 1..128 (84us..68.1ms per channel)\n";
  out << "Grades A (0.5% max) / B (higher accuracy); SOT23-8, SOIC-8; -40..+125 C\n";
  return core::kExitOk;
}

constexpr std::array<std::string_view, 8> kIna219Ops{{
    "cal",
    "shunt",
    "bus",
    "decode",
    "correct",
    "adc",
    "config",
    "spec",
}};

constexpr std::array<std::string_view, 16> kIna219Options{{
    "--imax", "--rshunt", "--lsb", "--vbus", "--vshunt", "--current", "--shunt", "--bus",
    "--cal", "--imeas", "--iina", "--bits", "--avg", "--brng", "--pga", "--mode",
}};

constexpr std::array<std::string_view, 2> kIna219Help{{"--help", "-h"}};

void push_matches(std::string_view partial, std::span<const std::string_view> options,
                  std::vector<std::string> &out) {
  for (std::string_view o : options) {
    if (o.substr(0, partial.size()) == partial) {
      out.emplace_back(o);
    }
  }
}

} // namespace

int Ina219Spec::run(ArgCursor &args, std::ostream &out) {
  if (args.take_flag({"--help", "-h"})) {
    out << kName << " - " << kDescription << "\nUsage:\n" << kUsage << '\n';
    return core::kExitOk;
  }
  std::optional<std::string_view> op = args.next_positional();
  if (!op) {
    return usage_error(out, "missing op (cal|shunt|bus|decode|correct|adc|config|spec)");
  }
  if (*op == "cal") {
    return run_cal(args, out);
  }
  if (*op == "shunt") {
    return run_shunt(args, out);
  }
  if (*op == "bus") {
    return run_bus(args, out);
  }
  if (*op == "decode") {
    return run_decode(args, out);
  }
  if (*op == "correct") {
    return run_correct(args, out);
  }
  if (*op == "adc") {
    return run_adc(args, out);
  }
  if (*op == "config") {
    return run_config(args, out);
  }
  if (*op == "spec") {
    return run_spec(args, out);
  }
  return usage_error(out, "unknown op (cal|shunt|bus|decode|correct|adc|config|spec)");
}

void Ina219Spec::complete(std::span<const std::string_view> prior_args, std::string_view partial,
                          std::vector<std::string> &out) {
  if (!partial.empty() && partial.front() == '-') {
    push_matches(partial, kIna219Options, out);
    push_matches(partial, kIna219Help, out);
    return;
  }
  if (prior_args.empty()) {
    push_matches(partial, kIna219Ops, out);
  }
  // Later positionals are numeric values: nothing to complete.
}

} // namespace wengine::cli::ic
