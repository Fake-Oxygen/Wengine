#include "wengine/cli/commands.hpp"
#include "wengine/solvers/s0_pulse.hpp"

#include <array>
#include <optional>
#include <ostream>
#include <string_view>

namespace wengine::cli::commands {

namespace {

[[nodiscard]] constexpr bool is_s0_mode(std::string_view s) noexcept {
  return s == "rate" || s == "power" || s == "energy" || s == "pulses" ||
         s == "convert" || s == "full";
}

[[nodiscard]] constexpr bool ends_with(std::string_view s, std::string_view suf) noexcept {
  return s.size() >= suf.size() && s.substr(s.size() - suf.size()) == suf;
}

// Energy tokens carry J/Ws/kJ/Wh/kWh/MWh suffixes.
[[nodiscard]] constexpr bool looks_like_energy(std::string_view s) noexcept {
  return ends_with(s, "kWh") || ends_with(s, "MWh") || ends_with(s, "Wh") ||
         ends_with(s, "kJ") || ends_with(s, "MJ") || ends_with(s, "J") ||
         ends_with(s, "Ws");
}

int usage_error(std::ostream &out, std::string_view msg) {
  out << "Error: " << msg << '\n';
  out << S0Spec::kUsage << '\n';
  return 2;
}

int unexpected_error(std::ostream &out, std::string_view tok) {
  out << "Error: unexpected argument '" << tok << "'\n";
  out << S0Spec::kUsage << '\n';
  return 2;
}

} // namespace

int S0Spec::run(ArgCursor &args, std::ostream &out) {
  if (args.take_flag({"--help", "-h"})) {
    out << kName << " - " << kDescription << "\nUsage:\n" << kUsage << '\n';
    return 0;
  }

  using S = solvers::S0PulseSolver;

  std::optional<double> power_w;
  std::optional<double> freq_hz;
  std::optional<double> pulses_n;
  std::optional<double> const_c;
  std::optional<double> time_s;
  std::optional<double> energy_j;
  std::optional<double> u_v;
  std::optional<double> i_a;
  double power_factor = 1.0;
  bool has_pf = false;

  if (auto o = args.take_option({"--power", "-P"})) {
    auto p = parse_power(*o);
    if (!p.ok) {
      return usage_error(out, "invalid power value");
    }
    power_w = p.value;
  }
  if (auto o = args.take_option({"--freq", "--frequency", "-f"})) {
    auto p = parse_frequency(*o);
    if (!p.ok) {
      return usage_error(out, "invalid frequency value");
    }
    freq_hz = p.value;
  }
  if (auto o = args.take_option({"--pulses", "--count", "-n"})) {
    auto p = parse_pulses(*o);
    if (!p.ok) {
      return usage_error(out, "invalid pulse count");
    }
    pulses_n = p.value;
  }
  if (auto o = args.take_option({"--const", "--meter", "-c"})) {
    auto p = parse_meter_const(*o);
    if (!p.ok) {
      return usage_error(out, "invalid meter constant (e.g. 1000imp/kWh)");
    }
    const_c = p.value;
  }
  if (auto o = args.take_option({"--time", "-t"})) {
    auto p = parse_time(*o);
    if (!p.ok) {
      return usage_error(out, "invalid time value");
    }
    time_s = p.value;
  }
  if (auto o = args.take_option({"--energy", "-e"})) {
    auto p = parse_energy(*o);
    if (!p.ok) {
      return usage_error(out, "invalid energy value");
    }
    energy_j = p.value;
  }
  if (auto o = args.take_option({"--voltage", "--u", "-U"})) {
    auto p = parse_voltage(*o);
    if (!p.ok) {
      return usage_error(out, "invalid voltage value");
    }
    u_v = p.value;
  }
  if (auto o = args.take_option({"--current", "--i", "-I"})) {
    auto p = parse_current(*o);
    if (!p.ok) {
      return usage_error(out, "invalid current value");
    }
    i_a = p.value;
  }
  if (auto o = args.take_option({"--pf", "--power-factor"})) {
    auto p = parse_double(*o);
    if (!p.ok) {
      return usage_error(out, "invalid power factor");
    }
    power_factor = p.value;
    has_pf = true;
  }

  std::optional<std::string_view> mode_opt = args.take_option({"--mode", "-m"});

  // Collect positionals (mode may be first).
  std::array<std::string_view, 6> pos{};
  std::size_t n_pos = 0;
  while (auto p = args.next_positional()) {
    if (n_pos >= pos.size()) {
      return usage_error(out, "too many positional arguments");
    }
    pos[n_pos++] = *p;
  }
  if (args.has_remaining()) {
    out << "Error: unknown option '" << *args.first_remaining() << "'\n";
    out << kUsage << '\n';
    return 2;
  }

  std::string_view mode{};
  std::size_t v_begin = 0; // first value positional
  if (mode_opt) {
    if (!is_s0_mode(*mode_opt)) {
      return usage_error(out, "unknown mode (rate|power|energy|pulses|convert|full)");
    }
    mode = *mode_opt;
  } else if (n_pos > 0 && is_s0_mode(pos[0])) {
    mode = pos[0];
    v_begin = 1;
  } else {
    // Infer from provided options.
    if (freq_hz) {
      mode = "power";
    } else if (pulses_n) {
      mode = "energy";
    } else if ((power_w && time_s) || energy_j) {
      mode = "pulses";
    } else if (power_w) {
      mode = "rate";
    } else if (u_v || i_a) {
      mode = "full";
    } else if (const_c) {
      mode = "convert";
    } else {
      return usage_error(out, "missing mode (rate|power|energy|pulses|convert|full)");
    }
  }
  const std::size_t n_val = n_pos - v_begin;
  auto val = [&](std::size_t k) -> std::string_view { return pos[v_begin + k]; };

  // Positional fallback per mode. Single-slot modes reject any positional
  // when the slot is already filled via named option (no silent ignores).
  if (mode == "convert") {
    if (const_c && n_val > 0) {
      return unexpected_error(out, val(0));
    }
    if (!const_c && n_val != 1) {
      return usage_error(out, "convert takes only the meter constant");
    }
    if (!const_c) {
      auto p = parse_meter_const(val(0));
      if (!p.ok) {
        return usage_error(out, "invalid meter constant positional");
      }
      const_c = p.value;
    }
  } else if (mode == "rate") {
    if (power_w && n_val > 0) {
      return unexpected_error(out, val(0));
    }
    if (!power_w && n_val != 1) {
      return usage_error(out, "rate takes only the power value");
    }
    if (!power_w) {
      auto p = parse_power(val(0));
      if (!p.ok) {
        return usage_error(out, "invalid power positional");
      }
      power_w = p.value;
    }
  } else if (mode == "power") {
    if (freq_hz && n_val > 0) {
      return unexpected_error(out, val(0));
    }
    if (!freq_hz && n_val != 1) {
      return usage_error(out, "power takes only the frequency value");
    }
    if (!freq_hz) {
      auto p = parse_frequency(val(0));
      if (!p.ok) {
        return usage_error(out, "invalid frequency positional");
      }
      freq_hz = p.value;
    }
  } else if (mode == "energy") {
    if (pulses_n && n_val > 0) {
      return unexpected_error(out, val(0));
    }
    if (!pulses_n && n_val != 1) {
      return usage_error(out, "energy takes only the pulse count");
    }
    if (!pulses_n) {
      auto p = parse_pulses(val(0));
      if (!p.ok) {
        return usage_error(out, "invalid pulse count positional");
      }
      pulses_n = p.value;
    }
  } else if (mode == "pulses") {
    if (energy_j && n_val > 0) {
      return usage_error(out, "pulses takes either --energy or power + time, not both");
    }
    if (!energy_j && !power_w && n_val == 1 && looks_like_energy(val(0))) {
      auto p = parse_energy(val(0));
      if (!p.ok) {
        return usage_error(out, "invalid energy positional");
      }
      energy_j = p.value;
    } else {
      // Assign positionals in order to the still-missing power/time slots.
      std::size_t k = 0;
      if (!power_w && k < n_val && !looks_like_energy(val(k))) {
        auto p = parse_power(val(k));
        if (!p.ok) {
          return usage_error(out, "invalid power positional");
        }
        power_w = p.value;
        ++k;
      }
      if (!time_s && k < n_val) {
        auto p = parse_time(val(k));
        if (!p.ok) {
          return usage_error(out, "invalid time positional");
        }
        time_s = p.value;
        ++k;
      }
      if (k < n_val) {
        return usage_error(out, "pulses takes power + time (or --energy)");
      }
    }
  } else { // full
    // Assign positionals in order to the still-missing U/I/time slots,
    // so mixed named + positional works (e.g. --u 230V 10A).
    std::size_t k = 0;
    if (!u_v && k < n_val) {
      auto p = parse_voltage(val(k));
      if (!p.ok) {
        return usage_error(out, "invalid voltage positional");
      }
      u_v = p.value;
      ++k;
    }
    if (!i_a && k < n_val) {
      auto p = parse_current(val(k));
      if (!p.ok) {
        return usage_error(out, "invalid current positional");
      }
      i_a = p.value;
      ++k;
    }
    if (!time_s && k < n_val) {
      auto p = parse_time(val(k));
      if (!p.ok) {
        return usage_error(out, "invalid time positional");
      }
      time_s = p.value;
      ++k;
    }
    if (k < n_val) {
      return usage_error(out, "full takes U I [time]");
    }
  }

  if (const_c && *const_c <= 0.0) {
    return usage_error(out, "meter constant must be > 0");
  }
  if (has_pf && (power_factor <= 0.0 || power_factor > 1.0)) {
    return usage_error(out, "power factor must be in (0, 1]");
  }

  if (mode == "convert") {
    if (!const_c) {
      return usage_error(out, "convert needs --const (e.g. 1000imp/kWh)");
    }
    const double c = *const_c;
    out << "C = " << c << " imp/kWh\n";
    out << "C = " << S::imp_per_kwh_to_imp_per_wh(c) << " imp/Wh\n";
    out << "C = " << S::imp_per_kwh_to_imp_per_j(c) << " imp/J (= imp/Ws)\n";
    out << "E/imp = " << S::imp_per_kwh_to_j_per_imp(c) << " J\n";
    out << "E/imp = " << S::imp_per_kwh_to_wh_per_imp(c) << " Wh\n";
    out << "E/imp = " << S::imp_per_kwh_to_kwh_per_imp(c) << " kWh\n";
    return 0;
  }
  if (mode == "rate") {
    if (!power_w || !const_c) {
      return usage_error(out, "rate needs --power and --const");
    }
    if (*power_w < 0.0) {
      return usage_error(out, "power must be >= 0");
    }
    const double f = S::frequency_hz(*power_w, *const_c);
    const double t = S::period_s(*power_w, *const_c);
    out << "P = " << *power_w << " W\n";
    out << "f = " << f << " Hz (imp/s)\n";
    out << "T = " << t << " s/imp\n";
    out << "rate = " << f * 60.0 << " imp/min\n";
    out << "rate = " << f * 3600.0 << " imp/h\n";
    return 0;
  }
  if (mode == "power") {
    if (!freq_hz || !const_c) {
      return usage_error(out, "power needs --freq and --const");
    }
    if (*freq_hz < 0.0) {
      return usage_error(out, "frequency must be >= 0");
    }
    const double p = S::power_w_from_frequency(*freq_hz, *const_c);
    out << "f = " << *freq_hz << " Hz\n";
    out << "P = " << p << " W\n";
    out << "P = " << p / 1000.0 << " kW\n";
    return 0;
  }
  if (mode == "energy") {
    if (!pulses_n || !const_c) {
      return usage_error(out, "energy needs --pulses and --const");
    }
    if (*pulses_n < 0.0) {
      return usage_error(out, "pulse count must be >= 0");
    }
    const double ekwh = S::energy_kwh_from_pulses(*pulses_n, *const_c);
    const double ej = S::energy_j_from_pulses(*pulses_n, *const_c);
    out << "N = " << *pulses_n << " imp\n";
    out << "E = " << ekwh << " kWh\n";
    out << "E = " << ekwh * 1000.0 << " Wh\n";
    out << "E = " << ej << " J\n";
    return 0;
  }
  if (mode == "pulses") {
    if (!const_c) {
      return usage_error(out, "pulses needs --const");
    }
    if (energy_j) {
      if (*energy_j < 0.0) {
        return usage_error(out, "energy must be >= 0");
      }
      const double n = S::pulses_from_energy_j(*energy_j, *const_c);
      out << "E = " << *energy_j << " J\n";
      out << "E = " << S::joules_to_kwh(*energy_j) << " kWh\n";
      out << "N = " << n << " imp\n";
      return 0;
    }
    if (!power_w || !time_s) {
      return usage_error(out, "pulses needs --power + --time (or --energy) and --const");
    }
    if (*power_w < 0.0 || *time_s <= 0.0) {
      return usage_error(out, "power must be >= 0 and time > 0");
    }
    const double n = S::pulses_from_power_time(*power_w, *time_s, *const_c);
    const double ej = *power_w * *time_s;
    out << "P = " << *power_w << " W\n";
    out << "t = " << *time_s << " s\n";
    out << "N = " << n << " imp\n";
    out << "E = " << S::joules_to_kwh(ej) << " kWh\n";
    out << "E = " << ej << " J\n";
    return 0;
  }
  // full
  if (!const_c) {
    return usage_error(out, "full needs --const");
  }
  double p_w = 0.0;
  if (power_w) {
    p_w = *power_w;
  } else {
    if (!u_v || !i_a) {
      return usage_error(out, "full needs --power or --u + --i (plus --const)");
    }
    p_w = S::power_w_from_ui(*u_v, *i_a, power_factor);
  }
  if (p_w < 0.0) {
    return usage_error(out, "power must be >= 0");
  }
  const double t = time_s.value_or(3600.0);
  if (t <= 0.0) {
    return usage_error(out, "time must be > 0");
  }
  const auto r = S::solve_full(p_w, t, *const_c);
  if (u_v) {
    out << "U = " << *u_v << " V\n";
  }
  if (i_a) {
    out << "I = " << *i_a << " A\n";
  }
  out << "P = " << r.power_w << " W\n";
  out << "P = " << r.power_w / 1000.0 << " kW\n";
  out << "E = " << r.energy_kwh << " kWh\n";
  out << "E = " << r.energy_j << " J\n";
  out << "f = " << r.freq_hz << " Hz\n";
  out << "T = " << r.period_s << " s/imp\n";
  out << "N = " << r.pulses << " imp\n";
  out << "E/imp = " << r.j_per_imp << " J\n";
  return 0;
}

} // namespace wengine::cli::commands
