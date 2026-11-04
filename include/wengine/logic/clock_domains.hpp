#pragma once

// Engine-owned clock-domain derivation (006-versioned-workspace-v1).
//
// Live edge-triggered flops (DFF/JK/T) grouped by clock-driver net, named
// by the source pin (e.g. "BUF1.Y", "DFF2.NQ" keeping the NQ spelling);
// flops with dangling clock pins form the implicit "clk" domain.
// First-seen (slot) order; members in slot order. Level-sensitive latches
// (DLATCH/SRLATCH) belong to no domain.
//
// Single implementation shared by the session `step` verb, `sim run`
// `--seq-clocks` lowering, completion, and the workspace loader's
// `clocks:` validation — the file pins what stepping uses, so derivation
// must exist exactly once. Header-only, session/file-path code (uses
// std::string/std::vector like yaml_store.hpp); never on hot paths.

#include "wengine/logic/netlist.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace wengine::logic {

struct ClockDomain {
  std::string name;
  std::vector<std::uint16_t> members;
};

// Derive clock domains from a netlist. CLK is input 1 (the same rule the
// session stepper and the engine member filter assume); a live driver
// names the domain by its source pin, dangling clock pins join "clk".
[[nodiscard]] inline std::vector<ClockDomain> derive_clock_domains(const Netlist &nl) {
  std::vector<ClockDomain> domains;
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    const Gate &g = nl.gates[s];
    if (!g.live) {
      continue;
    }
    if (g.kind != GateKind::kDff && g.kind != GateKind::kJk && g.kind != GateKind::kT) {
      continue; // level latches belong to no clock domain
    }
    std::string name = "clk";
    const std::uint16_t drv = (g.input_count > 1) ? g.drivers[1] : kNoDriver;
    if (drv != kNoDriver && drv < kMaxGates && nl.gates[drv].live) {
      const std::size_t flat = static_cast<std::size_t>(s) * kMaxInputs + 1;
      const bool neg = nl.driver_inv[flat] != 0;
      name = gate_kind_name(nl.gates[drv].kind);
      name += std::to_string(nl.gates[drv].counter);
      name += '.';
      if (is_sequential_kind(nl.gates[drv].kind)) {
        name += neg ? "NQ" : "Q";
      } else {
        name += 'Y';
      }
    }
    bool found = false;
    for (ClockDomain &d : domains) {
      if (d.name == name) {
        d.members.push_back(s);
        found = true;
        break;
      }
    }
    if (!found) {
      ClockDomain d;
      d.name = name;
      d.members.push_back(s);
      domains.push_back(std::move(d));
    }
  }
  return domains;
}

} // namespace wengine::logic
