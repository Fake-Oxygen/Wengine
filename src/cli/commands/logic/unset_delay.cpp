#include "common.hpp"

#include "wengine/cli/logic_registry.hpp"
#include "wengine/cli/status.hpp"
#include "wengine/core/status.hpp"
#include "wengine/logic/netlist.hpp"
#include "wengine/logic/pins.hpp"

#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wengine::cli::logic {

using wengine::logic::Netlist;

int LogicUnsetDelaySpec::run(ArgCursor &args, std::ostream &out) {
  if (support::handle_help(args, out, kName, kDescription, kUsage)) {
    return core::kExitOk;
  }
  std::vector<std::string_view> pos;
  if (!support::take_positionals(args, out, kUsage, pos, 1, 1, "unset-delay needs GATE (e.g. unset-delay AND1)")) {
    return core::kExitUsage;
  }
  Netlist &nl = workspace();
  const auto gate = wengine::logic::resolve_gate(nl, pos[0]);
  if (!gate.ok()) {
    return cli::usage_error(out, std::string("unknown gate '") + std::string(pos[0]) + "'", kUsage);
  }
  // Engine owns the lifecycle: clearing a zero field refuses loudly
  // (exit 2), same precedent as unpull ("input has no pull set").
  if (!wengine::logic::clear_gate_delay(nl, gate.value).ok()) {
    return cli::usage_error(out, std::string("no delay set on ") + support::gate_name(nl, gate.value), kUsage);
  }
  out << "cleared " << support::gate_name(nl, gate.value) << " delay\n";
  return core::kExitOk;
}

void LogicUnsetDelaySpec::complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out) {
  if (!partial.empty() && partial.front() == '-') {
    constexpr std::string_view kOpts[] = {"--help", "-h"};
    support::push_matches(partial, kOpts, out);
    return;
  }
  for (std::string_view t : prior_args) {
    if (t.empty() || t.front() != '-') {
      return; // gate given: nothing more
    }
  }
  support::collect_gate_names(workspace(), partial, out);
}

} // namespace wengine::cli::logic
