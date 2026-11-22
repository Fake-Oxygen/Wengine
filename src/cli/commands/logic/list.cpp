#include "common.hpp"

#include "wengine/cli/logic_registry.hpp"
#include "wengine/cli/status.hpp"
#include "wengine/core/status.hpp"
#include "wengine/logic/netlist.hpp"
#include "wengine/logic/shared_net.hpp"

#include <cstdint>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wengine::cli::logic {

using wengine::logic::PinRef;
using wengine::logic::Netlist;
using wengine::logic::Slot;
using wengine::logic::slot_count;

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
  for (Slot s = 0; s < slot_count(nl); ++s) {
    const auto &g = nl.slots[s];
    if (!g.live) {
      continue;
    }
    out << "  " << support::gate_name(nl, s) << ' ' << wengine::logic::gate_kind_name(g.kind) << " inputs="
        << static_cast<unsigned>(g.input_count) << " fanout=" << g.output_fanout;
    if (wengine::logic::is_sequential_kind(g.kind)) {
      out << " nqfanout=" << g.nq_fanout;
    }
    // Propagation-delay annotation (013): canonical picoseconds, present
    // only — un-annotated workspaces list byte-identically.
    if (g.propagation_delay_ps != 0) {
      out << " delay=" << g.propagation_delay_ps << "ps";
    }
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
    // Shared-net marks (011): per marked input, the pin and attachment
    // count. Absent on unmarked netlists (output byte-identical).
    bool first_shared = true;
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      if (!wengine::logic::is_shared_input(nl, s, i)) {
        continue;
      }
      if (first_shared) {
        out << " shared=";
        first_shared = false;
      } else {
        out << ',';
      }
      out << support::pin_text(nl, s, false, i) << '(' << static_cast<unsigned>(wengine::logic::shared_attachment_count(nl, s, i)) << ')';
    }
    // Pull settings (012): per pulled input, the pin and level. Absent
    // on pull-free netlists (output byte-identical).
    bool first_pull = true;
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      const std::uint8_t pull_level = wengine::logic::pull_level_flat(
          nl, static_cast<std::size_t>(s) * wengine::logic::kMaxInputs + i);
      if (pull_level == 0) {
        continue;
      }
      if (first_pull) {
        out << " pulls=";
        first_pull = false;
      } else {
        out << ',';
      }
      out << support::pin_text(nl, s, false, i) << '(' << (pull_level == 1 ? "up" : "down") << ')';
    }
    out << '\n';
  }
  if (show_wires) {
    const std::size_t nw = support::wire_count(nl);
    out << "wires (" << nw << "):\n";
    if (nw == 0) {
      out << "  (none)\n";
    }
    for (Slot s = 0; s < slot_count(nl); ++s) {
      const auto &g = nl.slots[s];
      if (!g.live) {
        continue;
      }
      for (std::uint8_t i = 0; i < g.input_count; ++i) {
        const Slot drv = g.drivers[i];
        if (drv == wengine::logic::kNoDriver) {
          continue;
        }
        const bool neg = nl.driver_inv[static_cast<std::size_t>(s) * wengine::logic::kMaxInputs + i] != 0;
        out << "  " << support::pin_text(nl, drv, true, 0, neg) << " -> " << support::pin_text(nl, s, false, i) << '\n';
        // Shared extras (011): every attachment is a wire row, in compact
        // index order. No extras on unmarked netlists (loop no-op).
        const std::size_t flat = static_cast<std::size_t>(s) * wengine::logic::kMaxInputs + i;
        const std::uint8_t extra_n = wengine::logic::shared_extra_count(nl, s, i);
        for (std::uint8_t j = 0; j < extra_n; ++j) {
          const Slot edrv = wengine::logic::shared_extra_driver(nl, flat, j);
          const bool eneg = wengine::logic::shared_extra_inv(nl, flat, j) != 0;
          out << "  " << support::pin_text(nl, edrv, true, 0, eneg) << " -> " << support::pin_text(nl, s, false, i) << '\n';
        }
      }
    }
  }
  if (show_iface) {
    std::vector<PinRef> in_pins;
    std::vector<PinRef> out_pins;
    support::interface_pins(nl, in_pins, out_pins);
    out << "inputs (" << in_pins.size() << "):\n";
    if (in_pins.empty()) {
      out << "  (none)\n";
    }
    for (const PinRef &p : in_pins) {
      out << "  " << support::pin_text(nl, p.slot, false, p.input) << '\n';
    }
    out << "outputs (" << out_pins.size() << "):\n";
    if (out_pins.empty()) {
      out << "  (none)\n";
    }
    for (const PinRef &p : out_pins) {
      out << "  " << support::pin_text(nl, p.slot, true, 0, p.negated) << '\n';
    }
    std::vector<std::string> ignored;
    for (Slot s = 0; s < slot_count(nl); ++s) {
      const auto &g = nl.slots[s];
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
      if (wengine::logic::is_sequential_kind(g.kind) && g.ignore_nq_output) {
        ignored.push_back(support::pin_text(nl, s, true, 0, true));
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
    for (Slot s = 0; s < slot_count(nl); ++s) {
      const auto &g = nl.slots[s];
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
        if (!wengine::logic::is_tied(nl, s, i)) {
          continue;
        }
        std::string line;
        for (Slot t = 0; t < slot_count(nl); ++t) {
          const auto &og = nl.slots[t];
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
