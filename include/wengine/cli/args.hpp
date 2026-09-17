#pragma once

// Universal, high-performance CLI argument parser.
//
// Goals: zero-copy (string_view into caller-owned line buffer), no exceptions,
// no <sstream>/<stoi>/regex in hot path, constexpr-friendly primitives.
// Everything here is header-only and noexcept where possible.

#include <array>
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

[[nodiscard]] constexpr std::string_view trim_front(std::string_view s) noexcept {
  std::size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) {
    ++i;
  }
  return s.substr(i);
}

// Consume one token from front of `rest` (mutated to remainder).
// Returns empty view only when no token remains.
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
template <typename T>
struct ParseResult {
  bool ok{false};
  T value{};
  std::string_view error{};

  [[nodiscard]] static constexpr ParseResult success(T v) noexcept { return {true, v, {}}; }
  [[nodiscard]] static constexpr ParseResult failure(std::string_view e) noexcept {
    return {false, T{}, e};
  }
};

namespace detail {

inline ParseResult<double> parse_double_impl(std::string_view s) noexcept {
  if (s.empty()) {
    return ParseResult<double>::failure("expected number, got empty");
  }
  double v = 0.0;
  const char *beg = s.data();
  const char *end = s.data() + s.size();
  auto res = std::from_chars(beg, end, v);
  if (res.ec != std::errc{} || res.ptr != end) {
    return ParseResult<double>::failure("invalid number");
  }
  return ParseResult<double>::success(v);
}

inline ParseResult<long long> parse_i64_impl(std::string_view s) noexcept {
  if (s.empty()) {
    return ParseResult<long long>::failure("expected integer, got empty");
  }
  long long v = 0;
  const char *beg = s.data();
  const char *end = s.data() + s.size();
  auto res = std::from_chars(beg, end, v);
  if (res.ec != std::errc{} || res.ptr != end) {
    return ParseResult<long long>::failure("invalid integer");
  }
  return ParseResult<long long>::success(v);
}

inline ParseResult<bool> parse_bool_impl(std::string_view s) noexcept {
  if (s == "1" || s == "true" || s == "on" || s == "yes") {
    return ParseResult<bool>::success(true);
  }
  if (s == "0" || s == "false" || s == "off" || s == "no") {
    return ParseResult<bool>::success(false);
  }
  return ParseResult<bool>::failure("invalid bool (true/false/1/0/on/off)");
}

// Split "12.4kV" -> {"12.4", "kV"}. Numeric prefix = [0-9+-.eE], rest = suffix.
inline std::pair<std::string_view, std::string_view> split_number_suffix(
    std::string_view s) noexcept {
  std::size_t i = 0;
  while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i] == '+' || s[i] == '-' ||
                          s[i] == '.' || s[i] == 'e' || s[i] == 'E')) {
    ++i;
  }
  return {s.substr(0, i), s.substr(i)};
}

struct SuffixScale {
  std::string_view suffix;
  double scale;
};

inline ParseResult<double> parse_scaled_impl(
    std::string_view s, std::span<const SuffixScale> table) noexcept {
  auto [num, suf] = split_number_suffix(s);
  if (num.empty()) {
    return ParseResult<double>::failure("expected number with optional suffix");
  }
  auto n = parse_double_impl(num);
  if (!n.ok) {
    return n;
  }
  if (suf.empty()) {
    return n; // bare SI value
  }
  for (const auto &e : table) {
    if (e.suffix == suf) {
      return ParseResult<double>::success(n.value * e.scale);
    }
  }
  return ParseResult<double>::failure("unknown unit suffix");
}

} // namespace detail

// Public scalar parsers (thin, noexcept wrappers).
[[nodiscard]] inline ParseResult<double> parse_double(std::string_view s) noexcept {
  return detail::parse_double_impl(s);
}
[[nodiscard]] inline ParseResult<long long> parse_i64(std::string_view s) noexcept {
  return detail::parse_i64_impl(s);
}
[[nodiscard]] inline ParseResult<bool> parse_bool(std::string_view s) noexcept {
  return detail::parse_bool_impl(s);
}

// Default SI suffix tables (base units V / A / Ohm).
inline constexpr std::array<detail::SuffixScale, 4> kVoltageSuffixes{{
    {"V", 1.0},
    {"mV", 1e-3},
    {"kV", 1e3},
    {"MV", 1e6},
}};
inline constexpr std::array<detail::SuffixScale, 4> kCurrentSuffixes{{
    {"A", 1.0},
    {"mA", 1e-3},
    {"kA", 1e3},
    {"MA", 1e6},
}};
inline constexpr std::array<detail::SuffixScale, 7> kResistanceSuffixes{{
    {"Ohm", 1.0},
    {"ohm", 1.0},
    {"mOhm", 1e-3},
    {"kOhm", 1e3},
    {"MOhm", 1e6},
    {"R", 1.0}, // shorthand
    {"kR", 1e3},
}};

[[nodiscard]] inline ParseResult<double> parse_voltage(std::string_view s) noexcept {
  return detail::parse_scaled_impl(s, kVoltageSuffixes);
}
[[nodiscard]] inline ParseResult<double> parse_current(std::string_view s) noexcept {
  return detail::parse_scaled_impl(s, kCurrentSuffixes);
}
[[nodiscard]] inline ParseResult<double> parse_resistance(std::string_view s) noexcept {
  return detail::parse_scaled_impl(s, kResistanceSuffixes);
}

// --------------------------------------------------------------- ArgCursor
// Universal cursor over token span with consumed-bitmask (up to 64 tokens).
// Supports mixed positional + named/flag styles without allocation:
//   --key value | --key=value | -k value | positional
//   --flag (bool, no value) | --no-flag (negated)
class ArgCursor {
public:
  explicit ArgCursor(std::span<const std::string_view> tokens) noexcept
      : tokens_(tokens), consumed_(0) {}

  [[nodiscard]] std::size_t size() const noexcept { return tokens_.size(); }
  [[nodiscard]] bool empty() const noexcept { return tokens_.empty(); }

  // -- options -----------------------------------------------------------
  // Find first unconsumed token equal to any of `names`, or starting with
  // "name=" prefix. On "--key value": consumes key + next token as value.
  // On "--key=value": consumes key token, returns suffix after '='.
  // Returns nullopt when absent.
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
  ParseResult<double> next_double() noexcept {
    auto o = next_positional();
    if (!o) {
      return ParseResult<double>::failure("missing number argument");
    }
    return parse_double(*o);
  }

  ParseResult<double> take_double_option(
      std::initializer_list<std::string_view> names) noexcept {
    auto o = take_option(names);
    if (!o) {
      return ParseResult<double>::failure("option not provided");
    }
    return parse_double(*o);
  }

  // -- diagnostics ---------------------------------------------------------
  [[nodiscard]] bool has_remaining() const noexcept {
    for (std::size_t i = 0; i < tokens_.size(); ++i) {
      if (!is_consumed(i)) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] std::optional<std::string_view> first_remaining() const noexcept {
    for (std::size_t i = 0; i < tokens_.size(); ++i) {
      if (!is_consumed(i)) {
        return tokens_[i];
      }
    }
    return std::nullopt;
  }

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

inline constexpr std::size_t kMaxArgs = 64;

} // namespace wengine::cli
