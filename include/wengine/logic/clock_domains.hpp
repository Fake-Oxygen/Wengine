#pragma once

/**
 * @file
 * Engine-owned clock-domain derivation: the grouping stage that turns
 * one netlist into named stepping subsets. Live edge-triggered flops
 * (DFF/JK/T) are grouped by clock-driver net, named by the source pin
 * (e.g. "BUF1.Y", "DFF2.NQ" keeping the NQ spelling); flops with
 * dangling clock pins form the implicit "clk" domain. First-seen (slot)
 * order; members in slot order. Level-sensitive latches (DLATCH/SRLATCH)
 * belong to no domain.
 *
 * Single implementation shared by the session `step` verb, `sim run`
 * `--seq-clocks` lowering, completion, and the workspace loader's
 * `clocks:` validation — the file pins what stepping uses, so
 * derivation must exist exactly once. Header-only; session/file-path
 * code (uses string and vector like yaml_store.hpp); never on hot
 * paths.
 *
 * Dependency rules (docs/logic.md section 4): depends only on
 * netlist.hpp. Logic is a standalone domain: this header must never
 * depend on CLI types. Consumers: the CLI step verb, sim run
 * --seq-clocks lowering, completion, the yaml_store loader's clocks
 * validation, and the test_logic* suite.
 */

#include "wengine/logic/netlist.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace wengine::logic {

/**
 * @brief One clock domain: live edge flops sharing a clock-driver net.
 */
struct ClockDomain {
  /** @brief Domain name: source-pin spelling, or bare "clk" for dangling clocks. */
  std::string name;
  /** @brief Member flop slots, in slot order. */
  std::vector<Slot> members;
};

/**
 * @brief Derive clock domains from a netlist: group live edge flops
 *        (DFF/JK/T) by clock-driver net.
 *
 * CLK is input 1 (the same rule the session stepper and the engine
 * member filter assume); a live driver names the domain by its source
 * pin, dangling clock pins join "clk".
 *
 * @param[in] nl Netlist to group (read-only).
 * @return Domains in first-seen slot order, members in slot order.
 *         Level latches belong to no domain.
 */
[[nodiscard]] inline std::vector<ClockDomain> derive_clock_domains(const Netlist &nl) {
  std::vector<ClockDomain> domains;
  for (Slot s = 0; s < slot_count(nl); ++s) {
    const Gate &g = nl.slots[s];
    if (!g.live) {
      continue;
    }
    if (g.kind != GateKind::kDff && g.kind != GateKind::kJk && g.kind != GateKind::kT) {
      continue; // level latches belong to no clock domain
    }
    std::string name = "clk";
    const Slot drv = (g.input_count > 1) ? g.drivers[1] : kNoDriver;
    if (drv != kNoDriver && drv < slot_count(nl) && nl.slots[drv].live) {
      const std::size_t flat = static_cast<std::size_t>(s) * kMaxInputs + 1;
      const bool neg = nl.driver_inv[flat] != 0;
      name = gate_kind_name(nl.slots[drv].kind);
      name += std::to_string(nl.slots[drv].counter);
      name += '.';
      if (is_sequential_kind(nl.slots[drv].kind)) {
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
