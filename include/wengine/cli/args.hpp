#pragma once

/**
 * @file
 * Universal CLI argument parser: zero-copy tokenizer (string_view into
 * the caller-owned line buffer), ArgCursor over token spans, and the
 * parse_* scalar/quantity helpers. Goals: no exceptions, no allocations
 * or string-stream machinery in the hot path, constexpr-friendly
 * primitives, noexcept where possible. Unit knowledge (suffix-to-SI
 * scale tables) lives in units/scales.hpp - the single source of truth;
 * this header keeps only the generic parsing machinery plus thin
 * per-quantity forwarders (parse_voltage, parse_quantity_value, and
 * friends) and must never grow independent unit tables. Exact
 * whole-picosecond time parsing lives in cli/time_ps.hpp (it serves one
 * verb, not every command). Consumers: every command implementation
 * plus the GUI, which includes this header alone with no wengine_cli
 * link (suffix tables stay owned by units/).
 * Named arguments take the `--key value` and `--key=value` forms; stray
 * positionals are errors, never silently ignored.
 */

// Universal, high-performance CLI argument parser.
//
// Goals: zero-copy (string_view into caller-owned line buffer), no exceptions,
// no <sstream>/<stoi>/regex in hot path, constexpr-friendly primitives.
// Everything here is header-only and noexcept where possible.
//
// Unit knowledge (suffix -> SI scale tables) lives in units/scales.hpp —
// the single source of truth. This header keeps only the generic parsing
// machinery and thin per-quantity forwarders.

#include "wengine/core/status.hpp"
#include "wengine/signal/types.hpp"
#include "wengine/units/scales.hpp"

#include <charconv>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace wengine::cli {

// ---------------------------------------------------------------- tokenizer
// Single-pass, allocation-free token primitives. Handles whitespace,
// single/double quotes (grouping, outer quotes stripped). Backslash escapes
// the next char inside quotes (quote char itself or backslash); escape is
// resolved by *keeping both chars* to stay zero-copy — callers needing full
// unescaping can copy. For the physics CLI this is the right perf tradeoff.

/**
 * @brief Skip leading ASCII whitespace (space, tab, newline, carriage return).
 * @param[in] s View to trim (caller-owned storage).
 * @return The view minus its leading whitespace run.
 */
[[nodiscard]] constexpr std::string_view trim_front(std::string_view s) noexcept {
  std::size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) {
    ++i;
  }
  return s.substr(i);
}

// Consume one token from front of `rest` (mutated to remainder).
// Returns empty view only when no token remains.
/**
 * @brief Consume one token from the front of a line.
 * @param[in,out] rest Line remainder; advanced past the token and framing.
 * @return The token view (outer quotes stripped for quoted tokens);
 *         empty only when no token remains.
 * @note Single/double quotes group words; escape handling inside quotes
 *       keeps both characters (zero-copy), so callers needing full
 *       unescaping must copy. Views alias the caller line.
 */
[[nodiscard]] constexpr std::string_view next_token(std::string_view &rest) noexcept {
  rest = trim_front(rest);
  if (rest.empty()) {
    return {};
  }
  const char q = rest.front();
  if (q == '"' || q == '\'') {
    // Quoted: scan to matching quote.
    std::size_t i = 1;
    while (i < rest.size() && rest[i] != q) {
      if (rest[i] == '\\' && i + 1 < rest.size() &&
          (rest[i + 1] == q || rest[i + 1] == '\\')) {
        i += 2;
      } else {
        ++i;
      }
    }
    std::string_view tok = rest.substr(1, i > 0 ? i - 1 : 0);
    rest = (i < rest.size()) ? rest.substr(i + 1) : std::string_view{};
    return tok;
  }
  std::size_t i = 0;
  while (i < rest.size() && rest[i] != ' ' && rest[i] != '\t' && rest[i] != '\n' &&
         rest[i] != '\r') {
    ++i;
  }
  std::string_view tok = rest.substr(0, i);
  rest = rest.substr(i);
  return tok;
}

// Tokenize `line` into pre-allocated `out`. Returns token count (may exceed
// out.size(); extra tokens are counted but not stored).
/**
 * @brief Tokenize a line into a pre-allocated span.
 * @param[in] line Caller-owned line to split (views alias it).
 * @param[out] out Span receiving one view per token, up to its size.
 * @return Token count, possibly exceeding out.size() (extra tokens are
 *         counted, not stored).
 */
inline std::size_t tokenize_into(std::string_view line,
                                 std::span<std::string_view> out) noexcept {
  std::size_t n = 0;
  std::string_view rest = line;
  while (true) {
    rest = trim_front(rest);
    if (rest.empty()) {
      break;
    }
    std::string_view tok = next_token(rest);
    // next_token on whitespace-only returns empty; guard against loop.
    if (tok.empty() && rest.empty()) {
      break;
    }
    if (n < out.size()) {
      out[n] = tok;
    }
    ++n;
    // Defensive: if nothing consumed, break (should not happen).
    if (tok.empty()) {
      break;
    }
  }
  return n;
}

/**
 * @brief Tokenize a line into an owned vector of views.
 * @param[in] line Caller-owned line to split (views alias it; it must
 *                 outlive the result).
 * @return One view per token, with a small head reserve to avoid regrowth.
 * @note Allocates the vector (unlike tokenize_into); the views stay
 *       zero-copy into the caller line.
 */
inline std::vector<std::string_view> tokenize_view(std::string_view line) {
  std::vector<std::string_view> out;
  out.reserve(8);
  std::string_view rest = line;
  while (true) {
    rest = trim_front(rest);
    if (rest.empty()) {
      break;
    }
    std::string_view tok = next_token(rest);
    if (tok.empty()) {
      break;
    }
    out.push_back(tok);
  }
  return out;
}

// ------------------------------------------------------------ parse results
// Unified error vocabulary: parsing returns `core::Result<T>` (the single
// value-or-error type — see `core/status.hpp`). There is no second type:
// former `ParseResult<T>` call sites now spell `core::Result<T>` directly.

namespace detail {

/**
 * @brief Parse a bare number (from_chars, full consumption required).
 * @param[in] s Text to parse (must be non-empty).
 * @return Success with the value; failure (kInvalidArgument) on empty
 *         text, partial consumption, or a malformed number.
 */
inline core::Result<double> parse_double_impl(std::string_view s) noexcept {
  if (s.empty()) {
    return core::Result<double>::failure("expected number, got empty");
  }
  double v = 0.0;
  const char *beg = s.data();
  const char *end = s.data() + s.size();
  auto res = std::from_chars(beg, end, v);
  if (res.ec != std::errc{} || res.ptr != end) {
    return core::Result<double>::failure("invalid number");
  }
  return core::Result<double>::success(v);
}

/**
 * @brief Parse a bare integer (from_chars, full consumption required).
 * @param[in] s Text to parse (must be non-empty).
 * @return Success with the value; failure (kInvalidArgument) on empty
 *         text, partial consumption, or a malformed integer.
 */
inline core::Result<long long> parse_i64_impl(std::string_view s) noexcept {
  if (s.empty()) {
    return core::Result<long long>::failure("expected integer, got empty");
  }
  long long v = 0;
  const char *beg = s.data();
  const char *end = s.data() + s.size();
  auto res = std::from_chars(beg, end, v);
  if (res.ec != std::errc{} || res.ptr != end) {
    return core::Result<long long>::failure("invalid integer");
  }
  return core::Result<long long>::success(v);
}

/**
 * @brief Parse a boolean word (1/true/on/yes and 0/false/off/no).
 * @param[in] s Text to parse.
 * @return Success with the value; failure (kInvalidArgument) on any
 *         other word.
 */
inline core::Result<bool> parse_bool_impl(std::string_view s) noexcept {
  if (s == "1" || s == "true" || s == "on" || s == "yes") {
    return core::Result<bool>::success(true);
  }
  if (s == "0" || s == "false" || s == "off" || s == "no") {
    return core::Result<bool>::success(false);
  }
  return core::Result<bool>::failure("invalid bool (true/false/1/0/on/off)");
}

// Split "12.4kV" -> {"12.4", "kV"}. Numeric prefix = [0-9+-.eE], rest = suffix.
/**
 * @brief Split quantity text into numeric prefix plus unit suffix.
 * @param[in] s Text to split (numeric prefix is the leading run of
 *            sign/digit/dot/exponent characters).
 * @return Pair of numeric text and suffix text (either may be empty).
 */
inline std::pair<std::string_view, std::string_view> split_number_suffix(
    std::string_view s) noexcept {
  std::size_t i = 0;
  while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i] == '+' || s[i] == '-' ||
                          s[i] == '.' || s[i] == 'e' || s[i] == 'E')) {
    ++i;
  }
  return {s.substr(0, i), s.substr(i)};
}

// Suffix tables live in units/scales.hpp (single source of truth); this
// stays generic over any such table (any row type with suffix/scale
// members — SuffixScale rows and richer rows alike, e.g. TimeScale).
/**
 * @brief Parse a number with an optional unit suffix from any table.
 * @param[in] s Text to parse.
 * @param[in] table Suffix-to-scale table (a units-owned row set — array
 *                  or span; this stays generic over any row type with
 *                  suffix/scale members).
 * @return Bare SI value on an empty suffix, scaled SI value on a known
 *         suffix; failure (kInvalidArgument) on empty numeric text, a bad
 *         number, or an unknown suffix.
 */
template <typename Table>
inline core::Result<double> parse_scaled_impl(std::string_view s, const Table &table) noexcept {
  auto [num, suf] = split_number_suffix(s);
  if (num.empty()) {
    return core::Result<double>::failure("expected number with optional suffix");
  }
  auto n = parse_double_impl(num);
  if (!n.ok()) {
    return n;
  }
  if (suf.empty()) {
    return n; // bare SI value
  }
  for (const auto &e : table) {
    if (e.suffix == suf) {
      return core::Result<double>::success(n.value * e.scale);
    }
  }
  return core::Result<double>::failure("unknown unit suffix");
}

} // namespace detail

// Public scalar parsers (thin, noexcept wrappers).
/**
 * @brief Parse a bare number (thin wrapper, no suffix).
 * @param[in] s Text to parse.
 * @return Success with the value; failure (kInvalidArgument) on a
 *         malformed number.
 */
[[nodiscard]] inline core::Result<double> parse_double(std::string_view s) noexcept {
  return detail::parse_double_impl(s);
}
/**
 * @brief Parse a bare integer (thin wrapper, no suffix).
 * @param[in] s Text to parse.
 * @return Success with the value; failure (kInvalidArgument) on a
 *         malformed integer.
 */
[[nodiscard]] inline core::Result<long long> parse_i64(std::string_view s) noexcept {
  return detail::parse_i64_impl(s);
}
/**
 * @brief Parse a boolean word (thin wrapper).
 * @param[in] s Text to parse.
 * @return Success with the value; failure (kInvalidArgument) on any
 *         other word.
 */
[[nodiscard]] inline core::Result<bool> parse_bool(std::string_view s) noexcept {
  return detail::parse_bool_impl(s);
}

// Per-quantity forwarders onto the units-owned suffix tables
// (units::scales_for — the single source of truth).
/**
 * @brief Parse voltage text with the units-owned potential table.
 * @param[in] s Text such as `12.4V` (spellings owned by units/).
 * @return SI volts; failure (kInvalidArgument) on a bad number or an
 *         unknown suffix.
 * @note Thin forwarder into units/scales.hpp; no suffix knowledge here.
 */
[[nodiscard]] inline core::Result<double> parse_voltage(std::string_view s) noexcept {
  return detail::parse_scaled_impl(s, units::scales_for<units::ElectricPotential>::value);
}
/**
 * @brief Parse current text with the units-owned current table.
 * @param[in] s Text such as `0.54A` (spellings owned by units/).
 * @return SI amperes; failure (kInvalidArgument) on a bad number or an
 *         unknown suffix.
 * @note Thin forwarder into units/scales.hpp; no suffix knowledge here.
 */
[[nodiscard]] inline core::Result<double> parse_current(std::string_view s) noexcept {
  return detail::parse_scaled_impl(s, units::scales_for<units::ElectricCurrent>::value);
}
/**
 * @brief Parse resistance text with the units-owned resistance table.
 * @param[in] s Text such as `4.7kOhm` (spellings owned by units/).
 * @return SI ohms; failure (kInvalidArgument) on a bad number or an
 *         unknown suffix.
 * @note Thin forwarder into units/scales.hpp; no suffix knowledge here.
 */
[[nodiscard]] inline core::Result<double> parse_resistance(std::string_view s) noexcept {
  return detail::parse_scaled_impl(s, units::scales_for<units::ElectricResistance>::value);
}
/**
 * @brief Parse power text with the units-owned power table.
 * @param[in] s Text such as `2kW` (spellings owned by units/).
 * @return SI watts; failure (kInvalidArgument) on a bad number or an
 *         unknown suffix.
 * @note Thin forwarder into units/scales.hpp; no suffix knowledge here.
 */
[[nodiscard]] inline core::Result<double> parse_power(std::string_view s) noexcept {
  return detail::parse_scaled_impl(s, units::scales_for<units::Power>::value);
}
/**
 * @brief Parse energy text with the units-owned energy table.
 * @param[in] s Text such as `2kWh` (spellings owned by units/).
 * @return SI joules; failure (kInvalidArgument) on a bad number or an
 *         unknown suffix.
 * @note Thin forwarder into units/scales.hpp; no suffix knowledge here.
 */
[[nodiscard]] inline core::Result<double> parse_energy(std::string_view s) noexcept {
  return detail::parse_scaled_impl(s, units::scales_for<units::Energy>::value);
}
/**
 * @brief Parse frequency text with the units-owned frequency table.
 * @param[in] s Text such as `1kHz` (spellings owned by units/).
 * @return SI hertz; failure (kInvalidArgument) on a bad number or an
 *         unknown suffix.
 * @note Thin forwarder into units/scales.hpp; no suffix knowledge here.
 */
[[nodiscard]] inline core::Result<double> parse_frequency(std::string_view s) noexcept {
  return detail::parse_scaled_impl(s, units::scales_for<units::Frequency>::value);
}
// Signal-stamped value parser: dispatches to the units-owned suffix table
// of the stamped quantity (the single table in signal::kQuantities).
// This is the only quantity-aware parser commands need — no ad-hoc
// voltage/current switches at call sites.
/**
 * @brief Parse signal-stamped text against one quantity row.
 * @param[in] q Quantity id selecting the units-owned suffix table (the
 *                single signal::kQuantities row).
 * @param[in] s Text such as `3.3V` or `2mA` (cross-dimension text fails:
 *                volts text against a current quantity is refused).
 * @return SI value in the quantity dimension; failure
 *         (kInvalidArgument) on an unknown quantity, a bad number, or an
 *         unknown suffix.
 * @note The only quantity-aware parser commands need - no voltage/current
 *       switches at call sites.
 */
[[nodiscard]] inline core::Result<double>
parse_quantity_value(signal::QuantityId q, std::string_view s) noexcept {
  const signal::QuantityDesc *d = nullptr;
  for (const signal::QuantityDesc &row : signal::kQuantities) {
    if (row.id == q) {
      d = &row;
      break;
    }
  }
  if (d == nullptr) {
    return core::Result<double>::failure("unknown quantity");
  }
  return detail::parse_scaled_impl(s, d->scales);
}[[nodiscard]] inline core::Result<double> parse_time(std::string_view s) noexcept/**< @brief Parse time text with the units-owned time table. @param[in] s Text such as `10ms` (spellings owned by units/). @return SI seconds; failure (kInvalidArgument) on a bad number or an unknown suffix. @note Thin forwarder into units/scales.hpp; no suffix knowledge here. */ {
  return detail::parse_scaled_impl(s, units::scales_for<units::Time>::value);
}
/**
 * @brief Parse meter-constant text with the units-owned table.
 * @param[in] s Text such as `1000imp/kWh` (spellings owned by units/).
 * @return SI-consistent meter constant; failure (kInvalidArgument) on a
 *         bad number or an unknown suffix.
 * @note Thin forwarder into units/scales.hpp; no suffix knowledge here.
 */
[[nodiscard]] inline core::Result<double> parse_meter_const(std::string_view s) noexcept {
  return detail::parse_scaled_impl(s, units::scales_for<units::MeterConstant>::value);
}
/**
 * @brief Parse pulse-count text with the units-owned count table.
 * @param[in] s Text such as `500` or `500imp` (spellings owned by units/).
 * @return Pulse count; failure (kInvalidArgument) on a bad number or an
 *         unknown suffix.
 * @note Thin forwarder into units/scales.hpp; no suffix knowledge here.
 */
[[nodiscard]] inline core::Result<double> parse_pulses(std::string_view s) noexcept {
  return detail::parse_scaled_impl(s, units::scales_for<units::Count>::value);
}
/**
 * @brief Parse capacitance text with the units-owned capacitance table.
 * @param[in] s Text such as `200pF` (spellings owned by units/).
 * @return SI farads; failure (kInvalidArgument) on a bad number or an
 *         unknown suffix.
 * @note Thin forwarder into units/scales.hpp; no suffix knowledge here.
 */
[[nodiscard]] inline core::Result<double> parse_capacitance(std::string_view s) noexcept {
  return detail::parse_scaled_impl(s, units::scales_for<units::Capacitance>::value);
}

// --------------------------------------------------------------- ArgCursor
// Universal cursor over token span with consumed-bitmask (up to 64 tokens).
// Supports mixed positional + named/flag styles without allocation:
//   --key value | --key=value | -k value | positional
//   --flag (bool, no value) | --no-flag (negated)
/**
 * @brief Cursor over a token span with a consumed bitmask (up to 64 tokens).
 * @note Mixed positional plus named/flag styles without allocation:
 *       `--key value`, `--key=value`, short `-k`, bare positionals, plus
 *       `--flag` and `--no-flag` negation. The bitmask covers 64 tokens;
 *       longer lines are rejected by callers via kMaxArgs. Views alias
 *       the caller token storage, which must outlive the cursor.
 */
class ArgCursor {
public:
  /**
   * @brief Cursor over caller-owned token views.
   * @param[in] tokens Token span to walk (must outlive the cursor).
   */
  explicit ArgCursor(std::span<const std::string_view> tokens) noexcept
      : tokens_(tokens), consumed_(0) {}

  /**
   * @brief Token count in the walked span.
   * @return Number of tokens (consumed or not).
   */
  [[nodiscard]] std::size_t size() const noexcept { return tokens_.size(); }
  /**
   * @brief Whether the walked span is empty.
   * @return True when there are no tokens at all.
   */
  [[nodiscard]] bool empty() const noexcept { return tokens_.empty(); }

  // -- options -----------------------------------------------------------
  // Find first unconsumed token equal to any of `names`, or starting with
  // "name=" prefix. On "--key value": consumes key + next token as value.
  // On "--key=value": consumes key token, returns suffix after '='.
  // Returns nullopt when absent.
  /**
   * @brief Take the value of the first unconsumed matching option.
   * @param[in] names Option spellings to match.
   * @return The value view (`--key value` consumes the key plus the next
   *         unconsumed token; `--key=value` consumes the key token and
   *         returns the suffix after `=`); nullopt when absent, or when a
   *         `--key` form has no following value.
   */
  std::optional<std::string_view> take_option(
      std::initializer_list<std::string_view> names) noexcept {
    for (std::size_t i = 0; i < tokens_.size(); ++i) {
      if (is_consumed(i)) {
        continue;
      }
      std::string_view t = tokens_[i];
      for (std::string_view n : names) {
        if (t == n) {
          // "--key value" form: value = next unconsumed token.
          for (std::size_t j = i + 1; j < tokens_.size(); ++j) {
            if (!is_consumed(j)) {
              mark_consumed(i);
              mark_consumed(j);
              return tokens_[j];
            }
          }
          return std::nullopt; // key without value
        }
        if (t.size() > n.size() && t.substr(0, n.size()) == n && t[n.size()] == '=') {
          mark_consumed(i);
          return t.substr(n.size() + 1);
        }
      }
    }
    return std::nullopt;
  }

  // Boolean flag: presence of any of `names` (consumes it). Supports
  // "--no-<name>" negation when `allow_negation` and names are long opts.
  /**
   * @brief Take a boolean flag (presence wins; `--no-` prefix negates).
   * @param[in] names Flag spellings to match.
   * @param[in] allow_negation False disables `--no-` handling (long
   *            `--opt` forms only; short forms never negate).
   * @return True when a positive spelling was present (consumed); false
   *         when absent or explicitly negated (a negation still consumes
   *         its token, so it never leaks into positionals).
   */
  bool take_flag(std::initializer_list<std::string_view> names,
                 bool allow_negation = true) noexcept {
    for (std::size_t i = 0; i < tokens_.size(); ++i) {
      if (is_consumed(i)) {
        continue;
      }
      std::string_view t = tokens_[i];
      for (std::string_view n : names) {
        if (t == n) {
          mark_consumed(i);
          return true;
        }
        if (allow_negation && n.size() > 2 && n.substr(0, 2) == "--") {
          std::string_view bare = n.substr(2);
          if (t.size() == bare.size() + 5 && t.substr(0, 5) == "--no-" &&
              t.substr(5) == bare) {
            mark_consumed(i);
            return false; // explicitly negated; treat as absent
          }
        }
      }
    }
    return false;
  }

  /**
   * @brief Peek for a flag spelling without consuming it.
   * @param[in] names Flag spellings to look for.
   * @return True when any spelling is present unconsumed.
   */
  [[nodiscard]] bool has_flag(std::initializer_list<std::string_view> names) const noexcept {
    for (std::size_t i = 0; i < tokens_.size(); ++i) {
      if (is_consumed(i)) {
        continue;
      }
      for (std::string_view n : names) {
        if (tokens_[i] == n) {
          return true;
        }
      }
    }
    return false;
  }

  // -- positionals --------------------------------------------------------
  // Next unconsumed token that does not look like an option ("-..."),
  // or any unconsumed token if `include_options` is true.
  /**
   * @brief Take the next unconsumed positional token.
   * @param[in] include_options True also matches dash-leading tokens.
   * @return The token view; nullopt when none remains.
   * @note Dash-leading tokens are skipped unless include_options is set,
   *       so options are never eaten as positionals by accident.
   */
  std::optional<std::string_view> next_positional(bool include_options = false) noexcept {
    for (std::size_t i = 0; i < tokens_.size(); ++i) {
      if (is_consumed(i)) {
        continue;
      }
      std::string_view t = tokens_[i];
      if (!include_options && t.size() > 1 && t[0] == '-') {
        continue;
      }
      mark_consumed(i);
      return t;
    }
    return std::nullopt;
  }

  // -- typed helpers ------------------------------------------------------
  /**
   * @brief Take the next positional as a number.
   * @return Success with the value; failure (kInvalidArgument) when no
   *         positional remains or the text is not a number.
   */
  core::Result<double> next_double() noexcept {
    auto o = next_positional();
    if (!o) {
      return core::Result<double>::failure("missing number argument");
    }
    return parse_double(*o);
  }

  /**
   * @brief Take a named option value as a number.
   * @param[in] names Option spellings to match.
   * @return Success with the value; failure (kInvalidArgument) when the
   *         option is absent or its text is not a number.
   */
  core::Result<double> take_double_option(
      std::initializer_list<std::string_view> names) noexcept {
    auto o = take_option(names);
    if (!o) {
      return core::Result<double>::failure("option not provided");
    }
    return parse_double(*o);
  }

  // -- diagnostics ---------------------------------------------------------
  /**
   * @brief Whether any unconsumed token remains.
   * @return True when at least one token is still unconsumed (stray
   *         input commands must refuse loudly, never silently ignore).
   */
  [[nodiscard]] bool has_remaining() const noexcept {
    for (std::size_t i = 0; i < tokens_.size(); ++i) {
      if (!is_consumed(i)) {
        return true;
      }
    }
    return false;
  }

  /**
   * @brief First unconsumed token without consuming it.
   * @return The token view; nullopt when nothing remains (used to echo
   *         the stray token in usage errors).
   */
  [[nodiscard]] std::optional<std::string_view> first_remaining() const noexcept {
    for (std::size_t i = 0; i < tokens_.size(); ++i) {
      if (!is_consumed(i)) {
        return tokens_[i];
      }
    }
    return std::nullopt;
  }

  /**
   * @brief Count of consumed tokens (cursor progress, not a limit check).
   * @return How many tokens take_option/take_flag/next_positional took.
   */
  [[nodiscard]] std::size_t consumed_count() const noexcept {
    std::size_t n = 0;
    for (std::size_t i = 0; i < tokens_.size(); ++i) {
      n += is_consumed(i) ? 1 : 0;
    }
    return n;
  }

private:
  void mark_consumed(std::size_t i) noexcept {
    if (i < 64) {
      consumed_ |= (1ULL << i);
    }
    // Beyond 64 tokens: ignore mask (treated as always unconsumed).
    // CLI lines this long are rejected by callers via kMaxArgs.
  }
  [[nodiscard]] bool is_consumed(std::size_t i) const noexcept {
    if (i >= 64) {
      return false;
    }
    return (consumed_ & (1ULL << i)) != 0;
  }

  std::span<const std::string_view> tokens_;
  std::uint64_t consumed_;
};

/**
 * @brief Maximum CLI tokens per line (matches the consumed-bitmask width).
 * @note Callers reject longer lines; tokens past 64 would alias the mask.
 */
inline constexpr std::size_t kMaxArgs = 64;

} // namespace wengine::cli
