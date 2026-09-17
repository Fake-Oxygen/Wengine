#include "wengine/math/vector.hpp"
#include "wengine/units/units.hpp"

#include <cassert>
#include <iostream>

int main() {
  const wengine::math::Vector2D<double> a{3.0, 4.0};
  assert(a.length_squared() == 25.0);
  assert(a.length() == 5.0);

  const wengine::math::Vector3D<double> b{1.0, 2.0, 2.0};
  assert(b.length() == 3.0);

  static_assert(wengine::math::Vector2D<int>{3, 4}.length_squared() == 25);

  std::cout << "test_math_units: OK\n";
  return 0;
}
