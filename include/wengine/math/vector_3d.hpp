#pragma once

#include <cmath>

namespace wengine::math {

template <typename T>
struct Vector3D {
  T x{};
  T y{};
  T z{};

  constexpr Vector3D() noexcept = default;
  constexpr Vector3D(T x_in, T y_in, T z_in) noexcept : x(x_in), y(y_in), z(z_in) {}

  [[nodiscard]] constexpr T length_squared() const noexcept {
    return x * x + y * y + z * z;
  }

  [[nodiscard]] double length() const noexcept {
    return std::sqrt(static_cast<double>(x * x + y * y + z * z));
  }

  [[nodiscard]] constexpr T dot(const Vector3D &rhs) const noexcept {
    return x * rhs.x + y * rhs.y + z * rhs.z;
  }
};

} // namespace wengine::math
