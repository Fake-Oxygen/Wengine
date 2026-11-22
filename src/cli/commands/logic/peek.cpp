#include "common.hpp"

#include "wengine/cli/logic_registry.hpp"
#include "wengine/cli/status.hpp"
#include "wengine/core/status.hpp"
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

namespace {

void print_flop(std::ostream &out, const Netlist &nl, wengine::logic::SeqState &st, wengine::logic::Slot slot) {
  const wengine::logic::LogicValue q = st.q[slot];
  const char qb = support::bit_of(q);
  const char nb = support::bit_of(q == wengine::logic::LogicValue::kHigh ? wengine::logic::LogicValue::kLow : wengine::logic::LogicValue::kHigh);
  out << support::pin_text(nl, slot, true, 0) << '=' << qb << " NQ=" << nb << " (serial " << st.serial[slot] << ")\n";
}

} // namespace

int LogicPeekSpec::run(ArgCursor &args, std::ostream &out) {
  if (args.take_flag({"--help", "-h"})) {
    out << kName << " - " << kDescription << "\nUsage:\n" << kUsage << '\n';
    return core::kExitOk;
  }
  std::optional<std::string_view> gate_text = args.next_positional();
  if (args.has_remaining()) {
    return cli::unexpected_error(out, *args.first_remaining(), kUsage);
  }
  Netlist &nl = workspace();
  wengine::logic::SeqState &st = support::seq_state();
  wengine::logic::init_seq_state(st, nl); // re-sync (idempotent, keeps serials)
  if (!gate_text) {
    std::size_t n = 0;
    for (wengine::logic::Slot s = 0; s < wengine::logic::slot_count(nl); ++s) {
      if (nl.slots[s].live && wengine::logic::is_sequential_kind(nl.slots[s].kind)) {
        print_flop(out, nl, st, s);
        ++n;
      }
    }
    if (n == 0) {
      out << "no flops\n";
      return core::kExitOk;
    }
    out << "flops: " << n << '\n';
    return core::kExitOk;
  }
  const auto gate = wengine::logic::resolve_gate(nl, *gate_text);
  if (!gate.ok()) {
    return cli::usage_error(out, std::string("peek: unknown gate '") + std::string(*gate_text) + "'", kUsage);
  }
  if (!wengine::logic::is_sequential_kind(nl.slots[gate.value].kind)) {
    return cli::usage_error(out, std::string("peek: '") + std::string(*gate_text) + "' is not a sequential gate", kUsage);
  }
  print_flop(out, nl, st, gate.value);
  out << "flops: 1\n";
  return core::kExitOk;
}

void LogicPeekSpec::complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out) {
  if (!prior_args.empty()) {
    return; // single optional positional
  }
  if (!partial.empty() && partial.front() == '-') {
    constexpr std::string_view kOpts[] = {"--help", "-h"};
    support::push_matches(partial, kOpts, out);
    return;
  }
  // Live sequential gate names only.
  const Netlist &nl = workspace();
  for (wengine::logic::Slot s = 0; s < wengine::logic::slot_count(nl); ++s) {
    if (!nl.slots[s].live || !wengine::logic::is_sequential_kind(nl.slots[s].kind)) {
      continue;
    }
    const std::string name = support::gate_name(nl, s);
    if (name.size() >= partial.size() && name.substr(0, partial.size()) == partial) {
      out.emplace_back(name);
    }
  }
}

} // namespace wengine::cli::logic
