#include "common.hpp"

#include "wengine/cli/logic_registry.hpp"
#include "wengine/cli/status.hpp"
#include "wengine/core/status.hpp"
#include "wengine/logic/netlist.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wengine::cli::logic {

using wengine::logic::LogicValue;
using wengine::logic::Netlist;

namespace {

// Canonical display of a tie-leader flat index.
[[nodiscard]] std::string leader_text(const Netlist &nl, std::size_t leader_flat) {
  const auto slot = static_cast<std::uint16_t>(leader_flat / wengine::logic::kMaxInputs);
  const auto input = static_cast<std::uint8_t>(leader_flat % wengine::logic::kMaxInputs);
  return support::pin_text(nl, slot, false, input);
}

// Validate one PIN=value spelling. On success fills leader_flat + value
// (release == std::nullopt). Never mutates state.
[[nodiscard]] core::Status parse_entry(const Netlist &nl, std::string_view text, std::size_t &leader_flat,
                                      std::optional<LogicValue> &value, std::string &err) {
  const std::size_t eq = text.find('=');
  if (eq == std::string_view::npos) {
    err = std::string("poke: takes PIN=0|1|release (e.g. poke DFF1.D=1), got '") + std::string(text) + "'";
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  const std::string_view pin_text_arg = text.substr(0, eq);
  const std::string_view bit = text.substr(eq + 1);
  const auto pin = wengine::logic::parse_pin(nl, pin_text_arg);
  if (!pin.ok()) {
    err = std::string("poke: bad block input pin '") + std::string(pin_text_arg) + "'";
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  if (pin.value.is_output || nl.gates[pin.value.slot].drivers[pin.value.input] != wengine::logic::kNoDriver ||
      (nl.gates[pin.value.slot].ignored_mask & static_cast<std::uint8_t>(1u << pin.value.input)) != 0) {
    err = std::string("poke: '") + std::string(pin_text_arg) + "' is not a pokable block input (wired or ignored?)";
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  const std::size_t flat = static_cast<std::size_t>(pin.value.slot) * wengine::logic::kMaxInputs +
                           static_cast<std::size_t>(pin.value.input);
  leader_flat = wengine::logic::tie_leader_flat(nl, flat);
  if (bit == "0") {
    value = LogicValue::kLow;
  } else if (bit == "1") {
    value = LogicValue::kHigh;
  } else if (bit == "release") {
    value = std::nullopt;
  } else {
    err = std::string("poke: takes PIN=0|1|release (e.g. poke DFF1.D=1), got '") + std::string(text) + "'";
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  return core::Status::success();
}

} // namespace

int LogicPokeSpec::run(ArgCursor &args, std::ostream &out) {
  if (args.take_flag({"--help", "-h"})) {
    out << kName << " - " << kDescription << "\nUsage:\n" << kUsage << '\n';
    return core::kExitOk;
  }
  const bool clear = args.take_flag({"--clear"});
  std::vector<std::string_view> from_vals;
  while (auto f = args.take_option({"--from"})) {
    from_vals.push_back(*f);
  }
  std::vector<std::string_view> positionals;
  while (auto p = args.next_positional()) {
    positionals.push_back(*p);
  }
  if (args.has_remaining()) {
    return cli::unexpected_error(out, *args.first_remaining(), kUsage);
  }
  Netlist &nl = workspace();
  if (clear) {
    if (!positionals.empty() || !from_vals.empty()) {
      return cli::usage_error(out, "poke: --clear takes no pins", kUsage);
    }
    const std::size_t n = support::poke_map().size();
    support::poke_map().clear();
    out << "cleared " << n << (n == 1 ? " poke entry\n" : " poke entries\n");
    return core::kExitOk;
  }
  if (positionals.empty() && from_vals.empty()) {
    // List the map in slot order (std::map is flat-ordered == slot order).
    const std::size_t dropped = support::reap_stale_pokes(nl);
    if (dropped > 0) {
      out << "dropped " << dropped << (dropped == 1 ? " stale poke entry\n" : " stale poke entries\n");
    }
    if (support::poke_map().empty()) {
      out << "poke map: empty\n";
      return core::kExitOk;
    }
    for (const auto &kv : support::poke_map()) {
      out << leader_text(nl, kv.first) << '=' << (kv.second == LogicValue::kHigh ? '1' : '0') << '\n';
    }
    return core::kExitOk;
  }
  // Validate everything before mutating (errors change no state).
  struct Entry {
    std::size_t leader;
    std::optional<LogicValue> value;
  };
  std::vector<Entry> entries;
  std::string err;
  auto take_text = [&](std::string_view text) -> core::Status {
    std::size_t leader = 0;
    std::optional<LogicValue> value;
    if (!parse_entry(nl, text, leader, value, err).ok()) {
      return core::Status::failure(core::StatusCode::kInvalidArgument);
    }
    for (const Entry &e : entries) {
      if (e.leader == leader) {
        err = std::string("poke: duplicate value for '") + leader_text(nl, leader) + "'";
        return core::Status::failure(core::StatusCode::kInvalidArgument);
      }
    }
    entries.push_back(Entry{leader, value});
    return core::Status::success();
  };
  for (std::string_view t : positionals) {
    if (!take_text(t).ok()) {
      return cli::usage_error(out, err, kUsage);
    }
  }
  for (std::string_view t : from_vals) {
    if (!take_text(t).ok()) {
      return cli::usage_error(out, err, kUsage);
    }
  }
  auto &pokes = support::poke_map();
  for (const Entry &e : entries) {
    const std::string name = leader_text(nl, e.leader);
    const auto it = pokes.find(e.leader);
    if (!e.value.has_value()) {
      if (it != pokes.end()) {
        pokes.erase(it);
        out << "released " << name << '\n';
      } else {
        out << "released " << name << " (was not poked)\n";
      }
      continue;
    }
    if (it != pokes.end() && it->second == *e.value) {
      out << "poked " << name << '=' << (*e.value == LogicValue::kHigh ? '1' : '0') << " (leader " << name << ")\n";
      continue;
    }
    if (it != pokes.end()) {
      out << "updated " << name << ' ' << (it->second == LogicValue::kHigh ? '1' : '0') << "->"
          << (*e.value == LogicValue::kHigh ? '1' : '0') << '\n';
    } else {
      out << "poked " << name << '=' << (*e.value == LogicValue::kHigh ? '1' : '0') << " (leader " << name << ")\n";
    }
    pokes[e.leader] = *e.value;
  }
  return core::kExitOk;
}

void LogicPokeSpec::complete(std::span<const std::string_view> prior_args, std::string_view partial,
                             std::vector<std::string> &out) {
  if (support::starts_with(partial, "--")) {
    constexpr std::string_view kOpts[] = {"--from", "--from=", "--clear", "--help", "-h"};
    support::push_matches(partial, kOpts, out);
    return;
  }
  // Value completion after PIN=.
  const std::size_t eq = partial.find('=');
  if (eq != std::string_view::npos) {
    const std::string_view val = partial.substr(eq + 1);
    constexpr std::string_view kVals[] = {"0", "1", "release"};
    for (std::string_view v : kVals) {
      if (v.size() >= val.size() && v.substr(0, val.size()) == val) {
        out.emplace_back(std::string(partial.substr(0, eq + 1)) + std::string(v));
      }
    }
    return;
  }
  // After --from with a separate value token, complete the value too.
  if (!prior_args.empty() && prior_args.back() == "--from") {
    LogicPokeSpec::complete({}, partial, out);
    return;
  }
  // Bare prefix: valued forms for every pokable block input.
  const Netlist &nl = workspace();
  for (std::uint16_t s = 0; s < wengine::logic::kMaxGates; ++s) {
    const auto &g = nl.gates[s];
    if (!g.live) {
      continue;
    }
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      if (g.drivers[i] != wengine::logic::kNoDriver) {
        continue;
      }
      if ((g.ignored_mask & static_cast<std::uint8_t>(1u << i)) != 0) {
        continue;
      }
      const std::size_t flat = static_cast<std::size_t>(s) * wengine::logic::kMaxInputs + i;
      if (wengine::logic::tie_leader_flat(nl, flat) != flat) {
        continue;
      }
      const std::string pin = support::pin_text(nl, s, false, i);
      for (std::string_view v : {"=0", "=1", "=release"}) {
        const std::string cand = pin + std::string(v);
        if (cand.size() >= partial.size() && cand.substr(0, partial.size()) == partial) {
          out.emplace_back(cand);
        }
      }
    }
  }
}

} // namespace wengine::cli::logic
