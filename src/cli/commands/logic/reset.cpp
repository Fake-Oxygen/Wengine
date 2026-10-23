#include "common.hpp"

#include "wengine/cli/logic_registry.hpp"
#include "wengine/cli/status.hpp"
#include "wengine/core/status.hpp"

#include <cstdint>
#include <ostream>
#include <span>
#include <string_view>
#include <vector>

namespace wengine::cli::logic {

using wengine::logic::Netlist;

int LogicResetSpec::run(ArgCursor &args, std::ostream &out) {
  if (args.take_flag({"--help", "-h"})) {
    out << kName << " - " << kDescription << "\nUsage:\n" << kUsage << '\n';
    return core::kExitOk;
  }
  (void)args.take_flag({"--force"});
  if (args.has_remaining()) {
    return cli::unexpected_error(out, *args.first_remaining(), kUsage);
  }
  Netlist &nl = workspace();
  const std::uint16_t cleared = nl.live_count;
  wengine::logic::reset_netlist(nl);
  out << "reset: cleared " << cleared << " gates\n";
  return core::kExitOk;
}

void LogicResetSpec::complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out) {
  (void)prior_args;
  if (partial.empty() || partial.front() != '-') {
    return;
  }
  constexpr std::string_view kOpts[] = {"--force", "--help", "-h"};
  support::push_matches(partial, kOpts, out);
}

} // namespace wengine::cli::logic
