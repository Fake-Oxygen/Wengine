#include "common.hpp"

#include "wengine/cli/logic_registry.hpp"
#include "wengine/cli/status.hpp"
#include "wengine/core/status.hpp"
#include "wengine/logic/netlist.hpp"

#include <cstdint>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wengine::cli::logic {

using wengine::logic::Netlist;

int LogicPullSpec::run(ArgCursor &args, std::ostream &out) {
  if (support::handle_help(args, out, kName, kDescription, kUsage)) {
    return core::kExitOk;
  }
  std::vector<std::string_view> pos;
  if (!support::take_positionals(args, out, kUsage, pos, 2, 2, "pull needs TO.i and a level (e.g. pull BUS.1 up)")) {
    return core::kExitUsage;
  }
  const std::string_view pin_text_arg = pos[0];
  const std::string_view level_text = pos[1];
  Netlist &nl = workspace();
  wengine::logic::PinRef pin{};
  if (!support::parse_input_pin(nl, pin_text_arg, pin, out, kUsage, "pull needs an input pin TO.i (outputs carry no pull)")) {
    return core::kExitUsage;
  }
  std::uint8_t level = 0;
  if (level_text == "up") {
    level = 1;
  } else if (level_text == "down") {
    level = 2;
  } else {
    return cli::usage_error(out, "bad pull level (want up|down)", kUsage);
  }
  if (wengine::logic::is_tied(nl, pin.slot, pin.input)) {
    return cli::usage_error(out, "input is tied (untie it first)", kUsage);
  }
  if ((nl.gates[pin.slot].ignored_mask & static_cast<std::uint8_t>(1u << pin.input)) != 0) {
    return cli::usage_error(out, "input is ignored (use it first)", kUsage);
  }
  if (!wengine::logic::set_pull(nl, pin.slot, pin.input, level).ok()) {
    return cli::usage_error(out, "pull failed", kUsage);
  }
  out << "pulled " << support::pin_text(nl, pin.slot, false, pin.input) << ' ' << level_text << '\n';
  return core::kExitOk;
}

void LogicPullSpec::complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out) {
  constexpr std::string_view kOpts[] = {"--help", "-h"};
  if (support::complete_opts(partial, kOpts, out)) {
    return;
  }
  bool have_pin = false;
  for (std::string_view t : prior_args) {
    if (t.empty() || t.front() == '-') {
      continue;
    }
    if (!have_pin) {
      have_pin = true;
      continue;
    }
    return; // pin and level given: nothing more
  }
  if (!have_pin) {
    support::collect_pins(workspace(), false, true, partial, out);
    return;
  }
  constexpr std::string_view kLevels[] = {"up", "down"};
  support::push_matches(partial, kLevels, out);
}

} // namespace wengine::cli::logic
