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

int LogicUnshareSpec::run(ArgCursor &args, std::ostream &out) {
  if (args.take_flag({"--help", "-h"})) {
    out << kName << " - " << kDescription << "\nUsage:\n" << kUsage << '\n';
    return core::kExitOk;
  }
  std::optional<std::string_view> pin_text_arg = args.next_positional();
  if (!pin_text_arg) {
    return cli::usage_error(out, "unshare needs TO.i (e.g. unshare BUS.1)", kUsage);
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
    return cli::usage_error(out, "unshare needs an input pin TO.i (outputs carry no mark)", kUsage);
  }
  if (!wengine::logic::is_shared_input(nl, pin.value.slot, pin.value.input)) {
    return cli::usage_error(out, "input is not shared", kUsage);
  }
  const unsigned attached = wengine::logic::shared_attachment_count(nl, pin.value.slot, pin.value.input);
  if (attached > 1) {
    return cli::usage_error(out, "input still has multiple drivers (unwire extras first)", kUsage);
  }
  const core::Status st = wengine::logic::unshare_input(nl, pin.value.slot, pin.value.input);
  if (!st.ok()) {
    return cli::usage_error(out, "input is not shared", kUsage);
  }
  out << "unshared " << support::pin_text(nl, pin.value.slot, false, pin.value.input) << " (" << attached
      << " driver" << (attached == 1 ? "" : "s") << " attached)\n";
  return core::kExitOk;
}

void LogicUnshareSpec::complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out) {
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
