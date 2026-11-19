#pragma once

/**
 * @file
 * The stepped-solver interface driven by core::Runner: one step() call
 * advances one physical domain over the fixed timestep by mutating
 * world::State in place. Implementations are registered in
 * core/solver_registry.hpp and reached through the CLI/GUI/headless run
 * loops. This seam must stay free of I/O, allocation-heavy work, and
 * frontend types — solvers report failure exclusively through
 * core::Status, never exceptions.
 */

#include "wengine/core/status.hpp"
#include "wengine/world/state.hpp"

#include <string_view>

namespace wengine::core {

/**
 * @brief Stepped solver for fixed-step simulation.
 *
 * @note Contract: implementations mutate world::State in place with no
 *       allocation on the hot path, perform no I/O, validate dt, skip
 *       detached/inactive slots, and report failures via Status (no
 *       exceptions, no strings). Safe to call from CLI, GUI, or
 *       headless emit loops; mutation order within a step is the
 *       solver's determinism contract.
 */
// Stepped solver for fixed-step simulation. Implementations mutate
// `world::State` in place (no allocation in the hot path), perform no I/O,
// and report failures via Status (no exceptions, no strings).
// Safe to call from CLI, GUI, or headless emit loops.
class ISolver {
public:
  /**
   * @brief Virtual destructor: solvers are owned via unique_ptr from
   *        their static create() factory and destroyed polymorphically.
   */
  virtual ~ISolver() = default;
  /**
   * @brief Stable registry/display name of the solver.
   *
   * @return string_view of the implementation's static kName; the view
   *         points at static storage and needs no lifetime management.
   */
  [[nodiscard]] virtual std::string_view name() const noexcept = 0;
  /**
   * @brief Advance the solver's domain exactly one fixed step.
   *
   * @param[in, out] state live world state; mutated in place per the
   *                 solver contract (pools updated in slot order, no
   *                 reallocation, no I/O).
   * @param[in] dt_seconds fixed step size in seconds (_s), the same
   *            value for every registered solver in the step.
   * @return kOk when the step applied; a failing Status (for example
   *         kInvalidArgument when dt is non-positive or non-finite)
   *         aborts the whole run step — the caller (core::Runner)
   *         latches it; no exception is ever thrown.
   */
  virtual Status step(world::State &state, double dt_seconds) noexcept = 0;
};

} // namespace wengine::core
