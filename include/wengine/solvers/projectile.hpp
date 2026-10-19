#pragma once

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

class ProjectileSolver : public core::ISolver {
public:
  static constexpr std::string_view kName = "projectile";
  static constexpr std::string_view kDescription =
      "Point particles under constant gravity (semi-implicit Euler)";

  static std::unique_ptr<core::ISolver> create() {
    return std::make_unique<ProjectileSolver>();
  }

  explicit ProjectileSolver(double gravity = 9.81) : gravity_(gravity) {}

  [[nodiscard]] std::string_view name() const noexcept override { return kName; }
  [[nodiscard]] double gravity() const noexcept { return gravity_; }
  // Gravity must be finite; step() rejects a non-finite setting with
  // kInvalidArgument before mutating state (the configured-run builder
  // rejects it earlier for all frontends — see configured_run.hpp).
  void set_gravity(double g) noexcept { gravity_ = g; }

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
