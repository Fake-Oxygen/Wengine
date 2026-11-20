#include "common.hpp"

#include "wengine/cli/logic_registry.hpp"
#include "wengine/cli/status.hpp"
#include "wengine/core/status.hpp"
#include "wengine/logic/netlist.hpp"

#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wengine::cli::logic {

using wengine::logic::Netlist;

int LogicUnpullSpec::run(ArgCursor &args, std::ostream &out) {
  if (support::handle_help(args, out, kName, kDescription, kUsage)) {
    return core::kExitOk;
  }
  std::vector<std::string_view> pos;
  if (!support::take_positionals(args, out, kUsage, pos, 1, 1, "unpull needs TO.i (e.g. unpull BUS.1)")) {
    return core::kExitUsage;
  }
  Netlist &nl = workspace();
  wengine::logic::PinRef pin{};
  if (!support::parse_input_pin(nl, pos[0], pin, out, kUsage, "unpull needs an input pin TO.i (outputs carry no pull)")) {
    return core::kExitUsage;
  }
  // Engine owns the lifecycle: unpulled clear refuses loudly (exit 2),
  // same precedent as unwire ("input is not wired").
  const core::Status st = wengine::logic::clear_pull(nl, pin.slot, pin.input);
  if (!st.ok()) {
    return cli::usage_error(out, "input has no pull set", kUsage);
  }
  out << "unpulled " << support::pin_text(nl, pin.slot, false, pin.input) << '\n';
  return core::kExitOk;
}

void LogicUnpullSpec::complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out) {
  constexpr std::string_view kOpts[] = {"--help", "-h"};
  if (support::complete_opts(partial, kOpts, out)) {
    return;
  }
  for (std::string_view t : prior_args) {
    if (t.empty() || t.front() != '-') {
      return; // pin given: nothing more
    }
  }
  support::collect_pins(workspace(), false, true, partial, out);
}

} // namespace wengine::cli::logic
