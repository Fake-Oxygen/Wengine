#pragma once

/**
 * @file
 * Toy stepped solver proving the engine backbone end to end (ISolver to
 * world::State particle pool to core::Runner to emit sinks): point
 * particles under constant gravity, semi-implicit Euler. Owns the
 * per-step particle mutation only — depends on core/world/units, never
 * CLI types; gravity scoping for all frontends lives in
 * configured_run.hpp. Registered in solver_registry.hpp and consumed by
 * `sim run` and tests/test_projectile.cpp.
 */

// Toy stepped solver: point particles under constant gravity, integrated
// with semi-implicit Euler (velocities first, then positions).
//   vy -= g*dt;  px += vx*dt;  py += vy*dt
// Exists to prove the engine backbone end to end: ISolver -> world::State
// particle pool -> core::Runner -> emit sinks. Holes (detached slots) are
// skipped. Closed form for constant g (n steps from v0, semi-implicit):
//   v(n) = v0 - n*g*dt
//   x(n) = x0 + n*v0*dt - g*dt^2*n*(n+1)/2
// which golden tests assert against. Pure stepping logic, no I/O.

#include "wengine/core/solver.hpp"

#include <cmath>
#include <cstdint>
#include <memory>
#include <string_view>

namespace wengine::solvers {

/**
 * @brief Stepped solver: 2-D point particles under constant gravity.
 *
 * Mutates world::State's particle pool in place per the solver contract:
 * validate dt, skip detached slots, integrate velocities first, then
 * positions (semi-implicit Euler).
 *
 * @note Determinism: slot iteration is ascending index order and each
 *       axis is one fused-multiply add per step, so runs are exactly
 *       reproducible. Constant-velocity axes (x under zero drag) stay
 *       exact; the integrated y axis accumulates error, and golden tests
 *       derive expectations per axis from the closed form in the banner
 *       above (v(n) = v0 - n*g*dt, x(n) = x0 + n*v0*dt - g*dt^2*n*(n+1)/2).
 */
class ProjectileSolver : public core::ISolver {
public:
  /** @brief Registry key: the solver named by `sim run --solver`. */
  static constexpr std::string_view kName = "projectile";
  /** @brief One-line listing text shown by the CLI and GUI. */
  static constexpr std::string_view kDescription =
      "Point particles under constant gravity (semi-implicit Euler)";

  /** @brief Registry factory. @return A default-configured (9.81 m/s^2) instance. */
  static std::unique_ptr<core::ISolver> create() {
    return std::make_unique<ProjectileSolver>();
  }

  /**
   * @brief Construct with a gravity setting, in m/s^2.
   * @param[in] gravity downward acceleration magnitude (default 9.81).
   */
  explicit ProjectileSolver(double gravity = 9.81) : gravity_(gravity) {}

  /** @brief Registry identity. @return kName. */
  [[nodiscard]] std::string_view name() const noexcept override { return kName; }
  /** @brief Current gravity setting. @return The gravity, in m/s^2. */
  [[nodiscard]] double gravity() const noexcept { return gravity_; }
  // Gravity must be finite; step() rejects a non-finite setting with
  // kInvalidArgument before mutating state (the configured-run builder
  // rejects it earlier for all frontends — see configured_run.hpp).
  /**
   * @brief Set gravity before handing the solver to the Runner.
   * @param[in] g downward acceleration, in m/s^2.
   */
  void set_gravity(double g) noexcept { gravity_ = g; }

  /**
   * @brief Advance every live particle by one fixed step.
   *
   * @param[in,out] s world state; live particle slots are mutated in
   *                  place, detached slots skipped.
   * @param[in] dt fixed timestep, in seconds (must be > 0 and finite).
   * @return kOk on success; kInvalidArgument when dt is non-positive or
   *         non-finite, or when the configured gravity is non-finite —
   *         in both cases state is left untouched.
   * @note Update order per particle is vel_y -= g*dt, then pos_x +=
   *       vx*dt, pos_y += vy*dt (semi-implicit Euler): the freshly
   *       updated velocity drives the position update.
   */
  core::Status step(world::State &s, double dt) noexcept override {
    if (!(dt > 0.0) || !std::isfinite(dt)) {
      return {core::StatusCode::kInvalidArgument};
    }
    if (!std::isfinite(gravity_)) {
      return {core::StatusCode::kInvalidArgument};
    }
    const double dv = gravity_ * dt;
    for (std::uint32_t i = 0; i < s.particles.capacity(); ++i) {
      if (!s.particles.has(i)) {
        continue;
      }
      s.particles.vel_y[i] -= dv;
      s.particles.pos_x[i] += s.particles.vel_x[i] * dt;
      s.particles.pos_y[i] += s.particles.vel_y[i] * dt;
    }
    return core::Status::success();
  }

private:
  double gravity_;
};

} // namespace wengine::solvers
