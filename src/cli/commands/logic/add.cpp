#include "common.hpp"

#include "wengine/cli/logic_registry.hpp"
#include "wengine/cli/status.hpp"
#include "wengine/core/status.hpp"
#include "wengine/logic/netlist.hpp"
#include "wengine/logic/types.hpp"

#include <cstdint>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wengine::cli::logic {

using wengine::logic::GateKind;
using wengine::logic::kMaxInputs;
using wengine::logic::Netlist;

int LogicAddSpec::run(ArgCursor &args, std::ostream &out) {
  if (args.take_flag({"--help", "-h"})) {
    out << kName << " - " << kDescription << "\nUsage:\n" << kUsage << '\n';
    return core::kExitOk;
  }
  std::optional<std::string_view> inputs_text = args.take_option({"--inputs"});
  std::optional<std::string_view> kind_text = args.next_positional();
  if (!kind_text) {
    return cli::usage_error(out, "add needs a gate kind (AND|OR|NAND|NOR|XOR|XNOR|NOT|BUF)", kUsage);
  }
  if (args.has_remaining()) {
    return cli::unexpected_error(out, *args.first_remaining(), kUsage);
  }
  const auto kind = wengine::logic::parse_gate_kind(*kind_text);
  if (!kind.ok()) {
    return cli::usage_error(out, "bad gate kind (want AND|OR|NAND|NOR|XOR|XNOR|NOT|BUF)", kUsage);
  }
  std::uint8_t inputs = wengine::logic::default_fanin(kind.value);
  if (inputs_text) {
    const auto n = cli::parse_i64(*inputs_text);
    if (!n.ok() || n.value < 1 || n.value > static_cast<long long>(kMaxInputs)) {
      return cli::usage_error(out, "bad input count (want 1..8)", kUsage);
    }
    inputs = static_cast<std::uint8_t>(n.value);
  }
  Netlist &nl = workspace();
  const auto added = wengine::logic::add_gate(nl, kind.value, inputs);
  if (!added.ok()) {
    if (added.error == std::string_view("bad fan-in for kind")) {
      return cli::usage_error(out, "bad fan-in for kind (NOT/BUF take 1, the rest 2..8)", kUsage);
    }
    return cli::runtime_error(out, "netlist full (max 1024 gates)");
  }
  out << "added " << support::gate_name(nl, added.value) << " kind=" << wengine::logic::gate_kind_name(kind.value)
      << " inputs=" << static_cast<unsigned>(inputs) << '\n';
  return core::kExitOk;
}

void LogicAddSpec::complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out) {
  constexpr std::string_view kInputsEq = "--inputs=";
  if (support::starts_with(partial, kInputsEq)) {
    return; // numeric value: nothing to complete
  }
  if (!partial.empty() && partial.front() == '-') {
    constexpr std::string_view kOpts[] = {"--inputs", "--help", "-h"};
    support::push_matches(partial, kOpts, out);
    return;
  }
  if (!prior_args.empty() && prior_args.back() == "--inputs") {
    return; // numeric value
  }
  bool have_kind = false;
  for (std::size_t i = 0; i < prior_args.size(); ++i) {
    const std::string_view t = prior_args[i];
    if (t == "--inputs") {
      ++i; // skip its value
      continue;
    }
    if (t == "--help" || t == "-h" || support::starts_with(t, kInputsEq)) {
      continue;
    }
    if (!t.empty() && t.front() == '-') {
      continue; // unknown flag: run reports it; kind completion still valid
    }
    have_kind = true;
    break;
  }
  if (have_kind) {
    return; // kind given: only --inputs may follow
  }
  for (std::uint8_t o = 0; o < 8; ++o) {
    const std::string_view k = wengine::logic::gate_kind_name(static_cast<GateKind>(o));
    if (k.substr(0, partial.size()) == partial) {
      out.emplace_back(k);
    }
  }
}

} // namespace wengine::cli::logic
