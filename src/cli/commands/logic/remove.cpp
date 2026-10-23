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

int LogicRemoveSpec::run(ArgCursor &args, std::ostream &out) {
  if (args.take_flag({"--help", "-h"})) {
    out << kName << " - " << kDescription << "\nUsage:\n" << kUsage << '\n';
    return core::kExitOk;
  }
  std::optional<std::string_view> name = args.next_positional();
  if (!name) {
    return cli::usage_error(out, "remove needs a gate name (e.g. remove AND1)", kUsage);
  }
  if (args.has_remaining()) {
    return cli::unexpected_error(out, *args.first_remaining(), kUsage);
  }
  Netlist &nl = workspace();
  const auto gate = wengine::logic::resolve_gate(nl, *name);
  if (!gate.ok()) {
    return cli::usage_error(out, std::string("unknown gate '") + std::string(*name) + "'", kUsage);
  }
  const std::string shown = support::gate_name(nl, gate.value);
  const core::Status st = wengine::logic::remove_gate(nl, gate.value);
  if (!st.ok()) {
    return cli::runtime_error(out, "remove failed");
  }
  out << "removed " << shown << '\n';
  return core::kExitOk;
}

void LogicRemoveSpec::complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out) {
  if (!partial.empty() && partial.front() == '-') {
    constexpr std::string_view kOpts[] = {"--help", "-h"};
    support::push_matches(partial, kOpts, out);
    return;
  }
  for (std::string_view t : prior_args) {
    if (t.empty() || t.front() != '-') {
      return;
    }
  }
  support::collect_gate_names(workspace(), partial, out);
}

} // namespace wengine::cli::logic
