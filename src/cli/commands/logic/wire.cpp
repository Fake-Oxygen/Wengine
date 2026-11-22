#include "common.hpp"

#include "wengine/cli/logic_registry.hpp"
#include "wengine/cli/status.hpp"
#include "wengine/core/status.hpp"
#include "wengine/logic/adjacency.hpp"
#include "wengine/logic/netlist.hpp"
#include "wengine/logic/seq_state.hpp"
#include "wengine/logic/topo.hpp"

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
  const bool want_shared = args.take_flag({"--shared"});
  std::optional<std::string_view> from_text = args.next_positional();
  std::optional<std::string_view> to_text = args.next_positional();
  if (!from_text || !to_text) {
    return cli::usage_error(out, "wire needs FROM.out and TO.in (e.g. wire AND1.Y NOT1.1, wire DFF1.Q AND1.1)", kUsage);
  }
  if (args.has_remaining()) {
    return cli::unexpected_error(out, *args.first_remaining(), kUsage);
  }
  Netlist &nl = workspace();
  const auto from = wengine::logic::parse_pin(nl, *from_text);
  if (!from.ok()) {
    return cli::usage_error(out, std::string("bad source pin '") + std::string(*from_text) + "' (want NAME.out)", kUsage);
  }
  const auto to = wengine::logic::parse_pin(nl, *to_text);
  if (!to.ok()) {
    return cli::usage_error(out, std::string("bad destination pin '") + std::string(*to_text) + "' (want NAME.in)", kUsage);
  }
  if (!from.value.is_output || to.value.is_output) {
    return cli::usage_error(out, "wire needs FROM.out (output) and TO.in (input)", kUsage);
  }
  // The opt-in gate (011): --shared marks the input before landing the
  // driver; plain wire on an unmarked occupied input refuses exactly as
  // before — never a silent overwrite, never an implied mode.
  if (want_shared) {
    const core::Status marked = wengine::logic::mark_shared(nl, to.value.slot, to.value.input);
    if (!marked.ok()) {
      if (wengine::logic::is_tied(nl, to.value.slot, to.value.input)) {
        return cli::usage_error(out, "input is tied (untie it first)", kUsage);
      }
      return cli::usage_error(out, "cannot mark input shared", kUsage);
    }
  }
  const auto &dst = nl.slots[to.value.slot];
  const bool occupied = dst.drivers[to.value.input] != wengine::logic::kNoDriver;
  const bool shared = wengine::logic::is_shared_input(nl, to.value.slot, to.value.input);
  if (occupied && !shared) {
    return cli::usage_error(out, "input already wired (unwire it first)", kUsage);
  }
  if (wengine::logic::is_tied(nl, to.value.slot, to.value.input)) {
    return cli::usage_error(out, "input is tied (untie it first)", kUsage);
  }
  const bool shared_append = occupied && shared;
  if (shared_append &&
      wengine::logic::shared_attachment_count(nl, to.value.slot, to.value.input) == 1 + wengine::logic::kMaxSharedExtra) {
    return cli::usage_error(out, "shared net full (cap 4)", kUsage);
  }
  if (wengine::logic::has_sequential_gates(nl)) {
  // Sequential netlist: flop Q outputs break loops, so the comb-only
  // reachability check would false-positive on legal feedback (DFF Q ->
  // comb -> D). Tentatively wire, validate the whole graph over comb
  // edges only, then revert: wire_pins() below stays the single mutation
  // path (its internal topo::reaches agrees with the validator — both are
  // exact single-edge cycle tests). Polarity/fanout are untouched by the
  // probe (topo::validate reads drivers only), so plain revert is exact.
    if (shared_append) {
      // Shared probe: tentative extra-append through the same wire_pins
      // seam, validated, then exactly reverted (named-edge removal
      // restores fanout, polarity, and the dense extra order).
      const core::Status probe = wengine::logic::wire_pins(nl, from.value, to.value);
      if (!probe.ok()) {
        return cli::usage_error(out, "invalid wire", kUsage);
      }
      const bool acyclic = wengine::logic::topo::validate(nl, wengine::logic::EdgeFilter::kCombOnly).ok();
      (void)wengine::logic::unwire_driver(nl, from.value.slot, to.value.slot, to.value.input);
      if (!acyclic) {
        return cli::usage_error(out, "wire would close a combinational cycle", kUsage);
      }
    } else {
      wengine::logic::Gate &dst_gate = nl.slots[to.value.slot];
      wengine::logic::Gate &src_gate = nl.slots[from.value.slot];
      dst_gate.drivers[to.value.input] = from.value.slot;
      ++src_gate.output_fanout;
      // Adjacency (016): the tentative edge must be visible to the
      // chain-reading validator below, then exactly reverted.
      (void)wengine::logic::adjacency::adj_insert_edge(nl, from.value.slot, to.value.slot, to.value.input, 0);
      const bool acyclic = wengine::logic::topo::validate(nl, wengine::logic::EdgeFilter::kCombOnly).ok();
      (void)wengine::logic::adjacency::adj_unlink_edge(nl, from.value.slot, to.value.slot, to.value.input, 0);
      dst_gate.drivers[to.value.input] = wengine::logic::kNoDriver;
      --src_gate.output_fanout;
      if (!acyclic) {
        return cli::usage_error(out, "wire would close a combinational cycle", kUsage);
      }
    }
  } else if (wengine::logic::topo::reaches(nl, to.value.slot, from.value.slot, wengine::logic::EdgeFilter::kAll)) {
    return cli::usage_error(out, "wire would close a combinational cycle", kUsage);
  }
  const core::Status st = wengine::logic::wire_pins(nl, from.value, to.value);
  if (!st.ok()) {
    return cli::usage_error(out, "invalid wire", kUsage);
  }
  if (shared) {
    out << "wired " << support::pin_text(nl, from.value.slot, true, 0, from.value.negated) << " -> "
        << support::pin_text(nl, to.value.slot, false, to.value.input) << " (shared: "
        << static_cast<unsigned>(wengine::logic::shared_attachment_count(nl, to.value.slot, to.value.input))
        << " drivers)\n";
  } else {
    out << "wired " << support::pin_text(nl, from.value.slot, true, 0, from.value.negated) << " -> "
        << support::pin_text(nl, to.value.slot, false, to.value.input) << '\n';
  }
  // A freshly driven RST/SET acts at once (report forced flops below), as
  // do level latches. An illegal SR net (S=R=1 through the new wire)
  // fails here: the wire is reverted so the refused net changes no state.
  if (!support::apply_async_and_report(nl, out).ok()) {
    if (shared_append) {
      (void)wengine::logic::unwire_driver(nl, from.value.slot, to.value.slot, to.value.input);
    } else {
      (void)wengine::logic::unwire_pin(nl, to.value);
    }
    return cli::usage_error(out, "wire: S=1,R=1 is illegal (SR latch holds state, wire reverted)", kUsage);
  }
  return core::kExitOk;
}

void LogicWireSpec::complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out) {
  if (!partial.empty() && partial.front() == '-') {
    constexpr std::string_view kOpts[] = {"--help", "-h", "--shared"};
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
