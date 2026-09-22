#include "wengine/cli/commands.hpp"
#include "wengine/cli/status.hpp"
#include "wengine/solvers/i2c.hpp"

#include <array>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wengine::cli::commands {

namespace {

[[nodiscard]] constexpr bool is_i2c_mode(std::string_view s) noexcept {
  return s == "pullup" || s == "series" || s == "full";
}

[[nodiscard]] constexpr bool ends_with(std::string_view s, std::string_view suf) noexcept {
  return s.size() >= suf.size() && s.substr(s.size() - suf.size()) == suf;
}

[[nodiscard]] constexpr bool looks_like_cap(std::string_view s) noexcept {
  return ends_with(s, "F");
}

[[nodiscard]] constexpr bool looks_like_res(std::string_view s) noexcept {
  return ends_with(s, "Ohm") || ends_with(s, "ohm") || ends_with(s, "R");
}

int usage_error(std::ostream &out, std::string_view msg) {
  return cli::usage_error(out, msg, I2cSpec::kUsage);
}

int unexpected_error(std::ostream &out, std::string_view tok) {
  return cli::unexpected_error(out, tok, I2cSpec::kUsage);
}

inline int runtime_error(std::ostream &out, std::string_view msg) {
  return cli::runtime_error(out, msg);
}

core::Result<solvers::I2cMode> parse_speed(std::string_view s) noexcept {
  using solvers::I2cMode;
  if (s == "std" || s == "standard") {
    return core::Result<I2cMode>::success(I2cMode::Standard);
  }
  if (s == "fast") {
    return core::Result<I2cMode>::success(I2cMode::Fast);
  }
  if (s == "fmp" || s == "fastplus" || s == "fast-plus" || s == "fm+") {
    return core::Result<I2cMode>::success(I2cMode::FastPlus);
  }
  return core::Result<I2cMode>::failure("unknown speed (std|fast|fmp)");
}

const char *speed_name(solvers::I2cMode m) noexcept {
  using solvers::I2cMode;
  if (m == I2cMode::FastPlus) {
    return "fmp";
  }
  if (m == I2cMode::Fast) {
    return "fast";
  }
  return "std";
}

void push_matches(std::string_view partial, std::span<const std::string_view> options,
                  std::vector<std::string> &out) {
  for (std::string_view o : options) {
    if (o.substr(0, partial.size()) == partial) {
      out.emplace_back(o);
    }
  }
}

constexpr std::array<std::string_view, 3> kI2cModes{{
    "pullup",
    "series",
    "full",
}};

constexpr std::array<std::string_view, 3> kI2cSpeeds{{
    "std",
    "fast",
    "fmp",
}};

constexpr std::array<std::string_view, 20> kI2cOptions{{
    "--vdd", "--cb", "--speed", "--vol", "--iol", "--tr", "--rp", "--rs", "--help", "-h",
    "--voltage", "--capacitance", "--pullup", "--series", "--resistance", "--current",
    "--low", "--time", "--rise", "--fall",
}};

} // namespace

int I2cSpec::run(ArgCursor &args, std::ostream &out) {
  if (args.take_flag({"--help", "-h"})) {
    out << kName << " - " << kDescription << "\nUsage:\n" << kUsage << '\n';
    return core::kExitOk;
  }

  using S = solvers::I2cBusSolver;
  using solvers::I2cMode;

  std::optional<double> vdd_v;
  std::optional<double> cb_f;
  std::optional<double> vol_v;
  std::optional<double> iol_a;
  std::optional<double> tr_s;
  std::optional<double> rp_ohm;
  std::optional<double> rs_ohm;
  bool has_rs = false;
  std::optional<std::string_view> speed_raw = args.take_option({"--speed"});

  if (auto o = args.take_option({"--vdd", "--voltage"})) {
    auto p = parse_voltage(*o);
    if (!p.ok()) {
      return usage_error(out, "invalid VDD value");
    }
    vdd_v = p.value;
  }
  if (auto o = args.take_option({"--cb", "--capacitance"})) {
    auto p = parse_capacitance(*o);
    if (!p.ok()) {
      return usage_error(out, "invalid bus capacitance (e.g. 200pF)");
    }
    cb_f = p.value;
  }
  if (auto o = args.take_option({"--vol", "--low"})) {
    auto p = parse_voltage(*o);
    if (!p.ok()) {
      return usage_error(out, "invalid VOL value");
    }
    vol_v = p.value;
  }
  if (auto o = args.take_option({"--iol", "--current"})) {
    auto p = parse_current(*o);
    if (!p.ok()) {
      return usage_error(out, "invalid IOL value");
    }
    iol_a = p.value;
  }
  if (auto o = args.take_option({"--tr", "--rise", "--time"})) {
    auto p = parse_time(*o);
    if (!p.ok()) {
      return usage_error(out, "invalid rise-time value");
    }
    tr_s = p.value;
  }
  if (auto o = args.take_option({"--rp", "--pullup", "--resistance"})) {
    auto p = parse_resistance(*o);
    if (!p.ok()) {
      return usage_error(out, "invalid Rp value");
    }
    rp_ohm = p.value;
  }
  if (auto o = args.take_option({"--rs", "--series"})) {
    auto p = parse_resistance(*o);
    if (!p.ok()) {
      return usage_error(out, "invalid Rs value");
    }
    rs_ohm = p.value;
    has_rs = true;
  }

  // Mode subcommand: first positional.
  std::array<std::string_view, 6> pos{};
  std::size_t n_pos = 0;
  while (auto p = args.next_positional()) {
    if (n_pos >= pos.size()) {
      return usage_error(out, "too many positional arguments");
    }
    pos[n_pos++] = *p;
  }
  if (args.has_remaining()) {
    return cli::unexpected_error(out, *args.first_remaining(), kUsage);
  }
  if (n_pos == 0 || !is_i2c_mode(pos[0])) {
    return usage_error(out, "missing mode (pullup|series|full)");
  }
  const std::string_view mode = pos[0];

  // Remaining positionals fill empty slots in VDD, Cb, Rp, Rs order.
  for (std::size_t k = 1; k < n_pos; ++k) {
    const std::string_view tok = pos[k];
    if (looks_like_res(tok)) {
      auto p = parse_resistance(tok);
      if (!p.ok()) {
        return usage_error(out, "invalid resistance positional");
      }
      if (!rp_ohm) {
        rp_ohm = p.value;
      } else if (!has_rs) {
        rs_ohm = p.value;
        has_rs = true;
      } else {
        return unexpected_error(out, tok);
      }
    } else if (looks_like_cap(tok)) {
      if (cb_f) {
        return unexpected_error(out, tok);
      }
      auto p = parse_capacitance(tok);
      if (!p.ok()) {
        return usage_error(out, "invalid capacitance positional");
      }
      cb_f = p.value;
    } else if (ends_with(tok, "V")) {
      if (vdd_v) {
        return unexpected_error(out, tok);
      }
      auto p = parse_voltage(tok);
      if (!p.ok()) {
        return usage_error(out, "invalid voltage positional");
      }
      vdd_v = p.value;
    } else {
      // Bare number: first empty slot in VDD, Cb, Rp, Rs order.
      auto p = parse_double(tok);
      if (!p.ok()) {
        return unexpected_error(out, tok);
      }
      if (!vdd_v) {
        vdd_v = p.value;
      } else if (!cb_f) {
        return usage_error(out, "capacitance needs a unit (e.g. 200pF)");
      } else if (!rp_ohm) {
        rp_ohm = p.value;
      } else if (!has_rs) {
        rs_ohm = p.value;
        has_rs = true;
      } else {
        return unexpected_error(out, tok);
      }
    }
  }

  I2cMode speed = I2cMode::Fast;
  if (speed_raw) {
    auto m = parse_speed(*speed_raw);
    if (!m.ok()) {
      if (*speed_raw == "hs" || *speed_raw == "highspeed" || *speed_raw == "high-speed") {
        out << "Error: Hs-mode uses switched current-source pull-ups "
               "(UM10204 section 5); resistor sizing does not apply\n";
        return core::kExitUsage;
      }
      return usage_error(out, m.error);
    }
    speed = m.value;
  }

  if (!vdd_v || !cb_f) {
    return usage_error(out, "need --vdd and --cb");
  }
  if (*vdd_v <= 0.0) {
    return usage_error(out, "VDD must be > 0");
  }
  if (*cb_f < 0.0) {
    return usage_error(out, "bus capacitance must be >= 0");
  }
  if (rp_ohm && *rp_ohm <= 0.0) {
    return usage_error(out, "Rp must be > 0");
  }
  if (has_rs && *rs_ohm < 0.0) {
    return usage_error(out, "Rs must be >= 0");
  }

  const solvers::I2cModeLimits lim = solvers::i2c_mode_limits(speed);
  const double vol = vol_v.value_or(solvers::i2c_default_vol_v(*vdd_v));
  const double iol = iol_a.value_or(solvers::i2c_default_iol_a(*vdd_v, speed));
  const double tr = tr_s.value_or(lim.tr_max_s);
  if (vol < 0.0) {
    return usage_error(out, "VOL must be >= 0");
  }
  if (iol <= 0.0) {
    return usage_error(out, "IOL must be > 0");
  }
  if (tr <= 0.0) {
    return usage_error(out, "rise time must be > 0");
  }

  out << "limits: " << speed_name(speed) << " (tr " << tr * 1e9 << " ns, tf "
      << lim.tf_max_s * 1e9 << " ns, VOL " << vol << " V, IOL " << iol * 1e3 << " mA, Cbmax "
      << lim.cb_max_f * 1e12 << " pF)\n";
  out << "VDD = " << *vdd_v << " V, Cb = " << *cb_f * 1e12 << " pF\n";
  if (*cb_f > lim.cb_max_f) {
    out << "WARN: Cb exceeds mode maximum (" << lim.cb_max_f * 1e12 << " pF)\n";
  }

  const auto pull = S::solve_pullup(*vdd_v, *cb_f, tr, vol, iol, rp_ohm.value_or(0.0));

  auto print_pullup = [&] {
    out << "Rp(min) = " << pull.rp_min_ohm << " Ohm\n";
    out << "Rp(max) = " << pull.rp_max_ohm << " Ohm\n";
    out << "Rp window: " << (pull.feasible ? "FEASIBLE" : "INFEASIBLE") << '\n';
    if (pull.feasible) {
      out << "Rp(rec) = " << pull.rp_rec_ohm << " Ohm\n";
      out << "Rp(E12) = " << pull.rp_e12_ohm << " Ohm\n";
    }
    if (pull.rp_used_ohm > 0.0) {
      out << "Rp used = " << pull.rp_used_ohm << " Ohm\n";
      out << "tr @Rp = " << pull.actual_tr_s * 1e9 << " ns (limit " << tr * 1e9 << " ns) "
          << (pull.tr_ok ? "OK" : "FAIL") << '\n';
      out << "I static = " << pull.static_current_a * 1e3 << " mA (limit " << iol * 1e3
          << " mA) " << (pull.iol_ok ? "OK" : "FAIL") << '\n';
      out << "P static = " << pull.static_power_w * 1e3 << " mW per line\n";
    }
  };

  if (mode == "pullup") {
    if (!pull.feasible && !rp_ohm) {
      out << "Error: empty Rp window (Rp(min) > Rp(max)); reduce Cb or relax tr\n";
      print_pullup();
      return core::kExitRuntime;
    }
    print_pullup();
    return (pull.rp_used_ohm > 0.0 && (!pull.tr_ok || !pull.iol_ok))
               ? core::kExitRuntime
               : core::kExitOk;
  }

  // series / full need an Rp in place: explicit or the pullup E12 pick.
  double rp_used = 0.0;
  if (rp_ohm) {
    rp_used = *rp_ohm;
  } else if (pull.feasible) {
    rp_used = pull.rp_e12_ohm;
    out << "Rp assumed = " << rp_used << " Ohm (pullup E12 pick)\n";
  } else {
    out << "Error: no feasible Rp; pass --rp explicitly or fix the pullup window\n";
    print_pullup();
    return core::kExitRuntime;
  }

  const auto ser =
      S::solve_series(*vdd_v, vol, iol, rp_used, *cb_f, tr, lim.tf_max_s,
                      has_rs ? *rs_ohm : -1.0);

  auto print_series = [&] {
    out << "Rs(max, LOW) = " << ser.rs_max_low_ohm << " Ohm\n";
    out << "Rs(max, rise) = " << ser.rs_max_rise_ohm << " Ohm\n";
    out << "Rs(max) = " << ser.rs_max_ohm << " Ohm"
        << (ser.feasible ? "" : " INFEASIBLE") << '\n';
    if (!has_rs) {
      out << "Rs assumed = " << ser.rs_used_ohm << " Ohm (max in spec)\n";
    }
    out << "VOL(bus) = " << ser.vol_at_bus_v << " V, VIL margin = " << ser.vil_margin_v
        << " V " << (ser.low_ok ? "OK" : "FAIL") << '\n';
    out << "tr @Rp+Rs = " << ser.tr_with_rs_s * 1e9 << " ns " << (ser.rise_ok ? "OK" : "FAIL")
        << '\n';
    out << "tf est = " << ser.tf_est_s * 1e9 << " ns (limit " << lim.tf_max_s * 1e9 << " ns) "
        << (ser.fall_ok ? "OK" : "FAIL") << '\n';
  };

  if (mode == "series") {
    print_series();
    return (ser.low_ok && ser.rise_ok && ser.fall_ok) ? core::kExitOk : core::kExitRuntime;
  }

  print_pullup();
  print_series();
  const bool ok = pull.feasible && pull.tr_ok && pull.iol_ok && ser.low_ok && ser.rise_ok &&
                  ser.fall_ok && *cb_f <= lim.cb_max_f;
  out << "summary: " << (ok ? "PASS" : "FAIL") << '\n';
  return ok ? core::kExitOk : core::kExitRuntime;
}

void I2cSpec::complete(std::span<const std::string_view> prior_args, std::string_view partial,
                       std::vector<std::string> &out) {
  // Values for a preceding "--speed".
  if (!prior_args.empty() && prior_args.back() == "--speed") {
    push_matches(partial, kI2cSpeeds, out);
    return;
  }
  constexpr std::string_view kSpeedEq = "--speed=";
  if (partial.substr(0, kSpeedEq.size()) == kSpeedEq) {
    const std::string_view sub = partial.substr(kSpeedEq.size());
    for (std::string_view m : kI2cSpeeds) {
      if (m.substr(0, sub.size()) == sub) {
        out.emplace_back(std::string{kSpeedEq} + std::string{m});
      }
    }
    return;
  }
  if (!partial.empty() && partial.front() == '-') {
    push_matches(partial, kI2cOptions, out);
    return;
  }
  if (prior_args.empty()) {
    push_matches(partial, kI2cModes, out); // first positional = mode
  }
  // Later positionals are numeric values: nothing to complete.
}

} // namespace wengine::cli::commands
