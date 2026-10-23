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

int LogicUnwireSpec::run(ArgCursor &args, std::ostream &out) {
  if (args.take_flag({"--help", "-h"})) {
    out << kName << " - " << kDescription << "\nUsage:\n" << kUsage << '\n';
    return core::kExitOk;
  }
  std::optional<std::string_view> pin_text_arg = args.next_positional();
  if (!pin_text_arg) {
    return cli::usage_error(out, "unwire needs TO.i (e.g. unwire NOT1.1)", kUsage);
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
    return cli::usage_error(out, "unwire needs an input pin TO.i (outputs carry no wire)", kUsage);
  }
  const core::Status st = wengine::logic::unwire_pin(nl, pin.value);
  if (!st.ok()) {
    return cli::usage_error(out, "input is not wired", kUsage);
  }
  out << "unwired " << support::pin_text(nl, pin.value.slot, false, pin.value.input) << '\n';
  return core::kExitOk;
}

void LogicUnwireSpec::complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out) {
  if (!partial.empty() && partial.front() == '-') {
    constexpr std::string_view kOpts[] = {"--help", "-h"};
    support::push_matches(partial, kOpts, out);
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
