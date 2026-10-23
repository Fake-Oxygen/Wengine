#include "common.hpp"

#include "wengine/cli/logic_registry.hpp"
#include "wengine/cli/status.hpp"
#include "wengine/core/status.hpp"
#include "wengine/logic/netlist.hpp"
#include "wengine/logic/seq_state.hpp"

#include <cstdint>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wengine::cli::logic {

using wengine::logic::Netlist;

int LogicWireSpec::run(ArgCursor &args, std::ostream &out) {
  if (args.take_flag({"--help", "-h"})) {
    out << kName << " - " << kDescription << "\nUsage:\n" << kUsage << '\n';
    return core::kExitOk;
  }
  std::optional<std::string_view> from_text = args.next_positional();
  std::optional<std::string_view> to_text = args.next_positional();
  if (!from_text || !to_text) {
    return cli::usage_error(out, "wire needs FROM.Y and TO.i (e.g. wire AND1.Y NOT1.1)", kUsage);
  }
  if (args.has_remaining()) {
    return cli::unexpected_error(out, *args.first_remaining(), kUsage);
  }
  Netlist &nl = workspace();
  const auto from = wengine::logic::parse_pin(nl, *from_text);
  if (!from.ok()) {
    return cli::usage_error(out, std::string("bad source pin '") + std::string(*from_text) + "' (want NAME.Y)", kUsage);
  }
  const auto to = wengine::logic::parse_pin(nl, *to_text);
  if (!to.ok()) {
    return cli::usage_error(out, std::string("bad destination pin '") + std::string(*to_text) + "' (want NAME.i)", kUsage);
  }
  if (!from.value.is_output || to.value.is_output) {
    return cli::usage_error(out, "wire needs FROM.Y (output) and TO.i (input)", kUsage);
  }
  const auto &dst = nl.gates[to.value.slot];
  if (dst.drivers[to.value.input] != wengine::logic::kNoDriver) {
    return cli::usage_error(out, "input already wired (unwire it first)", kUsage);
  }
  if (nl.tie_rep[wengine::logic::tie_flat(to.value.slot, to.value.input)] != 0) {
    return cli::usage_error(out, "input is tied (untie it first)", kUsage);
  }
  if (wengine::logic::has_sequential_gates(nl)) {
    // Sequential netlist (LOGIC-V2-SEQ3): flop Q outputs break loops, so
    // the comb-only reachability check would false-positive on legal
    // feedback (DFF Q -> comb -> D). Tentatively wire, validate the whole
    // graph over comb edges only, then revert: wire_pins() below stays the
    // single mutation path (its internal reaches_seq agrees with the
    // validator — both are exact single-edge cycle tests). Dormant until
    // YAML v3 (SEQ10) introduces sequential gates; add/load reject them.
    wengine::logic::Gate &dst_gate = nl.gates[to.value.slot];
    wengine::logic::Gate &src_gate = nl.gates[from.value.slot];
    dst_gate.drivers[to.value.input] = from.value.slot;
    ++src_gate.output_fanout;
    const bool acyclic = wengine::logic::validate_sequential(nl).ok();
    dst_gate.drivers[to.value.input] = wengine::logic::kNoDriver;
    --src_gate.output_fanout;
    if (!acyclic) {
      return cli::usage_error(out, "wire would close a combinational cycle", kUsage);
    }
  } else if (wengine::logic::detail::reaches(nl, to.value.slot, from.value.slot)) {
    return cli::usage_error(out, "wire would close a combinational cycle", kUsage);
  }
  const core::Status st = wengine::logic::wire_pins(nl, from.value, to.value);
  if (!st.ok()) {
    return cli::usage_error(out, "invalid wire", kUsage);
  }
  out << "wired " << support::pin_text(nl, from.value.slot, true, 0) << " -> "
      << support::pin_text(nl, to.value.slot, false, to.value.input) << '\n';
  return core::kExitOk;
}

void LogicWireSpec::complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out) {
  if (!partial.empty() && partial.front() == '-') {
    constexpr std::string_view kOpts[] = {"--help", "-h"};
    support::push_matches(partial, kOpts, out);
    return;
  }
  std::size_t n_pins = 0;
  for (std::string_view t : prior_args) {
    if (!t.empty() && t.front() == '-') {
      continue;
    }
    ++n_pins;
  }
  const Netlist &nl = workspace();
  if (n_pins == 0) {
    support::collect_pins(nl, true, false, partial, out); // sources: outputs
  } else if (n_pins == 1) {
    support::collect_pins(nl, false, true, partial, out); // destinations: inputs
  }
}

} // namespace wengine::cli::logic
