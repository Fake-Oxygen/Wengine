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

int LogicPullSpec::run(ArgCursor &args, std::ostream &out) {
  if (args.take_flag({"--help", "-h"})) {
    out << kName << " - " << kDescription << "\nUsage:\n" << kUsage << '\n';
    return core::kExitOk;
  }
  std::optional<std::string_view> pin_text_arg = args.next_positional();
  std::optional<std::string_view> level_text = args.next_positional();
  if (!pin_text_arg || !level_text) {
    return cli::usage_error(out, "pull needs TO.i and a level (e.g. pull BUS.1 up)", kUsage);
  }
  if (args.has_remaining()) {
    return cli::unexpected_error(out, *args.first_remaining(), kUsage);
  }
  Netlist &nl = workspace();
  const auto pin = wengine::logic::parse_pin(nl, *pin_text_arg);
  if (!pin.ok()) {
    return cli::usage_error(out, std::string("bad pin '") + std::string(*pin_text_arg) + "' (want NAME.i)", kUsage);
  }
  if (pin.value.is_output) {
    return cli::usage_error(out, "pull needs an input pin TO.i (outputs carry no pull)", kUsage);
  }
  std::uint8_t level = 0;
  if (*level_text == "up") {
    level = 1;
  } else if (*level_text == "down") {
    level = 2;
  } else {
    return cli::usage_error(out, "bad pull level (want up|down)", kUsage);
  }
  if (wengine::logic::is_tied(nl, pin.value.slot, pin.value.input)) {
    return cli::usage_error(out, "input is tied (untie it first)", kUsage);
  }
  if ((nl.gates[pin.value.slot].ignored_mask & static_cast<std::uint8_t>(1u << pin.value.input)) != 0) {
    return cli::usage_error(out, "input is ignored (use it first)", kUsage);
  }
  if (!wengine::logic::set_pull(nl, pin.value.slot, pin.value.input, level).ok()) {
    return cli::runtime_error(out, "pull failed");
  }
  out << "pulled " << support::pin_text(nl, pin.value.slot, false, pin.value.input) << ' '
      << (*level_text) << '\n';
  return core::kExitOk;
}

void LogicPullSpec::complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out) {
  if (!partial.empty() && partial.front() == '-') {
    constexpr std::string_view kOpts[] = {"--help", "-h"};
    support::push_matches(partial, kOpts, out);
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
