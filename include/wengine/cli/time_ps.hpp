#pragma once

/**
 * @file
 * Exact whole-picosecond time parse (013 delay annotation): integer
 * arithmetic only — no floating point participates anywhere, so
 * exactness holds to the full u64 field range. Lives beside the
 * logic delay verb it serves (set-delay's only text consumer plus
 * its test) instead of the universal parser header: every command
 * and the GUI include cli/args.hpp, and only one verb needs this
 * 170-line grammar. The per-unit exact factor is owned by the
 * units/scales.hpp Time rows (TimeScale ps_mult/ps_exp10) — this
 * header reads the rows, never re-derives them.
 *
 * Dependency rules: header-only, allocation-free, exception-free, no
 * strings, no I/O, no syscalls, `[[nodiscard]]` `core::Status`
 * returns; never includes CLI types beyond the generic parsing
 * machinery (logic -> core/status only at the engine edge; this is
 * CLI-layer text handling). Consumers: the set-delay verb and
 * tests/test_logic_delay.cpp.
 */

#include "wengine/cli/args.hpp"
#include "wengine/units/scales.hpp"

#include <charconv>
#include <cstdint>
#include <string_view>

namespace wengine::cli {

// Exact whole-picosecond time parse (013): integer arithmetic only — no
// floating point participates anywhere, so exactness holds to the full
// u64 field range. Reads spellings and exact per-unit factors from the
// owned scales_for<Time> rows; no suffix knowledge lives here.
/**
 * @brief Parse exact whole-picosecond time text (013 delay annotation).
 *
 * Grammar: bare `^[0-9]+$` means integer picoseconds (full u64 range,
 * exact via `std::from_chars`); otherwise a plain decimal
 * `^[0-9]+(\.[0-9]+)?$` plus one owned `scales_for<Time>` spelling
 * (`s sec ms us ns min h ps`, case-sensitive), converted by pure
 * integer arithmetic (checked u64 multiply/add against the row's
 * exact picosecond factor, with an explicit u64 overflow check).
 * Stored now, honored later (docs/logic.md §12).
 *
 * @param[in] s Text such as `1500`, `1.5ns`, or `250ps`.
 * @return Whole picoseconds; failure (kInvalidArgument, netlist
 *         untouched by callers) on negative input ("delay must be
 *         non-negative"), sub-picosecond remainders, field overflow,
 *         unknown/cross-dimension suffixes, or malformed grammar.
 * @note Reads the exact factor from the matched Time row
 *       (units/scales.hpp TimeScale); unknown rows refuse loudly
 *       rather than guessing.
 */
[[nodiscard]] inline core::Result<std::uint64_t> parse_time_ps(std::string_view s) noexcept {
  auto [num, suf] = detail::split_number_suffix(s);
  if (suf.empty()) {
    // Bare form: integer picoseconds only.
    if (num.empty()) {
      return core::Result<std::uint64_t>::failure("bad delay value");
    }
    if (num.front() == '-') {
      return core::Result<std::uint64_t>::failure("delay must be non-negative");
    }
    for (char c : num) {
      if (c < '0' || c > '9') {
        return core::Result<std::uint64_t>::failure("bare form is integer picoseconds; use a suffix for other spellings");
      }
    }
    std::uint64_t v = 0;
    const char *beg = num.data();
    const char *end = num.data() + num.size();
    const auto res = std::from_chars(beg, end, v);
    if (res.ec != std::errc{} || res.ptr != end) {
      return core::Result<std::uint64_t>::failure("value exceeds the 64-bit picosecond field");
    }
    return core::Result<std::uint64_t>::success(v);
  }
  // Suffixed form: optional leading '-' still refuses as negative (the
  // numeric run admits signs); the remaining numeric text must be plain
  // decimal ^[0-9]+(\.[0-9]+)?$ (exponents refuse loudly).
  if (!num.empty() && num.front() == '-') {
    return core::Result<std::uint64_t>::failure("delay must be non-negative");
  }
  if (num.empty()) {
    return core::Result<std::uint64_t>::failure("bad delay value");
  }
  std::string_view int_part = num;
  std::string_view frac_part{};
  {
    std::size_t dot = num.find('.');
    if (dot != std::string_view::npos) {
      if (num.find('.', dot + 1) != std::string_view::npos) {
        return core::Result<std::uint64_t>::failure("bad delay value");
      }
      int_part = num.substr(0, dot);
      frac_part = num.substr(dot + 1);
      if (int_part.empty() || frac_part.empty()) {
        return core::Result<std::uint64_t>::failure("bad delay value");
      }
    }
    for (char c : int_part) {
      if (c < '0' || c > '9') {
        return core::Result<std::uint64_t>::failure("bad delay value");
      }
    }
    for (char c : frac_part) {
      if (c < '0' || c > '9') {
        return core::Result<std::uint64_t>::failure("bad delay value");
      }
    }
  }
  // Owned-row lookup (case-sensitive, exact spelling): the row carries
  // the exact integer factor — no double-literal classification here.
  // Cross-dimension text (e.g. `1500V`) simply misses and refuses below.
  std::uint64_t mult = 1;
  int exp10 = 0;
  bool found = false;
  for (const auto &e : units::scales_for<units::Time>::value) {
    if (e.suffix == suf) {
      mult = e.ps_mult;
      exp10 = e.ps_exp10;
      found = true;
      break;
    }
  }
  if (!found) {
    return core::Result<std::uint64_t>::failure("unknown time suffix");
  }
  // The multiplier m in {1, 6, 36} carries no factor of 5 (pinned by
  // static_assert in units/scales.hpp) — load-bearing for the
  // sub-picosecond proof below (a stripped nonzero fraction can only
  // clear a remaining denominator of 10^d for d <= 2).
  // Strip value-preserving zeros: leading int zeros (keep one digit) and
  // trailing fraction zeros (shrinks the divisor exactly).
  while (int_part.size() > 1 && int_part.front() == '0') {
    int_part.remove_prefix(1);
  }
  while (!frac_part.empty() && frac_part.back() == '0') {
    frac_part.remove_suffix(1);
  }
  // Every unit is >= 1ps, so an integer part above 10^20 already exceeds
  // the u64 field whatever the fraction contributes.
  if (int_part.size() > 20) {
    return core::Result<std::uint64_t>::failure("value exceeds the 64-bit picosecond field");
  }
  constexpr std::uint64_t kU64Max = 0xFFFFFFFFFFFFFFFFULL;
  constexpr std::uint64_t kPow10[] = {
      1ULL, 10ULL, 100ULL, 1000ULL, 10000ULL, 100000ULL, 1000000ULL, 10000000ULL, 100000000ULL,
      1000000000ULL, 10000000000ULL, 100000000000ULL, 1000000000000ULL, 10000000000000ULL,
      100000000000000ULL, 1000000000000000ULL, 10000000000000000ULL, 100000000000000000ULL,
      1000000000000000000ULL, 10000000000000000000ULL};
  const int frac_digits = static_cast<int>(frac_part.size());
  // A stripped nonzero fraction against a remaining denominator of 10^d
  // with d >= 3 can never clear it: 5^d must divide the fraction (the
  // multiplier carries no factor of 5) and 2^d must nearly divide it, so
  // the fraction would end in zero — contradicting the strip. Refuse as
  // sub-picosecond without touching wider arithmetic.
  const int cancelled = (frac_digits < exp10) ? frac_digits : exp10;
  if (!frac_part.empty() && frac_digits - cancelled >= 3) {
    return core::Result<std::uint64_t>::failure("not a whole number of picoseconds");
  }
  // Integer part: checked u64 accumulation (C >= 1 below, so an
  // unrepresentable integer already exceeds the field on its own).
  std::uint64_t int_v = 0;
  for (char c : int_part) {
    const std::uint64_t d = static_cast<std::uint64_t>(static_cast<unsigned>(c - '0'));
    if (int_v > (kU64Max - d) / 10U) {
      return core::Result<std::uint64_t>::failure("value exceeds the 64-bit picosecond field");
    }
    int_v = int_v * 10U + d;
  }
  std::uint64_t frac_v = 0;
  for (char c : frac_part) {
    const std::uint64_t d = static_cast<std::uint64_t>(static_cast<unsigned>(c - '0'));
    // Bounded above: frac_digits <= exp10 + 2 <= 19 (rows pin exp10 <= 17),
    // so no overflow here.
    frac_v = frac_v * 10U + d;
  }
  // Whole picoseconds per unit: mult * 10^exp10 (exact, fits u64 — both
  // pinned by static_assert in units/scales.hpp).
  const std::uint64_t per_unit = mult * kPow10[exp10];
  if (int_v > kU64Max / per_unit) {
    return core::Result<std::uint64_t>::failure("value exceeds the 64-bit picosecond field");
  }
  std::uint64_t whole = int_v * per_unit;
  // value_ps = mult * (int_v * 10^exp10 + frac_v * 10^exp10 / 10^frac_digits).
  if (frac_digits <= exp10) {
    const std::uint64_t scaled = frac_v * kPow10[exp10 - frac_digits];
    const std::uint64_t term = scaled * mult;
    if (whole > kU64Max - term) {
      return core::Result<std::uint64_t>::failure("value exceeds the 64-bit picosecond field");
    }
    whole += term;
  } else {
    const int rem = frac_digits - exp10; // 1 or 2 by the proof above
    const std::uint64_t num_v = frac_v * mult;
    const std::uint64_t den_v = kPow10[rem];
    if (num_v % den_v != 0U) {
      return core::Result<std::uint64_t>::failure("not a whole number of picoseconds");
    }
    const std::uint64_t term = num_v / den_v;
    if (whole > kU64Max - term) {
      return core::Result<std::uint64_t>::failure("value exceeds the 64-bit picosecond field");
    }
    whole += term;
  }
  return core::Result<std::uint64_t>::success(whole);
}

} // namespace wengine::cli
