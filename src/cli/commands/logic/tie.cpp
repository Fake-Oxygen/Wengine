#include "common.hpp"

#include "wengine/cli/logic_registry.hpp"
#include "wengine/cli/status.hpp"
#include "wengine/core/status.hpp"
#include "wengine/logic/netlist.hpp"

#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wengine::cli::logic {

using wengine::logic::Netlist;

int LogicTieSpec::run(ArgCursor &args, std::ostream &out) {
  if (args.take_flag({"--help", "-h"})) {
    out << kName << " - " << kDescription << "\nUsage:\n" << kUsage << '\n';
    return core::kExitOk;
  }
  std::optional<std::string_view> a_text = args.next_positional();
  std::optional<std::string_view> b_text = args.next_positional();
  if (!a_text || !b_text) {
    return cli::usage_error(out, "tie needs two input pins (e.g. tie NAND1.1 NAND1.2)", kUsage);
  }
  if (args.has_remaining()) {
    return cli::unexpected_error(out, *args.first_remaining(), kUsage);
  }
  Netlist &nl = workspace();
  const auto a = wengine::logic::parse_pin(nl, *a_text);
  if (!a.ok()) {
    return cli::usage_error(out, std::string("bad pin '") + std::string(*a_text) + "' (want NAME.i)", kUsage);
  }
  const auto b = wengine::logic::parse_pin(nl, *b_text);
  if (!b.ok()) {
    return cli::usage_error(out, std::string("bad pin '") + std::string(*b_text) + "' (want NAME.i)", kUsage);
  }
  if (a.value.is_output || b.value.is_output) {
    return cli::usage_error(out, "tie needs two input pins (outputs cannot be tied)", kUsage);
  }
  if (a.value.slot == b.value.slot && a.value.input == b.value.input) {
    return cli::usage_error(out, "tie needs two distinct pins", kUsage);
  }
  const auto &ga = nl.gates[a.value.slot];
  const auto &gb = nl.gates[b.value.slot];
  if (ga.drivers[a.value.input] != wengine::logic::kNoDriver ||
      gb.drivers[b.value.input] != wengine::logic::kNoDriver) {
    return cli::usage_error(out, "tie needs dangling pins (unwire first)", kUsage);
  }
  if (((ga.ignored_mask & static_cast<std::uint8_t>(1u << a.value.input)) != 0) ||
      ((gb.ignored_mask & static_cast<std::uint8_t>(1u << b.value.input)) != 0)) {
    return cli::usage_error(out, "tie needs non-ignored pins (use first)", kUsage);
  }
  if (!wengine::logic::tie_pins(nl, a.value, b.value).ok()) {
    return cli::usage_error(out, "tie failed", kUsage);
  }
  out << "tied " << support::pin_text(nl, a.value.slot, false, a.value.input) << " = "
      << support::pin_text(nl, b.value.slot, false, b.value.input) << '\n';
  return core::kExitOk;
}

void LogicTieSpec::complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out) {
  if (!partial.empty() && partial.front() == '-') {
    constexpr std::string_view kOpts[] = {"--help", "-h"};
    support::push_matches(partial, kOpts, out);
    return;
  }
  std::size_t n_pins = 0;
  for (std::string_view t : prior_args) {
    if (!t.empty() && t.front() == '-') {
      continue;
    }
    ++n_pins;
  }
  if (n_pins >= 2) {
    return;
  }
  support::collect_pins(workspace(), false, true, partial, out);
}

} // namespace wengine::cli::logic
