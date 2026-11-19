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
  std::optional<std::string_view> first = args.next_positional();
  std::optional<std::string_view> second = args.next_positional();
  if (!first) {
    return cli::usage_error(out, "unwire needs TO.i (e.g. unwire NOT1.1, unwire AND1.Y NOT1.1)", kUsage);
  }
  if (args.has_remaining()) {
    return cli::unexpected_error(out, *args.first_remaining(), kUsage);
  }
  Netlist &nl = workspace();
  // Two-argument form: unwire FROM.out TO.in releases exactly that edge
  // (a shared extra compacts; the primary promotes extra 0). One-argument
  // form: unwire TO.i releases the primary (promotion included).
  if (second) {
    const auto from = wengine::logic::parse_pin(nl, *first);
    if (!from.ok() || !from.value.is_output) {
      return cli::usage_error(out, std::string("bad source pin '") + std::string(*first) + "' (want NAME.out)", kUsage);
    }
    const auto to = wengine::logic::parse_pin(nl, *second);
    if (!to.ok() || to.value.is_output) {
      return cli::usage_error(out, std::string("bad destination pin '") + std::string(*second) + "' (want NAME.in)", kUsage);
    }
    const core::Status st = wengine::logic::unwire_driver(nl, from.value.slot, to.value.slot, to.value.input);
    if (!st.ok()) {
      return cli::usage_error(out, "edge is not attached", kUsage);
    }
    out << "unwired " << support::pin_text(nl, from.value.slot, true, 0, from.value.negated) << " -> "
        << support::pin_text(nl, to.value.slot, false, to.value.input) << '\n';
    return core::kExitOk;
  }
  const auto pin = wengine::logic::parse_pin(nl, *first);
  if (!pin.ok()) {
    return cli::usage_error(out, std::string("bad pin '") + std::string(*first) + "' (want NAME.i)", kUsage);
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
  // Two-argument form (unwire FROM.out TO.in): no pins yet offers sources
  // and destinations; after a source pin, destinations only. Single-pin
  // (unwire TO.i) completes destinations.
  std::size_t n_pins = 0;
  bool first_is_source = false;
  const Netlist &nl = workspace();
  for (std::string_view t : prior_args) {
    if (!t.empty() && t.front() == '-') {
      continue;
    }
    ++n_pins;
    if (n_pins == 1) {
      const auto p = wengine::logic::parse_pin(nl, t);
      first_is_source = p.ok() && p.value.is_output;
    }
  }
  if (n_pins == 0) {
    support::collect_pins(nl, true, false, partial, out);  // sources: outputs
    support::collect_pins(nl, false, true, partial, out);   // ...or the destination
  } else if (n_pins == 1 && first_is_source) {
    support::collect_pins(nl, false, true, partial, out); // destinations: inputs
  }
}

} // namespace wengine::cli::logic
