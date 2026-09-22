#pragma once

#include "wengine/units/units.hpp"

namespace wengine::solvers {

struct ResistorState {
  units::Value<units::ElectricPotential> voltage{0.0};
  units::Value<units::ElectricCurrent> current{0.0};
  units::Value<units::ElectricResistance> resistance{0.0};
  units::Value<units::Power> power{0.0};
};

class OhmLawSolver {
public:
  OhmLawSolver() = delete;

  [[nodiscard]] static constexpr ResistorState solve_from_ui(
      units::Value<units::ElectricPotential> u,
      units::Value<units::ElectricCurrent> i) noexcept {
    return {u, i, u / i, u * i};
  }

  [[nodiscard]] static constexpr ResistorState solve_from_ir(
      units::Value<units::ElectricCurrent> i,
      units::Value<units::ElectricResistance> r) noexcept {
    const units::Value<units::ElectricPotential> u = i * r;
    return {u, i, r, u * i};
  }

  [[nodiscard]] static constexpr ResistorState solve_from_ur(
      units::Value<units::ElectricPotential> u,
      units::Value<units::ElectricResistance> r) noexcept {
    const units::Value<units::ElectricCurrent> i = u / r;
    return {u, i, r, u * i};
  }
};

} // namespace wengine::solvers
