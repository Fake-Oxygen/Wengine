#pragma once

#include "wengine/core/status.hpp"
#include "wengine/world/state.hpp"

#include <string_view>

namespace wengine::core {

// Stepped solver for fixed-step simulation. Implementations mutate
// `world::State` in place (no allocation in the hot path), perform no I/O,
// and report failures via Status (no exceptions, no strings).
// Safe to call from CLI, GUI, or headless emit loops.
class ISolver {
public:
  virtual ~ISolver() = default;
  [[nodiscard]] virtual std::string_view name() const noexcept = 0;
  virtual Status step(world::State &state, double dt_seconds) noexcept = 0;
};

} // namespace wengine::core
