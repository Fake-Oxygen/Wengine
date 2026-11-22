#pragma once

/**
 * @file
 * Sequential-net stepped solver: advances one fixed-step edge over the
 * singleton sequential pool in world::State (netlist + clocked flop
 * state + stimulus). Owns the per-step clocked-logic mutation only —
 * depends on core/world/logic (units, math downstream), never CLI types.
 * Registered in solver_registry.hpp; consumed by `sim run --solver
 * logic`, the netlist/seq drivers, and tests.
 */

// Sequential-net stepped solver: advances one fixed-step edge over the
// singleton sequential pool in world::State (see research R4).
//
// End-of-step convention: the Q values written during a step are value(T)
// where T is the snapshot time the step is about to stamp
// (`state.sim_time + dt`), matching the signal solver's T convention.
// Unselected flops (subset stepping) hold their Q while combinational
// outputs still settle around them. Level latches re-settle every step;
// illegal S=R=1 fails loudly with nothing committed.
//
// Single call per step, no allocation, noexcept. dt advances the clock but
// never gates edges (zero-delay semantics).

#include "wengine/core/solver.hpp"
#include "wengine/logic/seq_step.hpp"
#include "wengine/world/state.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace wengine::solvers {

/**
 * @brief Stepped solver for sequential logic nets: edge-triggered flops
 *        plus level-sensitive latches, one fixed step at a time.
 *
 * Mutates the singleton sequential pool in world::State per the solver
 * contract: validate dt, advance only the selected live edge flops
 * (members), re-settle combinational outputs and latches around them,
 * and skip everything detached.
 *
 * @note Determinism: the step is a pure function of the pool's current
 *       contents — single call, no allocation, noexcept, fixed slot
 *       order. End-of-step convention: Q values written during a step
 *       are value(T) where T is the snapshot time the step is about to
 *       stamp (state.sim_time + dt), matching the signal solver. dt
 *       advances the clock but never gates edges (zero-delay
 *       semantics). Unselected flops hold their Q; level latches
 *       re-settle every step; an illegal S=R=1 latch fails the step
 *       loudly with nothing committed.
 */
class SequentialSolver : public core::ISolver {
 public:
  /** @brief Registry key: the solver named by `sim run --solver logic`. */
  static constexpr std::string_view kName = "logic";
  /** @brief One-line listing text shown by the CLI and GUI. */
  static constexpr std::string_view kDescription = "Sequential logic nets: edge-triggered flops + level latches (fixed-step)";

  /** @brief Registry factory. @return A default-configured instance. */
  static std::unique_ptr<core::ISolver> create() {
    return std::make_unique<SequentialSolver>();
  }

  /** @brief Registry identity. @return kName. */
  [[nodiscard]] std::string_view name() const noexcept override { return kName; }

  /**
   * @brief Advance the netlist by one fixed step.
   *
   * @param[in,out] s world state; s.seq (clocked flop state), s.seq_outputs
   *                  (settled block outputs), and s.sim_time bookkeeping
   *                  are mutated per the contract above.
   * @param[in] dt fixed timestep, in seconds (must be > 0 and finite).
   * @return kOk on success; kInvalidArgument when dt is non-positive or
   *         non-finite, or when the edge step itself rejects the state
   *         (e.g. illegal latch inputs) — the pool is left unchanged on
   *         any refusal.
   */
  core::Status step(world::State &s, double dt) noexcept override {
    if (!(dt > 0.0) || !std::isfinite(dt)) {
      return {core::StatusCode::kInvalidArgument};
    }
    const logic::Slot *members = s.seq_members.empty() ? nullptr : s.seq_members.data();
    const std::size_t n_members = s.seq_members.size();
    std::span<const logic::LogicValue> stimulus(s.seq_inputs.data(), s.seq_inputs.size());
    std::span<logic::LogicValue> outputs(s.seq_outputs.data(), s.seq_outputs.size());
    const core::Status st = logic::seq_step_filtered(s.netlist, s.seq, stimulus, outputs, members, n_members);
    if (!st.ok()) {
      return {core::StatusCode::kInvalidArgument};
    }
    return core::Status::success();
  }
};

} // namespace wengine::solvers
