#pragma once

#include <cmath>

namespace wengine::math {

template <typename T>
struct Vector2D {
  T x{};
  T y{};

  constexpr Vector2D() noexcept = default;
  constexpr Vector2D(T x_in, T y_in) noexcept : x(x_in), y(y_in) {}

  [[nodiscard]] constexpr T length_squared() const noexcept { return x * x + y * y; }

  [[nodiscard]] double length() const noexcept {
    return std::sqrt(static_cast<double>(x * x + y * y));
  }

  [[nodiscard]] constexpr T dot(const Vector2D &rhs) const noexcept {
    return x * rhs.x + y * rhs.y;
  }
};

} // namespace wengine::math
