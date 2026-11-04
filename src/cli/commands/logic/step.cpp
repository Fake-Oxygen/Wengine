#include "common.hpp"

#include "wengine/cli/logic_registry.hpp"
#include "wengine/cli/status.hpp"
#include "wengine/core/status.hpp"
#include "wengine/logic/seq_state.hpp"
#include "wengine/logic/seq_step.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wengine::cli::logic {

using wengine::logic::LogicValue;
using wengine::logic::Netlist;

namespace {

[[nodiscard]] bool is_nonneg_int(std::string_view text) noexcept {
  if (text.empty()) {
    return false;
  }
  for (char c : text) {
    if (c < '0' || c > '9') {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool eq_ci(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    char x = a[i];
    char y = b[i];
    if (x >= 'a' && x <= 'z') {
      x = static_cast<char>(x - ('a' - 'A'));
    }
    if (y >= 'a' && y <= 'z') {
      y = static_cast<char>(y - ('a' - 'A'));
    }
    if (x != y) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] char bit_of(LogicValue v) noexcept {
  return (v == LogicValue::kHigh) ? '1' : '0';
}

} // namespace

int LogicStepSpec::run(ArgCursor &args, std::ostream &out) {
  if (args.take_flag({"--help", "-h"})) {
    out << kName << " - " << kDescription << "\nUsage:\n" << kUsage << '\n';
    return core::kExitOk;
  }
  std::optional<std::string_view> cycles_text = args.take_option({"--cycles"});
  std::vector<std::string_view> positional;
  while (auto p = args.next_positional()) {
    positional.push_back(*p);
  }
  if (args.has_remaining()) {
    return cli::unexpected_error(out, *args.first_remaining(), kUsage);
  }
  if (positional.size() > 2) {
    return cli::usage_error(out, "step takes [DOMAIN] [N]", kUsage);
  }
  // Resolve (domain, N): bare N is all-digits; anything else is a domain.
  bool have_n = false;
  std::uint64_t n = 1;
  bool have_domain = false;
  std::string_view domain_text;
  if (cycles_text) {
    if (!is_nonneg_int(*cycles_text)) {
      return cli::usage_error(out, "step: want N >= 0", kUsage);
    }
    n = 0;
    for (char c : *cycles_text) {
      n = n * 10u + static_cast<std::uint64_t>(c - '0');
    }
    have_n = true;
  }
  auto take_count = [&](std::string_view text) -> core::Status {
    if (!is_nonneg_int(text)) {
      return core::Status::failure(core::StatusCode::kInvalidArgument);
    }
    std::uint64_t v = 0;
    for (char c : text) {
      v = v * 10u + static_cast<std::uint64_t>(c - '0');
    }
    n = v;
    have_n = true;
    return core::Status::success();
  };
  if (positional.size() == 2) {
    have_domain = true;
    domain_text = positional[0];
    if (have_n) {
      return cli::usage_error(out, "step: duplicate count (N and --cycles)", kUsage);
    }
    if (!take_count(positional[1]).ok()) {
      return cli::usage_error(out, "step: want N >= 0", kUsage);
    }
  } else if (positional.size() == 1) {
    if (is_nonneg_int(positional[0])) {
      if (have_n) {
        return cli::usage_error(out, "step: duplicate count (N and --cycles)", kUsage);
      }
      take_count(positional[0]);
    } else {
      have_domain = true;
      domain_text = positional[0];
    }
  }
  Netlist &nl = workspace();
  if (!wengine::logic::has_sequential_gates(nl)) {
    return cli::runtime_error(out, "step: no flops in workspace");
  }
  const std::vector<support::ClockDomain> domains = support::clock_domains(nl);
  // Select members: global (all domains, null member list = exact SEQ4
  // path) or one named domain (case-insensitive).
  const std::uint16_t *members = nullptr;
  std::size_t n_members = 0;
  std::vector<std::uint16_t> selected;
  std::vector<std::size_t> stepped_domains; // indices into domains
  if (have_domain) {
    bool found = false;
    for (std::size_t i = 0; i < domains.size(); ++i) {
      if (eq_ci(domains[i].name, domain_text)) {
        selected = domains[i].members;
        members = selected.data();
        n_members = selected.size();
        stepped_domains.push_back(i);
        found = true;
        break;
      }
    }
    if (!found) {
      std::string want;
      for (const auto &d : domains) {
        if (!want.empty()) {
          want += '|';
        }
        want += d.name;
      }
      return cli::usage_error(out, std::string("step: unknown clock '") + std::string(domain_text) + "' (want " + want + ")", kUsage);
    }
  } else {
    for (std::size_t i = 0; i < domains.size(); ++i) {
      stepped_domains.push_back(i);
    }
  }
  auto is_stepped = [&](std::uint16_t s) {
    if (members == nullptr) {
      return true;
    }
    for (std::size_t i = 0; i < n_members; ++i) {
      if (selected[i] == s) {
        return true;
      }
    }
    return false;
  };
  // Validate atomicity first: compose inputs (sizes) before touching state.
  wengine::logic::SeqState &st = support::seq_state();
  wengine::logic::init_seq_state(st, nl);
  std::vector<LogicValue> inputs;
  if (!support::compose_block_inputs(nl, inputs).ok()) {
    return cli::runtime_error(out, "step failed");
  }
  std::vector<wengine::logic::BlockPin> out_pins;
  std::vector<wengine::logic::BlockPin> in_pins;
  support::interface_pins(nl, in_pins, out_pins);
  if (inputs.size() != in_pins.size()) {
    return cli::runtime_error(out, "step failed");
  }
  // Live async controls act before edges (and before the transition
  // baseline below, so the report shows what stepping itself did);
  // cycle counters only move for edges taken.
  if (!wengine::logic::apply_async_controls(nl, st, std::span<const LogicValue>(inputs.data(), inputs.size())).ok()) {
    return cli::runtime_error(out, "step failed");
  }
  // Level latches settle before edges too (transparent D re-tracks D at
  // every step top; frozen latches hold); the baseline below therefore
  // shows edge-caused changes only.
  if (!wengine::logic::apply_level_latches(nl, st, std::span<const LogicValue>(inputs.data(), inputs.size())).ok()) {
    return cli::runtime_error(out, "step failed");
  }
  // Old Q for the transition report (stepped flops only).
  std::vector<LogicValue> old_q(wengine::logic::kMaxGates, LogicValue::kLow);
  for (std::uint16_t s = 0; s < wengine::logic::kMaxGates; ++s) {
    if (!nl.gates[s].live || !wengine::logic::is_sequential_kind(nl.gates[s].kind) || !is_stepped(s)) {
      continue;
    }
    old_q[s] = st.q[s];
  }
  std::vector<LogicValue> outputs(out_pins.size(), LogicValue::kLow);
  if (n == 0) {
    // No-op report: cycle counts plus current flop state, no stepping.
    for (std::size_t di : stepped_domains) {
      const auto it = support::cycle_counts().find(domains[di].name);
      const std::uint64_t count = (it == support::cycle_counts().end()) ? 0 : it->second;
      out << domains[di].name << ": cycle " << count << " (0 edges, no-op)\n";
    }
    for (std::uint16_t s = 0; s < wengine::logic::kMaxGates; ++s) {
      if (!nl.gates[s].live || !wengine::logic::is_sequential_kind(nl.gates[s].kind) || !is_stepped(s)) {
        continue;
      }
      out << support::pin_text(nl, s, true, 0) << '=' << bit_of(st.q[s]) << " NQ="
          << bit_of(st.q[s] == LogicValue::kHigh ? LogicValue::kLow : LogicValue::kHigh) << " (serial " << st.serial[s] << ")\n";
    }
    return core::kExitOk;
  }
  for (std::uint64_t e = 0; e < n; ++e) {
    const core::Status step_st = wengine::logic::seq_step_filtered(
        nl, st, std::span<const LogicValue>(inputs.data(), inputs.size()),
        std::span<LogicValue>(outputs.data(), outputs.size()), members, n_members);
    if (!step_st.ok()) {
      return cli::runtime_error(out, "step failed");
    }
  }
  // Commit cycle counters only on success (failure leaves state untouched).
  for (std::size_t di : stepped_domains) {
    support::cycle_counts()[domains[di].name] += n;
    out << domains[di].name << ": cycle " << support::cycle_counts()[domains[di].name] << " (" << n << " edges)\n";
  }
  for (std::uint16_t s = 0; s < wengine::logic::kMaxGates; ++s) {
    if (!nl.gates[s].live || !wengine::logic::is_sequential_kind(nl.gates[s].kind) || !is_stepped(s)) {
      continue;
    }
    const LogicValue now = st.q[s];
    if (now == old_q[s]) {
      out << support::pin_text(nl, s, true, 0) << ": " << bit_of(now) << " (no change)\n";
    } else {
      out << support::pin_text(nl, s, true, 0) << ": " << bit_of(old_q[s]) << "->" << bit_of(now) << " (NQ "
          << bit_of(old_q[s] == LogicValue::kHigh ? LogicValue::kLow : LogicValue::kHigh) << "->"
          << bit_of(now == LogicValue::kHigh ? LogicValue::kLow : LogicValue::kHigh) << ")\n";
    }
  }
  for (std::size_t o = 0; o < out_pins.size(); ++o) {
    out << support::pin_text(nl, out_pins[o].slot, true, 0, out_pins[o].negated) << '=' << bit_of(outputs[o]) << '\n';
  }
  return core::kExitOk;
}

void LogicStepSpec::complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out) {
  constexpr std::string_view kCyclesEq = "--cycles=";
  if (support::starts_with(partial, kCyclesEq)) {
    return; // numeric value: nothing to complete
  }
  if (support::starts_with(partial, "--")) {
    constexpr std::string_view kOpts[] = {"--cycles", "--cycles=", "--help", "-h"};
    support::push_matches(partial, kOpts, out);
    return;
  }
  if (!prior_args.empty()) {
    const std::string_view last = prior_args.back();
    if (last == "--cycles" || is_nonneg_int(last)) {
      return; // count position: numeric
    }
    if (prior_args.size() >= 2) {
      return; // [DOMAIN] [N] already filled
    }
    return; // second positional is the count: numeric
  }
  // First positional: domain names (canonical spellings).
  const Netlist &nl = workspace();
  for (const auto &d : support::clock_domains(nl)) {
    if (d.name.size() >= partial.size() && d.name.substr(0, partial.size()) == partial) {
      out.emplace_back(d.name);
    }
  }
}

} // namespace wengine::cli::logic
