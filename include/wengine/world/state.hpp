#pragma once

#include <cstdint>
#include <vector>

namespace wengine::world {

// Central simulation state. Prefer SoA vectors here for cache/SIMD
// friendliness as solvers are added (particles, bodies, fields).
struct State {
  double sim_time{0.0};
  std::uint64_t step_index{0};

  // Example SoA placeholder (positions x/y). Real domains extend this.
  std::vector<double> pos_x;
  std::vector<double> pos_y;
};

// Immutable view handed to emit sinks / GUI.
struct Snapshot {
  double sim_time{0.0};
  std::uint64_t step_index{0};
};

[[nodiscard]] inline Snapshot take_snapshot(const State &s) noexcept {
  return {.sim_time = s.sim_time, .step_index = s.step_index};
}

} // namespace wengine::world
