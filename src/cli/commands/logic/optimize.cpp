#include "common.hpp"

#include "wengine/cli/logic_registry.hpp"
#include "wengine/cli/status.hpp"
#include "wengine/core/status.hpp"
#include "wengine/logic/optimize.hpp"

#include <cstdint>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wengine::cli::logic {

using wengine::logic::BlockPin;
using wengine::logic::Netlist;

namespace {

// Space-joined display names of an interface pin list (slot order,
// deterministic). Empty stays empty (the caller brackets it).
[[nodiscard]] std::string join_pins(const Netlist &nl, const std::vector<BlockPin> &pins, bool is_output) {
  std::string text;
  for (const BlockPin &p : pins) {
    if (!text.empty()) {
      text += ' ';
    }
    text += support::pin_text(nl, p.slot, is_output, p.input);
  }
  return text;
}

} // namespace

int LogicOptimizeSpec::run(ArgCursor &args, std::ostream &out) {
  if (args.take_flag({"--help", "-h"})) {
    out << kName << " - " << kDescription << "\nUsage:\n" << kUsage << '\n';
    return core::kExitOk;
  }
  std::optional<std::string_view> level_text = args.take_option({"--level"});
  std::optional<std::string_view> to_text = args.take_option({"--to"});
  if (args.has_remaining()) {
    return cli::unexpected_error(out, *args.first_remaining(), kUsage);
  }
  wengine::logic::OptimizeOptions opts;
  bool level_o2 = false;
  if (level_text) {
    const auto level = wengine::logic::parse_opt_level(*level_text);
    if (!level.ok()) {
      return cli::usage_error(out, "bad opt level (want O0|O1|O2)", kUsage);
    }
    opts.level = level.value;
    level_o2 = (level.value == wengine::logic::OptLevel::kO2);
  }
  if (to_text) {
    if (!level_o2) {
      return cli::usage_error(out, "optimize --to needs --level O2", kUsage);
    }
    const auto target = wengine::logic::parse_opt_target(*to_text);
    if (!target.ok()) {
      return cli::usage_error(out, "bad opt target (want nand|nor)", kUsage);
    }
    opts.target = target.value;
  }
  Netlist &nl = workspace();
  // Snapshot the pre-optimize interface: folding drops dead pins, O2
  // duplicates pins for inversion (NAND(a,a) = NOT a), and gates are
  // renamed — so the new truth table only matches the old one under pin
  // replication. Any interface change is reported loudly below.
  std::vector<BlockPin> before_in;
  std::vector<BlockPin> before_out;
  support::interface_pins(nl, before_in, before_out);
  const std::string before_in_text = join_pins(nl, before_in, false);
  const std::string before_out_text = join_pins(nl, before_out, true);
  const std::uint16_t before = nl.live_count;
  auto r = wengine::logic::optimize(nl, opts);
  if (!r.ok()) {
    return cli::runtime_error(out, std::string("optimize failed (") + std::string(r.error) + ")");
  }
  nl = r.value;
  std::vector<BlockPin> after_in;
  std::vector<BlockPin> after_out;
  support::interface_pins(nl, after_in, after_out);
  const std::string after_in_text = join_pins(nl, after_in, false);
  const std::string after_out_text = join_pins(nl, after_out, true);
  out << "optimize: " << before << " -> " << nl.live_count << " gates (level=";
  switch (opts.level) {
  case wengine::logic::OptLevel::kO0:
    out << "O0";
    break;
  case wengine::logic::OptLevel::kO1:
    out << "O1";
    break;
  case wengine::logic::OptLevel::kO2:
    out << "O2";
    break;
  }
  if (level_o2) {
    out << " to=" << (opts.target == wengine::logic::OptTarget::kNand ? "nand" : "nor");
  }
  out << ")\n";
  if (before_in_text != after_in_text || before_out_text != after_out_text) {
    out << "interface: " << before_in.size() << " inputs [" << before_in_text << "] -> " << after_in.size()
        << " inputs [" << after_in_text << "]\n";
    out << "interface: " << before_out.size() << " outputs [" << before_out_text << "] -> " << after_out.size()
        << " outputs [" << after_out_text << "]\n";
    out << "note: interface changed (folding drops dead pins, O2 duplicates pins for inversion); re-run truth on "
           "the new pins and compare under pin replication, not row-for-row.\n";
  }
  return core::kExitOk;
}

void LogicOptimizeSpec::complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out) {
  constexpr std::string_view kLevelEq = "--level=";
  constexpr std::string_view kToEq = "--to=";
  if (support::starts_with(partial, kLevelEq)) {
    const std::string_view sub = partial.substr(kLevelEq.size());
    constexpr std::string_view kLevels[] = {"O0", "O1", "O2"};
    for (std::string_view l : kLevels) {
      if (l.substr(0, sub.size()) == sub) {
        out.emplace_back(std::string(kLevelEq) + std::string(l));
      }
    }
    return;
  }
  if (support::starts_with(partial, kToEq)) {
    const std::string_view sub = partial.substr(kToEq.size());
    constexpr std::string_view kTargets[] = {"nand", "nor"};
    for (std::string_view t : kTargets) {
      if (t.substr(0, sub.size()) == sub) {
        out.emplace_back(std::string(kToEq) + std::string(t));
      }
    }
    return;
  }
  if (!partial.empty() && partial.front() == '-') {
    constexpr std::string_view kOpts[] = {"--level", "--to", "--help", "-h"};
    support::push_matches(partial, kOpts, out);
    return;
  }
  if (!prior_args.empty() && prior_args.back() == "--level") {
    constexpr std::string_view kLevels[] = {"O0", "O1", "O2"};
    support::push_matches(partial, kLevels, out);
    return;
  }
  if (!prior_args.empty() && prior_args.back() == "--to") {
    constexpr std::string_view kTargets[] = {"nand", "nor"};
    support::push_matches(partial, kTargets, out);
  }
}

} // namespace wengine::cli::logic
