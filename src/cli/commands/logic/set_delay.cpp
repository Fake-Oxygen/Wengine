#include "common.hpp"

#include "wengine/cli/logic_registry.hpp"
#include "wengine/cli/status.hpp"
#include "wengine/cli/time_ps.hpp"
#include "wengine/core/status.hpp"
#include "wengine/logic/netlist.hpp"
#include "wengine/logic/pins.hpp"

#include <cstdint>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wengine::cli::logic {

using wengine::logic::Netlist;

int LogicSetDelaySpec::run(ArgCursor &args, std::ostream &out) {
  if (support::handle_help(args, out, kName, kDescription, kUsage)) {
    return core::kExitOk;
  }
  std::vector<std::string_view> pos;
  if (!support::take_positionals(args, out, kUsage, pos, 2, 2, "set-delay needs GATE and VALUE (e.g. set-delay AND1 1.5ns)",
                                 /*include_options=*/true)) {
    return core::kExitUsage;
  }
  Netlist &nl = workspace();
  const auto gate = wengine::logic::resolve_gate(nl, pos[0]);
  if (!gate.ok()) {
    return cli::usage_error(out, std::string("unknown gate '") + std::string(pos[0]) + "'", kUsage);
  }
  const auto value = cli::parse_time_ps(pos[1]);
  if (!value.ok()) {
    return cli::usage_error(out, value.error, kUsage);
  }
  // Engine owns the lifecycle: re-set replaces (last write wins); zero
  // is a legal write equal to clearing. Live-slot validated here.
  if (!wengine::logic::set_gate_delay(nl, gate.value, value.value).ok()) {
    return cli::usage_error(out, std::string("unknown gate '") + std::string(pos[0]) + "'", kUsage);
  }
  out << "set " << support::gate_name(nl, gate.value) << " delay=" << value.value << "ps\n";
  return core::kExitOk;
}

void LogicSetDelaySpec::complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out) {
  if (!partial.empty() && partial.front() == '-') {
    constexpr std::string_view kOpts[] = {"--help", "-h"};
    support::push_matches(partial, kOpts, out);
    return;
  }
  for (std::string_view t : prior_args) {
    if (t.empty() || t.front() != '-') {
      return; // gate given: values are free-form, nothing to offer
    }
  }
  support::collect_gate_names(workspace(), partial, out);
}

} // namespace wengine::cli::logic
