#include "common.hpp"

#include "wengine/cli/logic_registry.hpp"
#include "wengine/cli/status.hpp"
#include "wengine/core/status.hpp"
#include "wengine/logic/netlist.hpp"

#include <cstdint>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wengine::cli::logic {

using wengine::logic::Netlist;

int LogicUntieSpec::run(ArgCursor &args, std::ostream &out) {
  if (args.take_flag({"--help", "-h"})) {
    out << kName << " - " << kDescription << "\nUsage:\n" << kUsage << '\n';
    return core::kExitOk;
  }
  std::optional<std::string_view> pin_text_arg = args.next_positional();
  if (!pin_text_arg) {
    return cli::usage_error(out, "untie needs a pin (e.g. untie NAND1.1)", kUsage);
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
    return cli::usage_error(out, "untie needs an input pin (outputs are never tied)", kUsage);
  }
  const auto &g = nl.gates[pin.value.slot];
  if (g.drivers[pin.value.input] != wengine::logic::kNoDriver) {
    return cli::usage_error(out, "untie needs a dangling pin (unwire first)", kUsage);
  }
  if ((g.ignored_mask & static_cast<std::uint8_t>(1u << pin.value.input)) != 0) {
    return cli::usage_error(out, "untie needs a non-ignored pin (use first)", kUsage);
  }
  if (!wengine::logic::is_tied(nl, pin.value.slot, pin.value.input)) {
    return cli::usage_error(out, "pin is not tied", kUsage);
  }
  if (!wengine::logic::untie_pin(nl, pin.value).ok()) {
    return cli::usage_error(out, "untie failed", kUsage);
  }
  out << "untied " << support::pin_text(nl, pin.value.slot, false, pin.value.input) << '\n';
  return core::kExitOk;
}

void LogicUntieSpec::complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out) {
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
  // Offer tied input pins first (the only pins untie accepts); fall back
  // to all inputs when nothing is tied so completion never goes silent.
  const Netlist &nl = workspace();
  bool any_tied = false;
  for (std::uint16_t s = 0; s < wengine::logic::kMaxGates; ++s) {
    const auto &g = nl.gates[s];
    if (!g.live) {
      continue;
    }
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      if (!wengine::logic::is_tied(nl, s, i)) {
        continue;
      }
      const std::string p = support::pin_text(nl, s, false, i);
      if (p.size() >= partial.size() && p.substr(0, partial.size()) == partial) {
        out.push_back(p);
        any_tied = true;
      }
    }
  }
  if (!any_tied) {
    support::collect_pins(nl, false, true, partial, out);
  }
}

} // namespace wengine::cli::logic
