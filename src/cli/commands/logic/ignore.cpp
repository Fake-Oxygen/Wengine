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

int LogicIgnoreSpec::run(ArgCursor &args, std::ostream &out) {
  if (args.take_flag({"--help", "-h"})) {
    out << kName << " - " << kDescription << "\nUsage:\n" << kUsage << '\n';
    return core::kExitOk;
  }
  std::optional<std::string_view> pin_text_arg = args.next_positional();
  if (!pin_text_arg) {
    return cli::usage_error(out, "ignore needs a pin (e.g. ignore AND1.2)", kUsage);
  }
  if (args.has_remaining()) {
    return cli::unexpected_error(out, *args.first_remaining(), kUsage);
  }
  Netlist &nl = workspace();
  const auto pin = wengine::logic::parse_pin(nl, *pin_text_arg);
  if (!pin.ok()) {
    return cli::usage_error(out, std::string("bad pin '") + std::string(*pin_text_arg) + "' (want NAME.Y|NAME.i)", kUsage);
  }
  if (!pin.value.is_output &&
      nl.tie_rep[wengine::logic::tie_flat(pin.value.slot, pin.value.input)] != 0) {
    return cli::usage_error(out, "pin is tied (untie it first)", kUsage);
  }
  if (!wengine::logic::ignore_pin(nl, pin.value).ok()) {
    return cli::runtime_error(out, "ignore failed");
  }
  out << "ignored " << support::pin_text(nl, pin.value.slot, pin.value.is_output, pin.value.input) << '\n';
  return core::kExitOk;
}

void LogicIgnoreSpec::complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out) {
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
  support::collect_pins(workspace(), true, true, partial, out);
}

} // namespace wengine::cli::logic
