#pragma once

// Table/CSV replay source (SIG-4a): replays a sampled waveform with linear
// interpolation over source time.
//
// File format (documented, byte-stable): a text CSV with the exact header
//   time,value
// followed by one `t,v` row per line. Times are SI seconds, values are SI
// (volts/amps per the source's stamped quantity — no unit suffixes in the
// table; quantity-suffixed text lives at CLI/GUI edges only). Doubles use
// full precision (write with max_digits10 for bit-exact replay); a single
// trailing '\r' per line is tolerated (CRLF files), anything else strict:
// exactly one comma per row, both fields from_chars doubles, finite, times
// strictly increasing, 1..kMaxTablePoints points.
//
// Evaluation policy (locked for 4a):
//   * exact time hit (t == stored time, bit-for-bit) returns the stored
//     value — so replaying a logged run at its own sample times is
//     bit-identical to the source file (== on re-read, max_digits10);
//   * between points: linear interpolation in fixed order
//     v0 + (v1 - v0) * ((t - t0) / (t1 - t0));
//   * outside the span: clamp (hold first/last) — replay neither
//     extrapolates nor fails at the edges; starting a source at t = 0 with
//     a table beginning later simply holds the first sample.
// The lookup is binary search (log n per step); bracketing is unique for
// strictly increasing times, so the result is independent of the search
// method — deterministic by construction.

#include "wengine/core/status.hpp"

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <istream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wengine::signal {

struct TablePoint {
  double t = 0.0;
  double v = 0.0;

  bool operator==(const TablePoint &) const noexcept = default;
};

inline constexpr std::size_t kMaxTablePoints = 65536;

namespace table_detail {

[[nodiscard]] inline bool parse_double_field(std::string_view s, double &out) noexcept {
  if (s.empty()) {
    return false;
  }
  const char *beg = s.data();
  const char *end = s.data() + s.size();
  double v = 0.0;
  const auto res = std::from_chars(beg, end, v);
  if (res.ec != std::errc{} || res.ptr != end) {
    return false;
  }
  if (!std::isfinite(v)) {
    return false;
  }
  out = v;
  return true;
}

} // namespace table_detail

// Structural validation for point spans (configure edge, batch driver,
// loaded blobs): non-empty, bounded, finite, strictly increasing times.
[[nodiscard]] inline core::Status
validate_table_points(std::span<const TablePoint> pts) noexcept {
  if (pts.empty() || pts.size() > kMaxTablePoints) {
    return {core::StatusCode::kInvalidArgument};
  }
  for (std::size_t k = 0; k < pts.size(); ++k) {
    if (!std::isfinite(pts[k].t) || !std::isfinite(pts[k].v)) {
      return {core::StatusCode::kInvalidArgument};
    }
    if (k > 0 && !(pts[k].t > pts[k - 1].t)) {
      return {core::StatusCode::kInvalidArgument};
    }
  }
  return core::Status::success();
}

// Parse a table CSV (see header doc) into `out`. Clears `out` on failure;
// `error` carries a line-numbered reason (config-edge allocation only).
[[nodiscard]] inline core::Status parse_table_csv(std::istream &in,
                                                 std::vector<TablePoint> &out,
                                                 std::string &error) {
  out.clear();
  error.clear();
  const auto invalid = [&](const std::string &msg) {
    out.clear();
    error = msg;
    return core::Status{core::StatusCode::kInvalidArgument};
  };
  std::string line;
  if (!std::getline(in, line)) {
    return invalid("empty table: missing header");
  }
  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }
  if (line != "time,value") {
    return invalid("bad table header on line 1: '" + line + "'");
  }
  std::uint64_t line_no = 1;
  double prev_t = 0.0;
  bool have_prev = false;
  while (std::getline(in, line)) {
    ++line_no;
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      return invalid("malformed table row on line " + std::to_string(line_no));
    }
    const std::size_t comma = line.find(',');
    if (comma == std::string::npos || line.find(',', comma + 1) != std::string::npos) {
      return invalid("malformed table row on line " + std::to_string(line_no));
    }
    double t = 0.0;
    double v = 0.0;
    if (!table_detail::parse_double_field(std::string_view(line.data(), comma), t) ||
        !table_detail::parse_double_field(
            std::string_view(line.data() + comma + 1, line.size() - comma - 1), v)) {
      return invalid("malformed table row on line " + std::to_string(line_no));
    }
    if (have_prev && !(t > prev_t)) {
      return invalid("table time not increasing on line " + std::to_string(line_no));
    }
    if (out.size() >= kMaxTablePoints) {
      return invalid("too many table points (max " + std::to_string(kMaxTablePoints) +
                     ")");
    }
    out.push_back(TablePoint{t, v});
    prev_t = t;
    have_prev = true;
  }
  if (out.empty()) {
    return invalid("empty table: no rows");
  }
  return core::Status::success();
}

// Interpolate `t` over parallel time/value arrays (n >= 1, times strictly
// increasing — the configure-time invariant). Infallible by contract:
// clamps outside the span, exact stored value on exact hits.
[[nodiscard]] inline double eval_table(std::span<const double> times,
                                      std::span<const double> values, double t) noexcept {
  const std::size_t n = times.size();
  if (n == 0 || values.size() != n) {
    return 0.0; // unreachable: solver guards empty ranges loudly
  }
  if (t <= times[0]) {
    return values[0];
  }
  if (t >= times[n - 1]) {
    return values[n - 1];
  }
  // Binary search for the first index with times[k] >= t (unique bracket:
  // times strictly increasing, so lo/hi converge independently of method).
  std::size_t lo = 0;
  std::size_t hi = n - 1;
  while (hi - lo > 1) {
    const std::size_t mid = lo + (hi - lo) / 2;
    if (times[mid] < t) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  if (times[hi] == t) {
    return values[hi]; // exact hit: stored value, bit-identical
  }
  const double t0 = times[lo];
  const double t1 = times[hi];
  const double v0 = values[lo];
  const double v1 = values[hi];
  return v0 + (v1 - v0) * ((t - t0) / (t1 - t0));
}

} // namespace wengine::signal
