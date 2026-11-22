#pragma once

/**
 * @file
 * Nested sequential-run section owned by the logic module and aggregated
 * by core::RunConfig (one member per solver): the state/edge vocabulary
 * the SequentialSolver driver validates and steps. Parsed values only
 * (frontends parse workspace/CLI text and store the result here); the
 * driver validates and spawns. set marks explicitly provided options so
 * the driver can reject them with a non-logic solver (same pattern as
 * SignalRunConfig::set). Domain names never enter the section: the CLI
 * maps names to flattened edge-flop slots and stores the slot list here;
 * the engine validates slots (live, edge kind DFF/JK/T, no duplicates)
 * and never derives names. step_all_domains=true requires empty
 * members; false requires non-empty.
 *
 * Dependency rules (docs/logic.md section 4): plain data plus vectors,
 * no engine behavior; depends only on netlist.hpp and types.hpp. Logic
 * is a standalone domain: this header must never depend on CLI types.
 * Consumers: core::RunConfig aggregation, the SequentialSolver driver,
 * the CLI `--seq-*` lowering, and the test_logic_seq_runner suite.
 */

#include "wengine/logic/netlist.hpp"
#include "wengine/logic/types.hpp"

#include <cstdint>
#include <vector>

namespace wengine::logic {

/**
 * @brief Nested sequential-run section: gate topology, stimulus,
 *        member slots, and power-on values.
 */
struct SequentialRunConfig {
  /** @brief Gate topology (plain data, copied). */
  Netlist netlist;                     // gate topology (plain data, copied)
  /** @brief Stimulus in block-interface order, Low/High only. */
  std::vector<LogicValue> stimulus;    // block-interface order, Low/High only
  /** @brief Flattened edge-flop slots to step (empty with step_all_domains). */
  std::vector<Slot> members;  // flattened edge-flop slots to step
  /** @brief True steps every live edge flop; false steps members only. */
  bool step_all_domains = true;        // true = every live edge flop steps
  /** @brief Explicitly provided (driver gate for non-logic solvers). */
  bool set = false;                    // explicitly provided (driver gate)
  /**
   * @brief Per-slot power-on values from the workspace bundle, applied
   *        to the pool right after init_seq_state, pre-first-step.
   *
   * All-Low default preserves the legacy fresh-attach behavior for
   * direct construction.
   */
  std::vector<LogicValue> init_q{};
};

} // namespace wengine::logic
