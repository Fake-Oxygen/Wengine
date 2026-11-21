#pragma once

/**
 * @file
 * Single source of truth for unit-text knowledge: the per-dimension
 * suffix-to-SI-scale tables. The DATA lives here, owned by units/ next
 * to Dimension/Value/literals, while cli/args.hpp keeps only the generic
 * parsing machinery reading these tables — this header must never gain
 * parsing logic, and cli/ or individual solvers must never grow their
 * own suffix tables. Consumers are the CLI parse helpers (header-only
 * forwarders also used by the GUI) and signal quantity parsing. New
 * unit recipe: add the literal in units.hpp AND exactly one entry here;
 * nothing else changes for parsing to accept it.
 */

// Single source of truth for unit knowledge: textual suffix -> SI scale.
//
// Previously these tables lived in cli/args.hpp next to the parsing
// machinery while units/ only had literals — the same knowledge
// (e.g. "kV" x 1e3) in two places, able to drift. Now the DATA lives
// here (owned by units/, next to Dimension/Value/literals) and cli/args.hpp
// keeps only the generic parsing machinery, reading these tables.
// New unit: add the literal in units.hpp AND one entry here; nothing else
// needs to change for CLI parsing to accept it. Time rows additionally
// carry their exact integer picosecond factor (TimeScale), so exact-time
// consumers never re-derive it by double comparison.

#include "wengine/units/units.hpp"

#include <array>
#include <cstdint>
#include <string_view>

namespace wengine::units {

/**
 * @brief One accepted unit-suffix spelling plus its multiplier to the
 *        SI base value.
 */
struct SuffixScale {
  /**
   * @brief Suffix text accepted after the number (e.g. "mV", "kOhm").
   */
  std::string_view suffix;
  /**
   * @brief Scale factor; parsed number * scale gives the SI base value
   *        (in the SI base unit of the dimension, e.g. volts for
   *        ElectricPotential).
   */
  double scale; // multiply the parsed number to get the SI base value
};

/**
 * @brief Per-dimension suffix table trait.
 *
 * @note The primary template is intentionally undefined: asking for the
 *       scales of a dimension without registered suffixes is a
 *       compile-time error, which is exactly what we want.
 */
// Per-dimension suffix tables. Primary template is intentionally undefined:
// asking for scales of a dimension without registered suffixes is a
// compile-time error, which is exactly what we want.
template <typename Dim>
struct scales_for;

/**
 * @brief Accepted spellings for ElectricPotential (practical unit volt).
 */
template <>
struct scales_for<ElectricPotential> {
  /**
   * @brief Suffix table; parsed number * scale yields volts (SI base).
   */
  static constexpr std::array<SuffixScale, 4> value{{
      {"V", 1.0},
      {"mV", 1e-3},
      {"kV", 1e3},
      {"MV", 1e6},
  }};
};

/**
 * @brief Accepted spellings for ElectricCurrent (practical unit ampere).
 */
template <>
struct scales_for<ElectricCurrent> {
  /**
   * @brief Suffix table; parsed number * scale yields amperes (SI base).
   */
  static constexpr std::array<SuffixScale, 5> value{{
      {"A", 1.0},
      {"mA", 1e-3},
      {"uA", 1e-6},
      {"kA", 1e3},
      {"MA", 1e6},
  }};
};

/**
 * @brief Accepted spellings for ElectricResistance (practical unit ohm;
 *        "R"/"kR" are the schematic shorthand spellings).
 */
template <>
struct scales_for<ElectricResistance> {
  /**
   * @brief Suffix table; parsed number * scale yields ohms (SI base).
   */
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

/**
 * @brief Accepted spellings for Power (practical unit watt).
 */
template <>
struct scales_for<Power> {
  /**
   * @brief Suffix table; parsed number * scale yields watts (SI base).
   */
  static constexpr std::array<SuffixScale, 4> value{{
      {"W", 1.0},
      {"mW", 1e-3},
      {"kW", 1e3},
      {"MW", 1e6},
  }};
};

/**
 * @brief Accepted spellings for Energy (practical unit joule; "Ws"/"Wh"/
 *        "kWh"/"MWh" are the composite watt-second spellings).
 */
template <>
struct scales_for<Energy> {
  /**
   * @brief Suffix table; parsed number * scale yields joules (SI base).
   */
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

/**
 * @brief Accepted spellings for Frequency (practical unit hertz;
 *        "rpm"/"rps" are revolution-per-minute/second spellings for
 *        motor speed).
 */
template <>
struct scales_for<Frequency> {
  /**
   * @brief Suffix table; parsed number * scale yields hertz (SI base).
   */
  static constexpr std::array<SuffixScale, 6> value{{
      {"Hz", 1.0},
      {"mHz", 1e-3},
      {"kHz", 1e3},
      {"MHz", 1e6},
      {"rpm", 1.0 / 60.0}, // revolutions per minute (motor speed)
      {"rps", 1.0},        // revolutions per second (motor speed)
  }};
};

/**
 * @brief One Time row: owned spelling + SI scale + exact picosecond factor.
 *
 * Extends the SuffixScale contract (suffix/scale, read by the generic
 * double parser) with the exact integer pair (ps_mult, ps_exp10) such
 * that one unit == ps_mult * 10^ps_exp10 picoseconds exactly. ps_mult
 * is coprime to 5 — load-bearing for the sub-picosecond proof in
 * exact-time consumers (cli/time_ps.hpp): a stripped nonzero fraction
 * against a remaining denominator of 10^d with d >= 3 can never clear
 * without a factor of 5 from the multiplier.
 *
 * The constructor requires all four members: a new Time row that omits
 * the factor fails to compile (no side table to forget). The
 * static_assert below pins the parser's preconditions (nonzero 5-free
 * multiplier, covered exponent, fitting product).
 */
struct TimeScale {
  /** @brief Suffix text accepted after the number (e.g. "ms", "min"). */
  std::string_view suffix;
  /** @brief Scale factor; parsed number * scale yields seconds (SI base). */
  double scale;
  /** @brief Exact integer multiplier of the picosecond value (5-free). */
  std::uint64_t ps_mult;
  /** @brief Decimal exponent of the picosecond value (10^ps_exp10). */
  int ps_exp10;
  /**
   * @brief Build one Time row (all four members required).
   * @param[in] s Suffix spelling.
   * @param[in] k SI scale (seconds per unit).
   * @param[in] m Exact picosecond multiplier (nonzero, coprime to 5).
   * @param[in] e Exact picosecond decimal exponent.
   */
  constexpr TimeScale(std::string_view s, double k, std::uint64_t m, int e) noexcept
      : suffix(s), scale(k), ps_mult(m), ps_exp10(e) {}
  /**
   * @brief Exact whole picoseconds per unit (fits u64 — pinned below).
   * @return ps_mult * 10^ps_exp10.
   */
  [[nodiscard]] constexpr std::uint64_t ps_per_unit() const noexcept {
    std::uint64_t v = ps_mult;
    for (int i = 0; i < ps_exp10; ++i) {
      v *= 10;
    }
    return v;
  }
};

namespace time_detail {
/**
 * @brief Constexpr 10^e for the exactness static_assert below.
 * @param[in] e Non-negative exponent (rows pin e <= 19).
 * @return 10 to the power e.
 */
[[nodiscard]] constexpr std::uint64_t pow10_u64(int e) noexcept {
  std::uint64_t v = 1;
  for (int i = 0; i < e; ++i) {
    v *= 10;
  }
  return v;
}
} // namespace time_detail

/**
 * @brief Accepted spellings for Time (practical unit second;
 *        "min"/"h" are the composite minute/hour spellings).
 */
template <>
struct scales_for<Time> {
  /**
   * @brief Suffix table; parsed number * scale yields seconds (SI base),
   *        while ps_mult/ps_exp10 give the exact picosecond factor per
   *        row (read by cli/time_ps.hpp — no double classification there).
   */
  static constexpr std::array<TimeScale, 8> value{{
      {"s", 1.0, 1, 12},    // 10^12 ps
      {"sec", 1.0, 1, 12},  // 10^12 ps
      {"ms", 1e-3, 1, 9},   // 10^9 ps
      {"us", 1e-6, 1, 6},   // 10^6 ps
      {"ns", 1e-9, 1, 3},   // 10^3 ps
      {"min", 60.0, 6, 13}, // 6x10^13 ps
      {"h", 3600.0, 36, 14},// 36x10^14 ps
      {"ps", 1e-12, 1, 0},   // 1 ps
  }};
};

// Compile-time pin on the exactness contract exact-time consumers rely
// on: every row carries a nonzero 5-free multiplier with an exponent the
// parser's power table covers, and the reconstructed picosecond value
// fits the u64 field. A degenerate future row fails here, loudly.
static_assert([]() constexpr noexcept {
  for (const auto &r : scales_for<Time>::value) {
    if (r.ps_mult == 0 || r.ps_mult % 5 == 0) {
      return false;
    }
    // exp10 <= 17: the parser's fraction accumulator holds fewer than
    // 10^(exp10+2) <= 10^19 digits worth of value (fits u64); the
    // power table itself covers 0..19.
    if (r.ps_exp10 < 0 || r.ps_exp10 > 17) {
      return false;
    }
    if (r.ps_mult > 0xFFFFFFFFFFFFFFFFULL / time_detail::pow10_u64(r.ps_exp10)) {
      return false;
    }
  }
  return true;
}());

/**
 * @brief Accepted spellings for MeterConstant (Count per Energy).
 *
 * @note The canonical base is impulses per kWh (composite, non-SI
 *       unit); the SI base value is impulses per joule.
 */
// Canonical base is impulses per kWh (composite, non-SI unit).
template <>
struct scales_for<MeterConstant> {
  /**
   * @brief Suffix table; parsed number * scale yields impulses per
   *        joule (SI base Count/Energy).
   */
  static constexpr std::array<SuffixScale, 6> value{{
      {"imp/kWh", 1.0},
      {"imp/Wh", 1000.0},
      {"imp/J", 3600000.0},
      {"imp/Ws", 3600000.0},
      {"imp/kJ", 3600.0},
      {"imp/MWh", 0.001},
  }};
};

/**
 * @brief Accepted spellings for Count (dimensionless: impulse/pulse
 *        counts, where a bare number is a count).
 */
// Dimensionless counts (bare number = count).
template <>
struct scales_for<Count> {
  /**
   * @brief Suffix table; parsed number * scale yields the plain count
   *        (dimensionless, scale always 1).
   */
  static constexpr std::array<SuffixScale, 5> value{{
      {"imp", 1.0},
      {"imps", 1.0},
      {"pulse", 1.0},
      {"pulses", 1.0},
      {"cnt", 1.0},
  }};
};

/**
 * @brief Accepted spellings for Capacitance (practical unit farad).
 */
template <>
struct scales_for<Capacitance> {
  /**
   * @brief Suffix table; parsed number * scale yields farads (SI base).
   */
  static constexpr std::array<SuffixScale, 5> value{{
      {"F", 1.0},
      {"mF", 1e-3},
      {"uF", 1e-6},
      {"nF", 1e-9},
      {"pF", 1e-12},
  }};
};

} // namespace wengine::units
