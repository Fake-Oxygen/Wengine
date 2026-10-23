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

using wengine::logic::Netlist;

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
  const std::uint16_t before = nl.live_count;
  auto r = wengine::logic::optimize(nl, opts);
  if (!r.ok()) {
    return cli::runtime_error(out, std::string("optimize failed (") + std::string(r.error) + ")");
  }
  nl = r.value;
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
