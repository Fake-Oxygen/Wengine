#include "wengine/cli/ic/ap63205.hpp"
#include "wengine/cli/status.hpp"
#include "wengine/solvers/ic/ap63205.hpp"

#include <array>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wengine::cli::ic {

namespace {

using S = solvers::ic::Ap63205Solver;

int usage_error(std::ostream &out, std::string_view msg) {
  return cli::usage_error(out, msg, Ap63205Spec::kUsage);
}

int unexpected_error(std::ostream &out, std::string_view tok) {
  return cli::unexpected_error(out, tok, Ap63205Spec::kUsage);
}

struct VinIout {
  double vin_v{0.0};
  double iout_a{0.0};
};

core::Result<VinIout> parse_vin_iout(ArgCursor &args) noexcept {
  auto vin_raw = args.take_option({"--vin"});
  if (!vin_raw) {
    return core::Result<VinIout>::failure("need --vin (3.8..32V)");
  }
  auto vin = parse_voltage(*vin_raw);
  if (!vin.ok()) {
    return core::Result<VinIout>::failure("invalid --vin value");
  }
  if (vin.value < S::kVinMinV || vin.value > S::kVinMaxV) {
    return core::Result<VinIout>::failure("VIN must be within 3.8..32V");
  }
  double iout = S::kMaxLoadA;
  if (auto o = args.take_option({"--iout"})) {
    auto p = parse_current(*o);
    if (!p.ok()) {
      return core::Result<VinIout>::failure("invalid --iout value");
    }
    iout = p.value;
  }
  if (iout <= 0.0 || iout > S::kMaxLoadA) {
    return core::Result<VinIout>::failure("--iout must be within (0, 2A]");
  }
  return core::Result<VinIout>::success(VinIout{vin.value, iout});
}

// --ripple FRAC (fraction of the 2A max load) or explicit --delta-il.
core::Result<double> resolve_delta_il(ArgCursor &args) noexcept {
  auto frac_raw = args.take_option({"--ripple"});
  auto expl_raw = args.take_option({"--delta-il"});
  if (frac_raw && expl_raw) {
    return core::Result<double>::failure("use either --ripple or --delta-il");
  }
  if (expl_raw) {
    auto p = parse_current(*expl_raw);
    if (!p.ok()) {
      return core::Result<double>::failure("invalid --delta-il value (e.g. 0.8A)");
    }
    if (p.value <= 0.0) {
      return core::Result<double>::failure("--delta-il must be > 0");
    }
    return core::Result<double>::success(p.value);
  }
  double frac = S::kRippleDefaultFrac;
  if (frac_raw) {
    auto p = parse_double(*frac_raw);
    if (!p.ok()) {
      return core::Result<double>::failure("invalid --ripple fraction (0.3..0.5)");
    }
    frac = p.value;
  }
  if (frac <= 0.0 || frac > 1.0) {
    return core::Result<double>::failure("--ripple fraction must be in (0, 1]");
  }
  return core::Result<double>::success(S::default_ripple_a(frac));
}

void warn_ripple_band(std::ostream &out, double delta_il_a) {
  if (delta_il_a < S::kRippleLoA || delta_il_a > S::kRippleHiA) {
    out << "WARN: dIL outside the datasheet 0.6..1.0A band (30..50% of 2A)\n";
  }
}

int run_inductor(ArgCursor &args, std::ostream &out) {
  auto vi = parse_vin_iout(args);
  if (!vi.ok()) {
    return usage_error(out, vi.error);
  }
  auto dr = resolve_delta_il(args);
  if (!dr.ok()) {
    return usage_error(out, dr.error);
  }
  if (args.has_remaining()) {
    return unexpected_error(out, *args.first_remaining());
  }
  const auto r = S::solve_inductor(vi.value.vin_v, vi.value.iout_a, dr.value);
  out << "VIN = " << vi.value.vin_v << " V, VOUT = 5 V (fixed), fSW = 1.1 MHz\n";
  warn_ripple_band(out, r.delta_il_a);
  out << "dIL = " << r.delta_il_a << " A\n";
  out << "L = " << r.l_h * 1e6 << " uH\n";
  out << "Table 3 pick: 4.7 uH (2 x 22uF COUT, 10uF CIN, 100nF BST)\n";
  out << "IL(peak) = " << r.peak_a << " A (HS limit min 2.5 A) "
      << (r.peak_ok ? "OK" : "FAIL") << '\n';
  out << "Isat rating >= 2.7 A (1.35x 2A max), DCR < 100 mOhm\n";
  out << "tON = " << r.ton_s * 1e9 << " ns (min 80 ns) " << (r.ton_ok ? "OK" : "FAIL")
      << '\n';
  return (!r.peak_ok || !r.ton_ok) ? core::kExitRuntime : core::kExitOk;
}

int run_cout(ArgCursor &args, std::ostream &out) {
  auto vi = parse_vin_iout(args);
  if (!vi.ok()) {
    return usage_error(out, vi.error);
  }
  auto dr = resolve_delta_il(args);
  if (!dr.ok()) {
    return usage_error(out, dr.error);
  }
  auto esr_raw = args.take_option({"--esr"});
  if (!esr_raw) {
    return usage_error(out, "cout needs --esr (e.g. 5mOhm)");
  }
  auto esr = parse_resistance(*esr_raw);
  if (!esr.ok() || esr.value <= 0.0) {
    return usage_error(out, "invalid --esr value (must be > 0)");
  }
  double overshoot = 0.05 * S::kVoutV;
  if (auto o = args.take_option({"--overshoot"})) {
    auto p = parse_voltage(*o);
    if (!p.ok() || p.value <= 0.0) {
      return usage_error(out, "invalid --overshoot value (must be > 0)");
    }
    overshoot = p.value;
  }
  if (args.has_remaining()) {
    return unexpected_error(out, *args.first_remaining());
  }
  const double l_h = S::inductor_h(vi.value.vin_v, S::kVoutV, dr.value, S::kFswHz);
  const auto r = S::solve_cout(l_h, vi.value.iout_a, dr.value, esr.value, overshoot);
  out << "VIN = " << vi.value.vin_v << " V, VOUT = 5 V (fixed)\n";
  warn_ripple_band(out, dr.value);
  out << "dIL = " << dr.value << " A (L = " << l_h * 1e6 << " uH)\n";
  out << "VOUT ripple = " << r.ripple_v * 1e3 << " mV (dIL * ESR)\n";
  out << "COUT(min) = " << r.cout_min_f * 1e6 << " uF (transient, dV = " << overshoot
      << " V)\n";
  out << "Table 3 pick: 2 x 22uF ceramic\n";
  return core::kExitOk;
}

int run_uvlo(ArgCursor &args, std::ostream &out) {
  auto von_raw = args.take_option({"--von"});
  auto voff_raw = args.take_option({"--voff"});
  if (!von_raw || !voff_raw) {
    return usage_error(out, "uvlo needs --von and --voff");
  }
  auto von = parse_voltage(*von_raw);
  auto voff = parse_voltage(*voff_raw);
  if (!von.ok() || !voff.ok()) {
    return usage_error(out, "invalid --von/--voff value");
  }
  if (voff.value <= 0.0 || von.value <= voff.value) {
    return usage_error(out, "need 0 < VOFF < VON");
  }
  if (0.932 * von.value <= voff.value) {
    return usage_error(out, "need 0.932*VON > VOFF (else R5 <= 0)");
  }
  const auto r = S::solve_uvlo(von.value, voff.value);
  if (!(r.r6_ohm > 0.0)) {
    return usage_error(out, "no feasible divider (R6 <= 0)");
  }
  if (args.has_remaining()) {
    return unexpected_error(out, *args.first_remaining());
  }
  out << "VON = " << von.value << " V, VOFF = " << voff.value << " V\n";
  out << "R5 = " << r.r5_ohm / 1e3 << " kOhm\n";
  out << "R6 = " << r.r6_ohm / 1e3 << " kOhm\n";
  out << "EN thresholds typ: high 1.18 V, low 1.10 V (internal VIN UVLO 3.1 V)\n";
  return core::kExitOk;
}

int run_delay(ArgCursor &args, std::ostream &out) {
  auto ts_raw = args.take_option({"--ts"});
  auto cd_raw = args.take_option({"--cd"});
  if (static_cast<bool>(ts_raw) == static_cast<bool>(cd_raw)) {
    return usage_error(out, "delay needs exactly one of --ts or --cd");
  }
  if (ts_raw) {
    auto p = parse_time(*ts_raw);
    if (!p.ok() || p.value <= 0.0) {
      return usage_error(out, "invalid --ts value (must be > 0)");
    }
    if (args.has_remaining()) {
      return unexpected_error(out, *args.first_remaining());
    }
    const double ts_ms = p.value * 1e3;
    out << "ts = " << ts_ms << " ms\n";
    out << "Cd = " << S::en_delay_cap_nf(ts_ms) << " nF\n";
    return core::kExitOk;
  }
  auto p = parse_capacitance(*cd_raw);
  if (!p.ok() || p.value <= 0.0) {
    return usage_error(out, "invalid --cd value (must be > 0)");
  }
  if (args.has_remaining()) {
    return unexpected_error(out, *args.first_remaining());
  }
  const double cd_nf = p.value * 1e9;
  out << "Cd = " << cd_nf << " nF\n";
  out << "ts = " << S::en_delay_time_ms(cd_nf) << " ms\n";
  return core::kExitOk;
}

int run_thermal(ArgCursor &args, std::ostream &out) {
  double ta_c = 25.0;
  if (auto o = args.take_option({"--ta"})) {
    auto p = parse_double(*o);
    if (!p.ok()) {
      return usage_error(out, "invalid --ta value (C, e.g. 85)");
    }
    ta_c = p.value;
  }
  auto pd_raw = args.take_option({"--pd"});
  const bool wants_est = args.has_flag({"--vin"}) || args.has_flag({"--iout"});
  if (pd_raw && wants_est) {
    return usage_error(out, "use either --pd or --vin/--iout");
  }
  double pd_w = 0.0;
  bool estimated = false;
  if (pd_raw) {
    auto p = parse_power(*pd_raw);
    if (!p.ok() || p.value < 0.0) {
      return usage_error(out, "invalid --pd value (must be >= 0)");
    }
    pd_w = p.value;
  } else if (wants_est) {
    auto vi = parse_vin_iout(args);
    if (!vi.ok()) {
      return usage_error(out, vi.error);
    }
    pd_w = S::conduction_loss_w(vi.value.vin_v, S::kVoutV, vi.value.iout_a);
    estimated = true;
    out << "VIN = " << vi.value.vin_v << " V, IOUT = " << vi.value.iout_a << " A\n";
  } else {
    return usage_error(out, "thermal needs --pd or --vin [--iout]");
  }
  if (args.has_remaining()) {
    return unexpected_error(out, *args.first_remaining());
  }
  const auto r = S::solve_thermal(pd_w, ta_c);
  out << "PD = " << r.pd_w << " W" << (estimated ? " (conduction estimate, no switching loss)"
                                                : "") << '\n';
  out << "TRISE = " << r.rise_c << " C (thetaJA 89 C/W, TSOT26)\n";
  out << "TJ = " << r.tj_c << " C at TA = " << ta_c << " C (design max +125 C) "
      << (r.ok ? "PASS" : "FAIL") << '\n';
  return r.ok ? core::kExitOk : core::kExitRuntime;
}

int run_spec(ArgCursor &args, std::ostream &out) {
  if (args.has_remaining()) {
    return unexpected_error(out, *args.first_remaining());
  }
  out << "AP63205WU-7: fixed 5V, 1.1MHz, PWM/PFM, 2A max, TSOT26\n";
  out << "VIN 3.8..32 V (internal UVLO 3.1 V), VFB 5.0 V\n";
  out << "RDS(ON) HS 125 mOhm / LS 68 mOhm\n";
  out << "HS peak limit 2.5..3.1 A, LS valley limit 2.5..3.9 A\n";
  out << "tON min 80 ns, soft-start 4 ms, EN high 1.18 V / low 1.10 V\n";
  out << "Table 3: L 4.7uH, CIN 10uF, COUT 2x22uF, BST 100nF\n";
  return core::kExitOk;
}

constexpr std::array<std::string_view, 6> kAp63205Ops{{
    "inductor",
    "cout",
    "uvlo",
    "delay",
    "thermal",
    "spec",
}};

constexpr std::array<std::string_view, 14> kAp63205Options{{
    "--vin", "--iout", "--ripple", "--delta-il", "--esr", "--overshoot", "--von", "--voff",
    "--ts", "--cd", "--pd", "--ta", "--help", "-h",
}};

void push_matches(std::string_view partial, std::span<const std::string_view> options,
                  std::vector<std::string> &out) {
  for (std::string_view o : options) {
    if (o.substr(0, partial.size()) == partial) {
      out.emplace_back(o);
    }
  }
}

} // namespace

int Ap63205Spec::run(ArgCursor &args, std::ostream &out) {
  if (args.take_flag({"--help", "-h"})) {
    out << kName << " - " << kDescription << "\nUsage:\n" << kUsage << '\n';
    return core::kExitOk;
  }
  std::optional<std::string_view> op = args.next_positional();
  if (!op) {
    return usage_error(out, "missing op (inductor|cout|uvlo|delay|thermal|spec)");
  }
  if (*op == "inductor") {
    return run_inductor(args, out);
  }
  if (*op == "cout") {
    return run_cout(args, out);
  }
  if (*op == "uvlo") {
    return run_uvlo(args, out);
  }
  if (*op == "delay") {
    return run_delay(args, out);
  }
  if (*op == "thermal") {
    return run_thermal(args, out);
  }
  if (*op == "spec") {
    return run_spec(args, out);
  }
  return usage_error(out, "unknown op (inductor|cout|uvlo|delay|thermal|spec)");
}

void Ap63205Spec::complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out) {
  if (!partial.empty() && partial.front() == '-') {
    push_matches(partial, kAp63205Options, out);
    return;
  }
  if (prior_args.empty()) {
    push_matches(partial, kAp63205Ops, out);
  }
  // Later positionals are numeric values: nothing to complete.
}

} // namespace wengine::cli::ic
