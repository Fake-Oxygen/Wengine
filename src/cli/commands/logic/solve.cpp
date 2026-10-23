#include "common.hpp"

#include "wengine/cli/logic_registry.hpp"
#include "wengine/cli/status.hpp"
#include "wengine/core/status.hpp"
#include "wengine/logic/eval.hpp"
#include "wengine/logic/netlist.hpp"

#include <cstddef>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wengine::cli::logic {

using wengine::logic::BlockPin;
using wengine::logic::LogicValue;
using wengine::logic::Netlist;

int LogicSolveSpec::run(ArgCursor &args, std::ostream &out) {
  if (args.take_flag({"--help", "-h"})) {
    out << kName << " - " << kDescription << "\nUsage:\n" << kUsage << '\n';
    return core::kExitOk;
  }
  std::optional<std::string_view> in_text = args.take_option({"--in"});
  std::vector<std::string_view> from_vals;
  while (auto f = args.take_option({"--from"})) {
    from_vals.push_back(*f);
  }
  if (args.has_remaining()) {
    return cli::unexpected_error(out, *args.first_remaining(), kUsage);
  }
  Netlist &nl = workspace();
  std::vector<BlockPin> in_pins;
  std::vector<BlockPin> out_pins;
  support::interface_pins(nl, in_pins, out_pins);
  const std::size_t nin = in_pins.size();
  const std::size_t nout = out_pins.size();
  std::vector<LogicValue> values(nin, LogicValue::kLow);
  std::vector<bool> from_set(nin, false);
  bool any_default = false;
  if (in_text) {
    if (in_text->size() != nin) {
      return cli::usage_error(out, "solve --in length must match the block input count", kUsage);
    }
    for (std::size_t j = 0; j < nin; ++j) {
      const char c = (*in_text)[j];
      if (c != '0' && c != '1') {
        return cli::usage_error(out, "solve --in takes 0/1 bits only", kUsage);
      }
      values[j] = (c == '1') ? LogicValue::kHigh : LogicValue::kLow;
    }
  }
  for (std::string_view fv : from_vals) {
    const std::size_t eq = fv.find('=');
    if (eq == std::string_view::npos) {
      return cli::usage_error(out, "solve --from takes PIN=0|1 (e.g. --from AND1.1=1)", kUsage);
    }
    const std::string_view pin_text_arg = fv.substr(0, eq);
    const std::string_view bit = fv.substr(eq + 1);
    if (bit != "0" && bit != "1") {
      return cli::usage_error(out, "solve --from takes PIN=0|1 (e.g. --from AND1.1=1)", kUsage);
    }
    const auto pin = wengine::logic::parse_pin(nl, pin_text_arg);
    if (!pin.ok() || pin.value.is_output) {
      return cli::usage_error(out, std::string("solve: bad block input pin '") + std::string(pin_text_arg) + "'", kUsage);
    }
    // Tied members resolve to their class leader (one column per class).
    std::uint16_t eff_slot = pin.value.slot;
    std::uint8_t eff_input = pin.value.input;
    {
      const std::size_t flat =
          static_cast<std::size_t>(pin.value.slot) * wengine::logic::kMaxInputs + pin.value.input;
      const std::size_t leader = wengine::logic::tie_leader_flat(nl, flat);
      eff_slot = static_cast<std::uint16_t>(leader / wengine::logic::kMaxInputs);
      eff_input = static_cast<std::uint8_t>(leader % wengine::logic::kMaxInputs);
    }
    std::size_t pos = nin;
    for (std::size_t j = 0; j < nin; ++j) {
      if (in_pins[j].slot == eff_slot && in_pins[j].input == eff_input) {
        pos = j;
        break;
      }
    }
    if (pos == nin) {
      return cli::usage_error(out, std::string("solve: '") + std::string(pin_text_arg) + "' is not a block input (wired or ignored?)",
                              kUsage);
    }
    if (from_set[pos]) {
      return cli::usage_error(out, std::string("solve: duplicate --from for '") + std::string(pin_text_arg) + "'", kUsage);
    }
    from_set[pos] = true;
    values[pos] = (bit == "1") ? LogicValue::kHigh : LogicValue::kLow;
  }
  {
    std::vector<bool> set_by_user(nin, false);
    if (in_text) {
      for (std::size_t j = 0; j < nin; ++j) {
        set_by_user[j] = true;
      }
    }
    for (std::size_t j = 0; j < nin; ++j) {
      set_by_user[j] = set_by_user[j] || from_set[j];
    }
    for (bool s : set_by_user) {
      if (!s) {
        any_default = true;
        break;
      }
    }
  }
  std::vector<LogicValue> outputs(nout == 0 ? 1 : nout, LogicValue::kLow);
  const core::Status st =
      wengine::logic::solve(nl, std::span<const LogicValue>{values.data(), nin}, std::span<LogicValue>{outputs.data(), nout});
  if (!st.ok()) {
    return cli::runtime_error(out, "solve failed");
  }
  if (nin == 0) {
    out << "in: (none)\n";
  } else {
    std::string bits;
    bits.reserve(nin);
    for (LogicValue v : values) {
      bits += (v == LogicValue::kHigh) ? '1' : '0';
    }
    out << "in: " << bits << " (";
    for (std::size_t j = 0; j < nin; ++j) {
      if (j != 0) {
        out << ' ';
      }
      out << support::pin_text(nl, in_pins[j].slot, false, in_pins[j].input) << '=' << bits[j];
    }
    out << ')';
    if (any_default) {
      out << " (defaults assumed)";
    }
    out << '\n';
  }
  for (std::size_t o = 0; o < nout; ++o) {
    out << support::pin_text(nl, out_pins[o].slot, true, 0) << '=' << ((outputs[o] == LogicValue::kHigh) ? '1' : '0') << '\n';
  }
  return core::kExitOk;
}

void LogicSolveSpec::complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out) {
  constexpr std::string_view kFromEq = "--from=";
  constexpr std::string_view kInEq = "--in=";
  if (support::starts_with(partial, kFromEq)) {
    support::complete_from_value(workspace(), partial.substr(kFromEq.size()), out);
    return;
  }
  if (support::starts_with(partial, kInEq)) {
    return; // bits: nothing to complete
  }
  if (!partial.empty() && partial.front() == '-') {
    constexpr std::string_view kOpts[] = {"--in", "--from", "--help", "-h"};
    support::push_matches(partial, kOpts, out);
    return;
  }
  if (!prior_args.empty() && prior_args.back() == "--in") {
    return; // bits value
  }
  if (!prior_args.empty() && prior_args.back() == "--from") {
    support::complete_from_value(workspace(), partial, out);
  }
}

} // namespace wengine::cli::logic
