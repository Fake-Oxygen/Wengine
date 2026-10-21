#pragma once

// Single source of truth for unit knowledge: textual suffix -> SI scale.
//
// Previously these tables lived in cli/args.hpp next to the parsing
// machinery while units/ only had literals — the same knowledge
// (e.g. "kV" x 1e3) in two places, able to drift. Now the DATA lives here
// (owned by units/, next to Dimension/Value/literals) and cli/args.hpp
// keeps only the generic parsing machinery, reading these tables.
// New unit: add the literal in units.hpp AND one entry here; nothing else
// needs to change for CLI parsing to accept it.

#include "wengine/units/units.hpp"

#include <array>
#include <string_view>

namespace wengine::units {

struct SuffixScale {
  std::string_view suffix;
  double scale; // multiply the parsed number to get the SI base value
};

// Per-dimension suffix tables. Primary template is intentionally undefined:
// asking for scales of a dimension without registered suffixes is a
// compile-time error, which is exactly what we want.
template <typename Dim>
struct scales_for;

template <>
struct scales_for<ElectricPotential> {
  static constexpr std::array<SuffixScale, 4> value{{
      {"V", 1.0},
      {"mV", 1e-3},
      {"kV", 1e3},
      {"MV", 1e6},
  }};
};

template <>
struct scales_for<ElectricCurrent> {
  static constexpr std::array<SuffixScale, 5> value{{
      {"A", 1.0},
      {"mA", 1e-3},
      {"uA", 1e-6},
      {"kA", 1e3},
      {"MA", 1e6},
  }};
};

template <>
struct scales_for<ElectricResistance> {
  static constexpr std::array<SuffixScale, 7> value{{
      {"Ohm", 1.0},
      {"ohm", 1.0},
      {"mOhm", 1e-3},
      {"kOhm", 1e3},
      {"MOhm", 1e6},
      {"R", 1.0}, // shorthand
      {"kR", 1e3},
  }};
};

template <>
struct scales_for<Power> {
  static constexpr std::array<SuffixScale, 4> value{{
      {"W", 1.0},
      {"mW", 1e-3},
      {"kW", 1e3},
      {"MW", 1e6},
  }};
};

template <>
struct scales_for<Energy> {
  static constexpr std::array<SuffixScale, 7> value{{
      {"J", 1.0},
      {"Ws", 1.0},
      {"kJ", 1e3},
      {"Wh", 3600.0},
      {"kWh", 3600000.0},
      {"MWh", 3600000000.0},
      {"MJ", 1e6},
  }};
};

template <>
struct scales_for<Frequency> {
  static constexpr std::array<SuffixScale, 6> value{{
      {"Hz", 1.0},
      {"mHz", 1e-3},
      {"kHz", 1e3},
      {"MHz", 1e6},
      {"rpm", 1.0 / 60.0}, // revolutions per minute (motor speed)
      {"rps", 1.0},        // revolutions per second (motor speed)
  }};
};

template <>
struct scales_for<Time> {
  static constexpr std::array<SuffixScale, 8> value{{
      {"s", 1.0},
      {"sec", 1.0},
      {"ms", 1e-3},
      {"us", 1e-6},
      {"ns", 1e-9},
      {"min", 60.0},
      {"h", 3600.0},
      {"ps", 1e-12},
  }};
};

// Canonical base is impulses per kWh (composite, non-SI unit).
template <>
struct scales_for<MeterConstant> {
  static constexpr std::array<SuffixScale, 6> value{{
      {"imp/kWh", 1.0},
      {"imp/Wh", 1000.0},
      {"imp/J", 3600000.0},
      {"imp/Ws", 3600000.0},
      {"imp/kJ", 3600.0},
      {"imp/MWh", 0.001},
  }};
};

// Dimensionless counts (bare number = count).
template <>
struct scales_for<Count> {
  static constexpr std::array<SuffixScale, 5> value{{
      {"imp", 1.0},
      {"imps", 1.0},
      {"pulse", 1.0},
      {"pulses", 1.0},
      {"cnt", 1.0},
  }};
};

template <>
struct scales_for<Capacitance> {
  static constexpr std::array<SuffixScale, 5> value{{
      {"F", 1.0},
      {"mF", 1e-3},
      {"uF", 1e-6},
      {"nF", 1e-9},
      {"pF", 1e-12},
  }};
};

} // namespace wengine::units
