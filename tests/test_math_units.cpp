#include "wengine/cli/args.hpp"
#include "wengine/units/scales.hpp"
#include "wengine/units/units.hpp"

#include <glm/glm.hpp>

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
  // MHz literal spot-check (scale-table proof is table-driven above).
  static_assert(12.0_MHz == 12000000.0_Hz);
  assert(1.0_MHz == 1000.0_Hz + 999000.0_Hz);
  // rpm/rps literals (motor-speed spellings of the Frequency table).
  static_assert(1.0_rps == 1.0_Hz);
  static_assert(60.0_rpm == 1.0_Hz);
  assert((60.0_rpm).get() == 1.0 && (1.0_rps).get() == 1.0);
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
void check_table(wengine::core::Result<double> (*parse)(std::string_view)) {
  for (const auto &e : scales_for<Dim>::value) {
    const std::string tok = "1" + std::string(e.suffix);
    const auto r = parse(tok);
    assert(r.ok() && r.value == e.scale);
  }
  const auto bare = parse("2.5");
  assert(bare.ok() && bare.value == 2.5); // bare number stays SI
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

  const glm::dvec2 a{3.0, 4.0};
  assert(glm::dot(a, a) == 25.0);
  assert(glm::length(a) == 5.0);

  const glm::dvec3 b{1.0, 2.0, 2.0};
  assert(glm::length(b) == 3.0);
  assert(b.x == 1.0 && b.y == 2.0 && b.z == 2.0);

  static_assert(glm::dot(glm::dvec2{3.0, 4.0}, glm::dvec2{3.0, 4.0}) == 25.0);

  std::cout << "test_math_units: OK\n";
  return 0;
}
