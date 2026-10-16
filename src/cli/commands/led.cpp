#include "wengine/cli/commands.hpp"
#include "wengine/cli/status.hpp"
#include "wengine/solvers/led.hpp"

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
  return cli::usage_error(out, msg, LedSpec::kUsage);
}

void push_matches(std::string_view partial, std::span<const std::string_view> options, std::vector<std::string> &out) {
  for (std::string_view o : options) {
    if (o.substr(0, partial.size()) == partial) {
      out.emplace_back(o);
    }
  }
}

constexpr std::array<std::string_view, 7> kLedOptions{{
    "--vs", "--vf", "--if", "--rs", "--prating", "--help", "-h",
}};

} // namespace

int LedSpec::run(ArgCursor &args, std::ostream &out) {
  if (args.take_flag({"--help", "-h"})) {
    out << kName << " - " << kDescription << "\nUsage:\n" << kUsage << '\n';
    return core::kExitOk;
  }

  std::optional<double> vs_v;
  std::optional<double> vf_v;
  std::optional<double> if_a;
  std::optional<double> rs_ohm;
  double prating_w = std::numeric_limits<double>::infinity();
  bool has_prating = false;

  if (auto o = args.take_option({"--vs"})) {
    auto p = parse_voltage(*o);
    if (!p.ok()) {
      return usage_error(out, "invalid VS value");
    }
    vs_v = p.value;
  }
  if (auto o = args.take_option({"--vf"})) {
    auto p = parse_voltage(*o);
    if (!p.ok()) {
      return usage_error(out, "invalid VF value");
    }
    vf_v = p.value;
  }
  if (auto o = args.take_option({"--if"})) {
    auto p = parse_current(*o);
    if (!p.ok()) {
      return usage_error(out, "invalid IF value (e.g. 20mA)");
    }
    if_a = p.value;
  }
  if (auto o = args.take_option({"--rs"})) {
    auto p = parse_resistance(*o);
    if (!p.ok()) {
      return usage_error(out, "invalid Rs value");
    }
    rs_ohm = p.value;
  }
  if (auto o = args.take_option({"--prating"})) {
    auto p = parse_power(*o);
    if (!p.ok()) {
      return usage_error(out, "invalid power-rating value (e.g. 0.25W)");
    }
    prating_w = p.value;
    has_prating = true;
  }

  // Named-options only: VS/VF share the V suffix, so bare/positional
  // voltages would be ambiguous. Stray positionals are errors, never ignored.
  if (auto p = args.next_positional()) {
    return cli::unexpected_error(out, *p, kUsage);
  }
  if (args.has_remaining()) {
    return cli::unexpected_error(out, *args.first_remaining(), kUsage);
  }

  if (!vs_v || !vf_v || !if_a) {
    return usage_error(out, "need --vs, --vf and --if");
  }
  if (*vs_v <= 0.0) {
    return usage_error(out, "VS must be > 0");
  }
  if (*vf_v <= 0.0) {
    return usage_error(out, "VF must be > 0");
  }
  if (*if_a <= 0.0) {
    return usage_error(out, "IF must be > 0");
  }
  if (rs_ohm && *rs_ohm <= 0.0) {
    return usage_error(out, "Rs must be > 0");
  }
  if (has_prating && !(prating_w > 0.0)) {
    return usage_error(out, "power rating must be > 0");
  }

  const auto r = solvers::LedSolver::solve(*vs_v, *vf_v, *if_a, rs_ohm.value_or(0.0), prating_w);

  out << "VS = " << *vs_v << " V, VF = " << *vf_v << " V, IF target = " << *if_a << " A\n";
  out << "headroom = " << r.headroom_v << " V " << (r.feasible ? "OK" : "INFEASIBLE") << '\n';
  if (!r.feasible) {
    out << "Error: VS must exceed VF (no positive Rs exists)\n";
    out << "summary: FAIL\n";
    return core::kExitRuntime;
  }
  out << "Rs exact = " << r.rs_ohm << " Ohm\n";
  out << "Rs(E12) = " << r.rs_e12_ohm << " Ohm\n";
  out << "Rs used = " << r.rs_used_ohm << " Ohm"
      << (rs_ohm ? " (explicit)" : " (E12 pick)") << '\n';
  out << "IF actual = " << r.if_actual_a << " A\n";
  out << "P res = " << r.p_res_w << " W, P led = " << r.p_led_w
      << " W, P total = " << r.p_total_w << " W\n";
  out << "eff = " << r.eff * 100.0 << " %\n";
  if (has_prating) {
    out << "P rating = " << prating_w << " W " << (r.rating_ok ? "OK" : "FAIL") << '\n';
  } else {
    out << "(no power-rating limit)\n";
  }
  out << "summary: " << (r.pass ? "PASS" : "FAIL") << '\n';
  return r.pass ? core::kExitOk : core::kExitRuntime;
}

void LedSpec::complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out) {
  (void)prior_args;
  // All options take numeric values: complete option names only.
  if (!partial.empty() && partial.front() == '-') {
    push_matches(partial, kLedOptions, out);
  }
  // Bare voltages/currents/resistances are numeric: nothing to complete.
}

} // namespace wengine::cli::commands
