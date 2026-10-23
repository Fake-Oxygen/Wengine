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

using wengine::logic::BlockPin;
using wengine::logic::kMaxGates;
using wengine::logic::Netlist;

int LogicListSpec::run(ArgCursor &args, std::ostream &out) {
  if (args.take_flag({"--help", "-h"})) {
    out << kName << " - " << kDescription << "\nUsage:\n" << kUsage << '\n';
    return core::kExitOk;
  }
  const bool want_wires = args.take_flag({"--wires"});
  const bool want_iface = args.take_flag({"--iface"});
  if (args.has_remaining()) {
    return cli::unexpected_error(out, *args.first_remaining(), kUsage);
  }
  const bool show_wires = (!want_wires && !want_iface) || want_wires;
  const bool show_iface = (!want_wires && !want_iface) || want_iface;
  const Netlist &nl = workspace();
  out << "gates (" << nl.live_count << "):\n";
  if (nl.live_count == 0) {
    out << "  (none)\n";
  }
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    const auto &g = nl.gates[s];
    if (!g.live) {
      continue;
    }
    out << "  " << support::gate_name(nl, s) << ' ' << wengine::logic::gate_kind_name(g.kind) << " inputs="
        << static_cast<unsigned>(g.input_count) << " fanout=" << g.output_fanout;
    bool first_ign = true;
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      if ((g.ignored_mask & static_cast<std::uint8_t>(1u << i)) == 0) {
        continue;
      }
      if (first_ign) {
        out << " ignored=";
        first_ign = false;
      } else {
        out << ',';
      }
      out << (static_cast<unsigned>(i) + 1u);
    }
    if (g.ignore_output) {
      out << " ignore-output";
    }
    out << '\n';
  }
  if (show_wires) {
    const std::size_t nw = support::wire_count(nl);
    out << "wires (" << nw << "):\n";
    if (nw == 0) {
      out << "  (none)\n";
    }
    for (std::uint16_t s = 0; s < kMaxGates; ++s) {
      const auto &g = nl.gates[s];
      if (!g.live) {
        continue;
      }
      for (std::uint8_t i = 0; i < g.input_count; ++i) {
        const std::uint16_t drv = g.drivers[i];
        if (drv == wengine::logic::kNoDriver) {
          continue;
        }
        out << "  " << support::pin_text(nl, drv, true, 0) << " -> " << support::pin_text(nl, s, false, i) << '\n';
      }
    }
  }
  if (show_iface) {
    std::vector<BlockPin> in_pins;
    std::vector<BlockPin> out_pins;
    support::interface_pins(nl, in_pins, out_pins);
    out << "inputs (" << in_pins.size() << "):\n";
    if (in_pins.empty()) {
      out << "  (none)\n";
    }
    for (const BlockPin &p : in_pins) {
      out << "  " << support::pin_text(nl, p.slot, false, p.input) << '\n';
    }
    out << "outputs (" << out_pins.size() << "):\n";
    if (out_pins.empty()) {
      out << "  (none)\n";
    }
    for (const BlockPin &p : out_pins) {
      out << "  " << support::pin_text(nl, p.slot, true, 0) << '\n';
    }
    std::vector<std::string> ignored;
    for (std::uint16_t s = 0; s < kMaxGates; ++s) {
      const auto &g = nl.gates[s];
      if (!g.live) {
        continue;
      }
      for (std::uint8_t i = 0; i < g.input_count; ++i) {
        if ((g.ignored_mask & static_cast<std::uint8_t>(1u << i)) != 0) {
          ignored.push_back(support::pin_text(nl, s, false, i));
        }
      }
      if (g.ignore_output) {
        ignored.push_back(support::pin_text(nl, s, true, 0));
      }
    }
    out << "ignored (" << ignored.size() << "):\n";
    if (ignored.empty()) {
      out << "  (none)\n";
    }
    for (const std::string &p : ignored) {
      out << "  " << p << '\n';
    }
    // Tie groups: one line per class (>= 2 members), members in slot
    // order joined by " = " (canonical leader first by construction).
    std::vector<std::string> tie_lines;
    for (std::uint16_t s = 0; s < kMaxGates; ++s) {
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
        if (nl.tie_rep[flat] == 0) {
          continue;
        }
        std::string line;
        for (std::uint16_t t = 0; t < kMaxGates; ++t) {
          const auto &og = nl.gates[t];
          if (!og.live) {
            continue;
          }
          for (std::uint8_t j = 0; j < og.input_count; ++j) {
            const std::size_t f = static_cast<std::size_t>(t) * wengine::logic::kMaxInputs + j;
            if (wengine::logic::tie_leader_flat(nl, f) != flat) {
              continue;
            }
            if (!line.empty()) {
              line += " = ";
            }
            line += support::pin_text(nl, t, false, j);
          }
        }
        tie_lines.push_back(line);
      }
    }
    out << "ties (" << tie_lines.size() << "):\n";
    if (tie_lines.empty()) {
      out << "  (none)\n";
    }
    for (const std::string &line : tie_lines) {
      out << "  " << line << '\n';
    }
  }
  return core::kExitOk;
}

void LogicListSpec::complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out) {
  (void)prior_args;
  if (partial.empty() || partial.front() != '-') {
    return; // no positionals
  }
  constexpr std::string_view kOpts[] = {"--wires", "--iface", "--help", "-h"};
  support::push_matches(partial, kOpts, out);
}

} // namespace wengine::cli::logic
