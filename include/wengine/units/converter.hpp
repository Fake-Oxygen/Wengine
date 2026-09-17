#pragma once

#include "wengine/units/units.hpp"

namespace wengine::units {

// SI-prefix converter (milli, kilo, ...). Operates on raw doubles;
// dimension safety is enforced by Value<Dim> at call sites.
class UnitConverter {
public:
  UnitConverter() = delete;

  [[nodiscard]] static constexpr double to_base(double v, double factor) noexcept {
    return v * factor;
  }
  [[nodiscard]] static constexpr double from_base(double base_v,
                                                  double factor) noexcept {
    return base_v / factor;
  }
};

} // namespace wengine::units
