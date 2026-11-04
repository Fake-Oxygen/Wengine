#pragma once

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

class SequentialSolver : public core::ISolver {
 public:
  static constexpr std::string_view kName = "logic";
  static constexpr std::string_view kDescription = "Sequential logic nets: edge-triggered flops + level latches (fixed-step)";

  static std::unique_ptr<core::ISolver> create() {
    return std::make_unique<SequentialSolver>();
  }

  [[nodiscard]] std::string_view name() const noexcept override { return kName; }

  core::Status step(world::State &s, double dt) noexcept override {
    if (!(dt > 0.0) || !std::isfinite(dt)) {
      return {core::StatusCode::kInvalidArgument};
    }
    const std::uint16_t *members = s.seq_members.empty() ? nullptr : s.seq_members.data();
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
