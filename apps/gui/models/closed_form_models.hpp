#pragma once

// Closed-form solver models for apps/gui: parse-on-every-frame evaluation
// of the ohm / s0 / i2c solvers from raw text fields, mirroring the CLI
// commands' validation, computation, and output text exactly (same engine
// solve functions, same ostringstream rendering).
//
// Parsing reuses wengine/cli/args.hpp — header-only machinery over the
// units/scales.hpp suffix tables (the single source of truth). No GUI-side
// unit tables, no wengine_cli link: args.hpp is header-only, so including
// it adds no library dependency. Empty (or whitespace-only) fields mean
// "not provided", exactly like a missing CLI option.
//
// Engine-only includes (no ImGui/SDL/Vulkan): unit tests link wengine_core
// alone. Result text uses ostringstream with default double formatting —
// the same stream semantics as the CLI handlers, so golden substrings match.

#include "wengine/cli/args.hpp"
#include "wengine/solvers/i2c.hpp"
#include "wengine/solvers/ohm_law.hpp"
#include "wengine/solvers/s0_pulse.hpp"

#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace wengine::gui {

struct CfResult {
  bool ok = false;
  std::string text;  // rendered output when ok (CLI-identical lines)
  std::string error; // reason when !ok (CLI-parity message)
};

namespace cf_detail {

[[nodiscard]] inline std::string_view trim(std::string_view s) noexcept {
  s = cli::trim_front(s);
  while (!s.empty() &&
         (s.back() == ' ' || s.back() == '\t' || s.back() == '\n' || s.back() == '\r')) {
    s.remove_suffix(1);
  }
  return s;
}

using Parser = core::Result<double> (*)(std::string_view) noexcept;

// Parse one text field. Empty-after-trim clears the slot (missing option);
// failures report CLI-style "invalid <what> '<text>' (<reason>)".
[[nodiscard]] inline bool parse_field(std::string_view text, const char *what, Parser parse,
                                      std::optional<double> &slot, std::string &error) {
  const std::string_view t = trim(text);
  if (t.empty()) {
    slot.reset();
    return true;
  }
  const core::Result<double> p = parse(t);
  if (!p.ok()) {
    error = std::string("invalid ") + what + " '" + std::string(t) + "' (" +
            std::string(p.error) + ")";
    return false;
  }
  slot = p.value;
  return true;
}

} // namespace cf_detail

// ------------------------------------------------------------------ ohm
struct OhmModel {
  std::string u;
  std::string i;
  std::string r;

  [[nodiscard]] CfResult evaluate() const {
    CfResult out;
    std::optional<double> u_si;
    std::optional<double> i_si;
    std::optional<double> r_si;
    if (!cf_detail::parse_field(u, "voltage", &cli::parse_voltage, u_si, out.error) ||
        !cf_detail::parse_field(i, "current", &cli::parse_current, i_si, out.error) ||
        !cf_detail::parse_field(r, "resistance", &cli::parse_resistance, r_si, out.error)) {
      return out;
    }
    const int given = (u_si ? 1 : 0) + (i_si ? 1 : 0) + (r_si ? 1 : 0);
    if (given != 2) {
      out.error = "provide exactly 2 of U/I/R (got " + std::to_string(given) + ")";
      return out;
    }
    using V = units::Value<units::ElectricPotential>;
    using I = units::Value<units::ElectricCurrent>;
    using R = units::Value<units::ElectricResistance>;
    solvers::ResistorState st{};
    if (u_si && i_si) {
      st = solvers::OhmLawSolver::solve_from_ui(V{*u_si}, I{*i_si});
    } else if (u_si && r_si) {
      if (*r_si == 0.0) {
        out.error = "resistance must be non-zero";
        return out;
      }
      st = solvers::OhmLawSolver::solve_from_ur(V{*u_si}, R{*r_si});
    } else {
      st = solvers::OhmLawSolver::solve_from_ir(I{*i_si}, R{*r_si});
    }
    std::ostringstream text;
    text << "U = " << st.voltage.get() << " V\n";
    text << "I = " << st.current.get() << " A\n";
    text << "R = " << st.resistance.get() << " Ohm\n";
    text << "P = " << st.power.get() << " W\n";
    out.ok = true;
    out.text = text.str();
    return out;
  }
};

// ------------------------------------------------------------------ s0
struct S0Model {
  enum Mode : int { Rate, Power, Energy, Pulses, Convert, Full };
  int mode = Rate;
  std::string power;
  std::string freq;
  std::string pulses;
  std::string const_c;
  std::string time;
  std::string energy;
  std::string u;
  std::string i;
  std::string pf;

  [[nodiscard]] CfResult evaluate() const {
    CfResult out;
    using S = solvers::S0PulseSolver;
    std::optional<double> power_w;
    std::optional<double> freq_hz;
    std::optional<double> pulses_n;
    std::optional<double> const_c_v;
    std::optional<double> time_s;
    std::optional<double> energy_j;
    std::optional<double> u_v;
    std::optional<double> i_a;
    std::optional<double> pf_v;
    auto fail = [&](const char *msg) {
      out.error = msg;
      return out;
    };
    if (!cf_detail::parse_field(power, "power", &cli::parse_power, power_w, out.error)) {
      return out;
    }
    if (!cf_detail::parse_field(freq, "frequency", &cli::parse_frequency, freq_hz,
                                out.error)) {
      return out;
    }
    if (!cf_detail::parse_field(pulses, "pulse count", &cli::parse_pulses, pulses_n,
                                out.error)) {
      return out;
    }
    if (!cf_detail::parse_field(const_c, "meter constant", &cli::parse_meter_const,
                                const_c_v, out.error)) {
      return out;
    }
    if (!cf_detail::parse_field(time, "time", &cli::parse_time, time_s, out.error)) {
      return out;
    }
    if (!cf_detail::parse_field(energy, "energy", &cli::parse_energy, energy_j, out.error)) {
      return out;
    }
    if (!cf_detail::parse_field(u, "voltage", &cli::parse_voltage, u_v, out.error)) {
      return out;
    }
    if (!cf_detail::parse_field(i, "current", &cli::parse_current, i_a, out.error)) {
      return out;
    }
    if (!cf_detail::parse_field(pf, "power factor", &cli::parse_double, pf_v, out.error)) {
      return out;
    }
    if (const_c_v && *const_c_v <= 0.0) {
      return fail("meter constant must be > 0");
    }
    const double power_factor = pf_v.value_or(1.0);
    if (pf_v && (power_factor <= 0.0 || power_factor > 1.0)) {
      return fail("power factor must be in (0, 1]");
    }
    std::ostringstream text;
    if (mode == Convert) {
      if (!const_c_v) {
        return fail("convert needs a meter constant (e.g. 1000imp/kWh)");
      }
      const double c = *const_c_v;
      text << "C = " << c << " imp/kWh\n";
      text << "C = " << S::imp_per_kwh_to_imp_per_wh(c) << " imp/Wh\n";
      text << "C = " << S::imp_per_kwh_to_imp_per_j(c) << " imp/J (= imp/Ws)\n";
      text << "E/imp = " << S::imp_per_kwh_to_j_per_imp(c) << " J\n";
      text << "E/imp = " << S::imp_per_kwh_to_wh_per_imp(c) << " Wh\n";
      text << "E/imp = " << S::imp_per_kwh_to_kwh_per_imp(c) << " kWh\n";
    } else if (mode == Rate) {
      if (!power_w || !const_c_v) {
        return fail("rate needs power and a meter constant");
      }
      if (*power_w < 0.0) {
        return fail("power must be >= 0");
      }
      const double f = S::frequency_hz(*power_w, *const_c_v);
      text << "P = " << *power_w << " W\n";
      text << "f = " << f << " Hz (imp/s)\n";
      text << "T = " << S::period_s(*power_w, *const_c_v) << " s/imp\n";
      text << "rate = " << f * 60.0 << " imp/min\n";
      text << "rate = " << f * 3600.0 << " imp/h\n";
    } else if (mode == Power) {
      if (!freq_hz || !const_c_v) {
        return fail("power needs frequency and a meter constant");
      }
      if (*freq_hz < 0.0) {
        return fail("frequency must be >= 0");
      }
      const double p = S::power_w_from_frequency(*freq_hz, *const_c_v);
      text << "f = " << *freq_hz << " Hz\n";
      text << "P = " << p << " W\n";
      text << "P = " << p / 1000.0 << " kW\n";
    } else if (mode == Energy) {
      if (!pulses_n || !const_c_v) {
        return fail("energy needs a pulse count and a meter constant");
      }
      if (*pulses_n < 0.0) {
        return fail("pulse count must be >= 0");
      }
      const double ekwh = S::energy_kwh_from_pulses(*pulses_n, *const_c_v);
      text << "N = " << *pulses_n << " imp\n";
      text << "E = " << ekwh << " kWh\n";
      text << "E = " << ekwh * 1000.0 << " Wh\n";
      text << "E = " << S::energy_j_from_pulses(*pulses_n, *const_c_v) << " J\n";
    } else if (mode == Pulses) {
      if (!const_c_v) {
        return fail("pulses needs a meter constant");
      }
      if (energy_j) {
        if (*energy_j < 0.0) {
          return fail("energy must be >= 0");
        }
        text << "E = " << *energy_j << " J\n";
        text << "E = " << S::joules_to_kwh(*energy_j) << " kWh\n";
        text << "N = " << S::pulses_from_energy_j(*energy_j, *const_c_v) << " imp\n";
      } else {
        if (!power_w || !time_s) {
          return fail("pulses needs power + time (or energy) and a meter constant");
        }
        if (*power_w < 0.0 || *time_s <= 0.0) {
          return fail("power must be >= 0 and time > 0");
        }
        const double ej = *power_w * *time_s;
        text << "P = " << *power_w << " W\n";
        text << "t = " << *time_s << " s\n";
        text << "N = " << S::pulses_from_power_time(*power_w, *time_s, *const_c_v)
             << " imp\n";
        text << "E = " << S::joules_to_kwh(ej) << " kWh\n";
        text << "E = " << ej << " J\n";
      }
    } else { // Full
      if (!const_c_v) {
        return fail("full needs a meter constant");
      }
      double p_w = 0.0;
      if (power_w) {
        p_w = *power_w;
      } else {
        if (!u_v || !i_a) {
          return fail("full needs power or U + I (plus a meter constant)");
        }
        p_w = S::power_w_from_ui(*u_v, *i_a, power_factor);
      }
      if (p_w < 0.0) {
        return fail("power must be >= 0");
      }
      const double t = time_s.value_or(3600.0);
      if (t <= 0.0) {
        return fail("time must be > 0");
      }
      const auto r = S::solve_full(p_w, t, *const_c_v);
      if (u_v) {
        text << "U = " << *u_v << " V\n";
      }
      if (i_a) {
        text << "I = " << *i_a << " A\n";
      }
      text << "P = " << r.power_w << " W\n";
      text << "P = " << r.power_w / 1000.0 << " kW\n";
      text << "E = " << r.energy_kwh << " kWh\n";
      text << "E = " << r.energy_j << " J\n";
      text << "f = " << r.freq_hz << " Hz\n";
      text << "T = " << r.period_s << " s/imp\n";
      text << "N = " << r.pulses << " imp\n";
      text << "E/imp = " << r.j_per_imp << " J\n";
    }
    out.ok = true;
    out.text = text.str();
    return out;
  }
};

// ------------------------------------------------------------------ i2c
struct I2cModel {
  enum Mode : int { Pullup, Series, Full };
  enum Speed : int { Std, Fast, Fmp };
  int mode = Pullup;
  int speed = Fast;
  std::string vdd;
  std::string cb;
  std::string vol;
  std::string iol;
  std::string tr;
  std::string rp;
  std::string rs;

  [[nodiscard]] CfResult evaluate() const {
    CfResult out;
    using S = solvers::I2cBusSolver;
    auto fail = [&](const char *msg) {
      out.error = msg;
      return out;
    };
    std::optional<double> vdd_v;
    std::optional<double> cb_f;
    std::optional<double> vol_v;
    std::optional<double> iol_a;
    std::optional<double> tr_s;
    std::optional<double> rp_ohm;
    std::optional<double> rs_ohm;
    if (!cf_detail::parse_field(vdd, "VDD", &cli::parse_voltage, vdd_v, out.error) ||
        !cf_detail::parse_field(cb, "bus capacitance", &cli::parse_capacitance, cb_f,
                                out.error) ||
        !cf_detail::parse_field(vol, "VOL", &cli::parse_voltage, vol_v, out.error) ||
        !cf_detail::parse_field(iol, "IOL", &cli::parse_current, iol_a, out.error) ||
        !cf_detail::parse_field(tr, "rise time", &cli::parse_time, tr_s, out.error) ||
        !cf_detail::parse_field(rp, "Rp", &cli::parse_resistance, rp_ohm, out.error) ||
        !cf_detail::parse_field(rs, "Rs", &cli::parse_resistance, rs_ohm, out.error)) {
      return out;
    }
    solvers::I2cMode sm = solvers::I2cMode::Fast;
    const char *speed_label = "fast";
    if (speed == Std) {
      sm = solvers::I2cMode::Standard;
      speed_label = "std";
    } else if (speed == Fmp) {
      sm = solvers::I2cMode::FastPlus;
      speed_label = "fmp";
    }
    if (!vdd_v || !cb_f) {
      return fail("need VDD and bus capacitance");
    }
    if (*vdd_v <= 0.0) {
      return fail("VDD must be > 0");
    }
    if (*cb_f < 0.0) {
      return fail("bus capacitance must be >= 0");
    }
    if (rp_ohm && *rp_ohm <= 0.0) {
      return fail("Rp must be > 0");
    }
    if (rs_ohm && *rs_ohm < 0.0) {
      return fail("Rs must be >= 0");
    }
    const solvers::I2cModeLimits lim = solvers::i2c_mode_limits(sm);
    const double vol_si = vol_v.value_or(solvers::i2c_default_vol_v(*vdd_v));
    const double iol_si = iol_a.value_or(solvers::i2c_default_iol_a(*vdd_v, sm));
    const double tr_si = tr_s.value_or(lim.tr_max_s);
    if (vol_si < 0.0) {
      return fail("VOL must be >= 0");
    }
    if (iol_si <= 0.0) {
      return fail("IOL must be > 0");
    }
    if (tr_si <= 0.0) {
      return fail("rise time must be > 0");
    }

    std::ostringstream text;
    text << "limits: " << speed_label << " (tr " << tr_si * 1e9 << " ns, tf "
         << lim.tf_max_s * 1e9 << " ns, VOL " << vol_si << " V, IOL " << iol_si * 1e3
         << " mA, Cbmax " << lim.cb_max_f * 1e12 << " pF)\n";
    text << "VDD = " << *vdd_v << " V, Cb = " << *cb_f * 1e12 << " pF\n";
    if (*cb_f > lim.cb_max_f) {
      text << "WARN: Cb exceeds mode maximum (" << lim.cb_max_f * 1e12 << " pF)\n";
    }
    const auto pull =
        S::solve_pullup(*vdd_v, *cb_f, tr_si, vol_si, iol_si, rp_ohm.value_or(0.0));
    std::ostringstream pull_text;
    pull_text << "Rp(min) = " << pull.rp_min_ohm << " Ohm\n";
    pull_text << "Rp(max) = " << pull.rp_max_ohm << " Ohm\n";
    pull_text << "Rp window: " << (pull.feasible ? "FEASIBLE" : "INFEASIBLE") << '\n';
    if (pull.feasible) {
      pull_text << "Rp(rec) = " << pull.rp_rec_ohm << " Ohm\n";
      pull_text << "Rp(E12) = " << pull.rp_e12_ohm << " Ohm\n";
    }
    if (pull.rp_used_ohm > 0.0) {
      pull_text << "Rp used = " << pull.rp_used_ohm << " Ohm\n";
      pull_text << "tr @Rp = " << pull.actual_tr_s * 1e9 << " ns (limit " << tr_si * 1e9
                 << " ns) " << (pull.tr_ok ? "OK" : "FAIL") << '\n';
      pull_text << "I static = " << pull.static_current_a * 1e3 << " mA (limit "
                 << iol_si * 1e3 << " mA) " << (pull.iol_ok ? "OK" : "FAIL") << '\n';
      pull_text << "P static = " << pull.static_power_w * 1e3 << " mW per line\n";
    }
    if (mode == Pullup) {
      // An infeasible empty window is analysis output, not a panel error:
      // the FEASIBLE/INFEASIBLE lines above carry the verdict.
      out.ok = true;
      out.text = pull_text.str();
      return out;
    }
    double rp_used = 0.0;
    if (rp_ohm) {
      rp_used = *rp_ohm;
    } else if (pull.feasible) {
      rp_used = pull.rp_e12_ohm;
      text << "Rp assumed = " << rp_used << " Ohm (pullup E12 pick)\n";
    } else {
      out.ok = true;
      text << "Error: no feasible Rp; set Rp explicitly or fix the pullup window\n";
      text << pull_text.str();
      return out;
    }
    const auto ser = S::solve_series(*vdd_v, vol_si, iol_si, rp_used, *cb_f, tr_si,
                                     lim.tf_max_s, rs_ohm.value_or(-1.0));
    std::ostringstream ser_text;
    ser_text << "Rs(max, LOW) = " << ser.rs_max_low_ohm << " Ohm\n";
    ser_text << "Rs(max, rise) = " << ser.rs_max_rise_ohm << " Ohm\n";
    ser_text << "Rs(max) = " << ser.rs_max_ohm << " Ohm"
              << (ser.feasible ? "" : " INFEASIBLE") << '\n';
    if (!rs_ohm) {
      ser_text << "Rs assumed = " << ser.rs_used_ohm << " Ohm (max in spec)\n";
    }
    ser_text << "VOL(bus) = " << ser.vol_at_bus_v << " V, VIL margin = "
              << ser.vil_margin_v << " V " << (ser.low_ok ? "OK" : "FAIL") << '\n';
    ser_text << "tr @Rp+Rs = " << ser.tr_with_rs_s * 1e9 << " ns "
              << (ser.rise_ok ? "OK" : "FAIL") << '\n';
    ser_text << "tf est = " << ser.tf_est_s * 1e9 << " ns (limit " << lim.tf_max_s * 1e9
              << " ns) " << (ser.fall_ok ? "OK" : "FAIL") << '\n';
    if (mode == Series) {
      out.ok = true;
      text << ser_text.str();
      out.text = text.str();
      return out;
    }
    const bool pass = pull.feasible && pull.tr_ok && pull.iol_ok && ser.low_ok &&
                      ser.rise_ok && ser.fall_ok && *cb_f <= lim.cb_max_f;
    text << pull_text.str();
    text << ser_text.str();
    text << "summary: " << (pass ? "PASS" : "FAIL") << '\n';
    out.ok = true;
    out.text = text.str();
    return out;
  }
};

} // namespace wengine::gui
