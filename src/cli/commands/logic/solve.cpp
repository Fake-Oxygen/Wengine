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

using wengine::logic::PinRef;
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
  const bool with_delays = args.take_flag({support::kWithDelaysFlag});
  if (args.has_remaining()) {
    return cli::unexpected_error(out, *args.first_remaining(), kUsage);
  }
  Netlist &nl = workspace();
  std::vector<PinRef> in_pins;
  std::vector<PinRef> out_pins;
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
      const std::size_t flat = static_cast<std::size_t>(pin.value.slot) * wengine::logic::kMaxInputs + pin.value.input;
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
  // (011) The session contention channel observes every resolution;
  // the count increments before any consumer refusal below.
  const core::Status st = wengine::logic::solve(nl, std::span<const LogicValue>{values.data(), nin}, std::span<LogicValue>{outputs.data(), nout}, &support::solve_diag());
  if (!st.ok()) {
    support::report_contention(nl, support::solve_diag(), out);
    return cli::runtime_error(out, "solve failed");
  }
  // (014) Opt-in timing window: the settled values above are the
  // reported tail (parity by construction); the wheel below only
  // observes ordering. Nothing prints before both succeed, so a wheel
  // refusal leaves output untouched.
  wengine::logic::TimingTrace trace;
  if (with_delays) {
    wengine::logic::TimingScratch scratch{};
    wengine::logic::timing_build_sink_maps(nl, scratch);
    std::vector<LogicValue> zero(nin, LogicValue::kLow);
    if (!wengine::logic::timing_settle_initial(nl, std::span<const LogicValue>{zero.data(), nin}, scratch, wengine::logic::EdgeFilter::kAll).ok()) {
      return cli::runtime_error(out, "solve failed");
    }
    std::vector<wengine::logic::TimingSeed> seeds;
    for (std::size_t j = 0; j < nin; ++j) {
      if (values[j] == LogicValue::kHigh) {
        seeds.push_back(wengine::logic::TimingSeed{.slot = in_pins[j].slot,
                                                   .input = in_pins[j].input,
                                                   .is_flop = false,
                                                   .fire_ps = 0,
                                                   .new_value = LogicValue::kHigh});
      }
    }
    if (!wengine::logic::run_timing_window_inner(nl, seeds, trace, scratch, false).ok()) {
      return cli::runtime_error(out, std::string("solve: timing window refused (event budget ") + std::to_string(wengine::logic::kMaxTimingEvents) + " exceeded or picosecond time overflow)");
    }
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
  if (with_delays) {
    support::print_timing_trace(nl, trace, out);
  }
  for (std::size_t o = 0; o < nout; ++o) {
    const char bit = support::bit_of(outputs[o]);
    out << support::pin_text(nl, out_pins[o].slot, true, 0, out_pins[o].negated) << '=' << bit << '\n';
  }
  // (011) Contention readback: visible without watching net values.
  support::report_contention(nl, support::solve_diag(), out);
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
    constexpr std::string_view kOpts[] = {"--in", "--from", "--with-delays", "--help", "-h"};
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
