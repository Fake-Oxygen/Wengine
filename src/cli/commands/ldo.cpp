#include "wengine/cli/commands.hpp"
#include "wengine/cli/status.hpp"
#include "wengine/solvers/ldo.hpp"

#include <array>
#include <limits>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wengine::cli::commands {

namespace {

int usage_error(std::ostream &out, std::string_view msg) {
  return cli::usage_error(out, msg, LdoSpec::kUsage);
}

void push_matches(std::string_view partial, std::span<const std::string_view> options, std::vector<std::string> &out) {
  for (std::string_view o : options) {
    if (o.substr(0, partial.size()) == partial) {
      out.emplace_back(o);
    }
  }
}

constexpr std::array<std::string_view, 10> kLdoOptions{{
    "--vin", "--vout", "--iout", "--iq", "--vdo", "--ta", "--rth", "--tjmax", "--help", "-h",
}};

} // namespace

int LdoSpec::run(ArgCursor &args, std::ostream &out) {
  if (args.take_flag({"--help", "-h"})) {
    out << kName << " - " << kDescription << "\nUsage:\n" << kUsage << '\n';
    return core::kExitOk;
  }

  using S = solvers::LdoSolver;

  std::optional<double> vin_v;
  std::optional<double> vout_v;
  std::optional<double> iout_a;
  double iq_a = 0.0;
  double vdo_v = 0.0;
  double ta_c = 25.0;
  double rth_cpw = 0.0;
  double tjmax_c = std::numeric_limits<double>::infinity();
  bool has_tjmax = false;

  if (auto o = args.take_option({"--vin"})) {
    auto p = parse_voltage(*o);
    if (!p.ok()) {
      return usage_error(out, "invalid VIN value");
    }
    vin_v = p.value;
  }
  if (auto o = args.take_option({"--vout"})) {
    auto p = parse_voltage(*o);
    if (!p.ok()) {
      return usage_error(out, "invalid VOUT value");
    }
    vout_v = p.value;
  }
  if (auto o = args.take_option({"--iout"})) {
    auto p = parse_current(*o);
    if (!p.ok()) {
      return usage_error(out, "invalid IOUT value");
    }
    iout_a = p.value;
  }
  if (auto o = args.take_option({"--iq"})) {
    auto p = parse_current(*o);
    if (!p.ok()) {
      return usage_error(out, "invalid Iq value (e.g. 50uA)");
    }
    iq_a = p.value;
  }
  if (auto o = args.take_option({"--vdo"})) {
    auto p = parse_voltage(*o);
    if (!p.ok()) {
      return usage_error(out, "invalid Vdo value");
    }
    vdo_v = p.value;
  }
  if (auto o = args.take_option({"--ta"})) {
    auto p = parse_double(*o);
    if (!p.ok()) {
      return usage_error(out, "invalid TA value (C, e.g. 85)");
    }
    ta_c = p.value;
  }
  if (auto o = args.take_option({"--rth"})) {
    auto p = parse_double(*o);
    if (!p.ok()) {
      return usage_error(out, "invalid Rth value (C/W, e.g. 100)");
    }
    rth_cpw = p.value;
  }
  if (auto o = args.take_option({"--tjmax"})) {
    auto p = parse_double(*o);
    if (!p.ok()) {
      return usage_error(out, "invalid Tjmax value (C, e.g. 125)");
    }
    tjmax_c = p.value;
    has_tjmax = true;
  }

  // Named-options only: VIN/VOUT share the V suffix, so bare/positional
  // voltages would be ambiguous. Stray positionals are errors, never ignored.
  if (auto p = args.next_positional()) {
    return cli::unexpected_error(out, *p, kUsage);
  }
  if (args.has_remaining()) {
    return cli::unexpected_error(out, *args.first_remaining(), kUsage);
  }

  if (!vin_v || !vout_v || !iout_a) {
    return usage_error(out, "need --vin, --vout and --iout");
  }
  if (*vin_v <= 0.0) {
    return usage_error(out, "VIN must be > 0");
  }
  if (*vout_v <= 0.0) {
    return usage_error(out, "VOUT must be > 0");
  }
  if (*iout_a < 0.0) {
    return usage_error(out, "IOUT must be >= 0");
  }
  if (iq_a < 0.0) {
    return usage_error(out, "Iq must be >= 0");
  }
  if (vdo_v < 0.0) {
    return usage_error(out, "Vdo must be >= 0");
  }
  if (rth_cpw < 0.0) {
    return usage_error(out, "Rth must be >= 0");
  }

  const auto r = S::solve(*vin_v, *vout_v, *iout_a, iq_a, vdo_v, ta_c, rth_cpw, tjmax_c);

  out << "VIN = " << *vin_v << " V, VOUT = " << *vout_v << " V, IOUT = " << *iout_a << " A\n";
  out << "headroom = " << r.headroom_v << " V (min VIN " << r.vin_min_v << " V for Vdo "
      << vdo_v << " V) " << (r.dropout_ok ? "OK" : "FAIL") << '\n';
  out << "IIN = " << r.iin_a << " A (Iq " << iq_a << " A)\n";
  out << "POUT = " << r.pout_w << " W, PIN = " << r.pin_w << " W\n";
  out << "P pass = " << r.p_pass_w << " W, P q = " << r.p_q_w << " W, P total = " << r.p_total_w
      << " W\n";
  if (r.pin_w == 0.0) {
    out << "eff = N/A (no input power)\n";
  } else {
    out << "eff = " << r.eff * 100.0 << " %\n";
  }
  out << "TJ = " << r.tj_c << " C at TA = " << ta_c << " C (Rth " << rth_cpw << " C/W)";
  if (has_tjmax) {
    out << " (limit " << tjmax_c << " C) " << (r.thermal_ok ? "OK" : "FAIL") << '\n';
  } else {
    out << " (no Tj limit)\n";
  }
  out << "summary: " << (r.pass ? "PASS" : "FAIL") << '\n';
  return r.pass ? core::kExitOk : core::kExitRuntime;
}

void LdoSpec::complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out) {
  (void)prior_args;
  // All options take numeric values: complete option names only.
  if (!partial.empty() && partial.front() == '-') {
    push_matches(partial, kLdoOptions, out);
  }
  // Bare voltages/currents/temperatures are numeric: nothing to complete.
}

} // namespace wengine::cli::commands
