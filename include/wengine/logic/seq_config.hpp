#pragma once

// Nested sequential-run section owned by the logic module and aggregated by
// core::RunConfig (one member per solver — see core/run_config.hpp).
//
// Parsed values only (frontends parse workspace/CLI text and store the
// result here); the driver validates + spawns. `set` marks explicitly
// provided options so the driver can reject them with a non-logic solver
// (same pattern as SignalRunConfig::set). Domain names never enter the
// section: the CLI maps names to flattened edge-flop slots and stores the
// slot list here; the engine validates slots (live, edge kind DFF/JK/T,
// no duplicates) and never derives names. `step_all_domains=true` requires
// empty `members`; `false` requires non-empty.

#include "wengine/logic/netlist.hpp"
#include "wengine/logic/types.hpp"

#include <cstdint>
#include <vector>

namespace wengine::logic {

struct SequentialRunConfig {
  Netlist netlist;                     // gate topology (plain data, copied)
  std::vector<LogicValue> stimulus;    // block-interface order, Low/High only
  std::vector<std::uint16_t> members;  // flattened edge-flop slots to step
  bool step_all_domains = true;        // true = every live edge flop steps
  bool set = false;                    // explicitly provided (driver gate)
};

} // namespace wengine::logic
