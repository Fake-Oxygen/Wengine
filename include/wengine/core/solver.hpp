#pragma once

#include "wengine/core/status.hpp"

namespace wengine::core {

// Solver interface for fixed-step simulation. Implementations must be
// stateless w.r.t. I/O (no iostream) and safe to call from CLI, GUI,
// or headless emit loops.
class ISolver {
public:
  virtual ~ISolver() = default;
  virtual Status step(double dt_seconds) noexcept = 0;
};

} // namespace wengine::core
