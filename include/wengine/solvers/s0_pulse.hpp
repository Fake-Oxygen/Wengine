#pragma once

namespace wengine::solvers {

// Placeholder for S0 pulse solver. Implement step() against world::State;
// keep header-only + constexpr-friendly for perf.
class S0PulseSolver {
public:
  S0PulseSolver() = delete;
};

} // namespace wengine::solvers
