#include "wengine/cli/args.hpp"
#include "wengine/math/vector.hpp"
#include "wengine/units/scales.hpp"
#include "wengine/units/units.hpp"

#include <cassert>
#include <iostream>
#include <sstream>
#include <string>

namespace {

using namespace wengine::units;
using namespace wengine::units::literals;

void test_value_comparisons() {
  static_assert(12.0_V > 3.0_V);
  static_assert(3.0_V <= 3.0_V);
  static_assert(!(3.0_V == 12.0_V));

  assert(12.0_V == 12.0_V);
  assert(12.0_V != 3.0_V);
  assert(3.0_V < 12.0_V);
  assert(3.0_V <= 12.0_V);
  assert(12.0_V <= 12.0_V);
  assert(12.0_V > 3.0_V);
  assert(12.0_V >= 12.0_V);
  assert(!(12.0_V < 12.0_V));
  // Cross-magnitude via literals (kV vs V share the dimension).
  assert(0.012_kW > 3.0_W);
}

void test_value_printing() {
  std::ostringstream out;
  out << 12.5_V;
  assert(out.str() == "12.5"); // raw SI base value; callers add context
  std::ostringstream out2;
  out2 << 2.0_A;
  assert(out2.str() == "2");
}

// Single-source proof: every suffix registered in units/scales.hpp must
// parse through the CLI forwarders to exactly its table scale. If a table
// and the parser ever drift apart, this fails.
template <typename Dim>
void check_table(wengine::cli::ParseResult<double> (*parse)(std::string_view)) {
  for (const auto &e : scales_for<Dim>::value) {
    const std::string tok = "1" + std::string(e.suffix);
    const auto r = parse(tok);
    assert(r.ok && r.value == e.scale);
  }
  const auto bare = parse("2.5");
  assert(bare.ok && bare.value == 2.5); // bare number stays SI
}

void test_scales_single_source() {
  // Spot-check table contents live in units (sizes pin the moved tables).
  static_assert(scales_for<ElectricPotential>::value.size() == 4);
  static_assert(scales_for<ElectricResistance>::value.size() == 7);
  static_assert(scales_for<Time>::value.size() == 8);
  static_assert(scales_for<MeterConstant>::value.size() == 6);
  assert(scales_for<ElectricPotential>::value[0].suffix == "V");
  assert(scales_for<Time>::value[5].suffix == "min");
  assert(scales_for<Time>::value[5].scale == 60.0);

  check_table<ElectricPotential>(wengine::cli::parse_voltage);
  check_table<ElectricCurrent>(wengine::cli::parse_current);
  check_table<ElectricResistance>(wengine::cli::parse_resistance);
  check_table<Power>(wengine::cli::parse_power);
  check_table<Energy>(wengine::cli::parse_energy);
  check_table<Frequency>(wengine::cli::parse_frequency);
  check_table<Time>(wengine::cli::parse_time);
  check_table<MeterConstant>(wengine::cli::parse_meter_const);
  check_table<Count>(wengine::cli::parse_pulses);
  check_table<Capacitance>(wengine::cli::parse_capacitance);
}

} // namespace

int main() {
  test_value_comparisons();
  test_value_printing();
  test_scales_single_source();

  const wengine::math::Vector2D<double> a{3.0, 4.0};
  assert(a.length_squared() == 25.0);
  assert(a.length() == 5.0);

  const wengine::math::Vector3D<double> b{1.0, 2.0, 2.0};
  assert(b.length() == 3.0);

  static_assert(wengine::math::Vector2D<int>{3, 4}.length_squared() == 25);

  std::cout << "test_math_units: OK\n";
  return 0;
}
