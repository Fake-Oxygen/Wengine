#pragma once

// UART wire codec for the SIG-8 Zephyr HIL track (host side, header-only).
//
// Two modes share one line language (the signals-CSV row shape, so the
// UART stream, the CSV log, and the Zephyr parser never drift):
//
//   ascii:  <step>,<time>,<sig_id>,<value>,<quantity>\n        (stream)
//           !<control line>\n                                (control, both ways)
//   cobs:   COBS('S' + <line>) + 0x00                        (stream)
//           COBS('Q' + <line>) + 0x00                        (device->host cmd)
//           COBS('R' + <line>) + 0x00                        (host->device rsp)
//
// Stream lines never start with '!' (they start with a step digit) and
// never contain 0x00, so the ascii demux is unambiguous; in cobs mode
// every wire unit is a 0x00-delimited frame distinguished by its type
// byte. Doubles print with %.17g (max_digits10: exact round-trip, `==`
// on re-read — the same guarantee as the CSV log). Control lines are the
// SIG-10 text lines verbatim (see signal/control.hpp); the framing lives
// here, the verbs live there (D4b-A).
//
// Resync: a garbage byte run ends at the next delimiter in both modes
// (`\n` / 0x00); the offending unit is counted malformed and dropped,
// never half-parsed. COBS decode failures (bad code byte, trailing span
// overrun, empty frame) are malformed too — loud, never silent.
//
// This header is pure + portable (no termios, no fd): the POSIX port TU
// (src/link/uart_publisher.cpp) owns the serial device, and the Zephyr
// sample ships its own C99 parser (examples/zephyr/src/wengine_parse.*)
// which the host test compiles and cross-checks against the builder here.

#include "wengine/core/status.hpp"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <system_error>

namespace wengine::link::uart {

// Default serial profile: 115200 8N1 (8 data, no parity, 1 stop — set in
// the TU via termios; this header only names the rate).
inline constexpr unsigned kDefaultUartBaud = 115200;
// Wire modes, selected explicitly (never silent): raw lines vs COBS frames.
enum class UartMode : std::uint8_t { kAscii = 0, kCobs = 1 };
// Control marker (ascii mode) and frame type bytes (cobs mode).
inline constexpr char kControlPrefix = '!';
inline constexpr char kFrameStream = 'S';
inline constexpr char kFrameCommand = 'Q';
inline constexpr char kFrameResponse = 'R';
// Largest COBS frame payload the host accepts (matches the control-plane
// line cap; stream lines are far shorter).
inline constexpr std::size_t kMaxFrameBytes = 4096;

[[nodiscard]] inline core::Result<UartMode> parse_mode(std::string_view s) noexcept {
  if (s == "ascii") {
    return core::Result<UartMode>::success(UartMode::kAscii);
  }
  if (s == "cobs") {
    return core::Result<UartMode>::success(UartMode::kCobs);
  }
  return core::Result<UartMode>::failure("want ascii|cobs");
}

// Baud rates configurable via --emit-uart <device[,baud]> (termios mapping
// lives in the TU; unknown rates are usage errors, never silent 9600).
inline constexpr std::array<unsigned, 8> kSupportedBauds{{9600u, 19200u, 38400u, 57600u,
                                                          115200u, 230400u, 460800u, 921600u}};

[[nodiscard]] inline bool baud_supported(unsigned baud) noexcept {
  for (unsigned b : kSupportedBauds) {
    if (b == baud) {
      return true;
    }
  }
  return false;
}

// Split "device[,baud]" on the LAST comma (device paths win over the rate).
struct UartEndpoint {
  std::string device;
  unsigned baud = kDefaultUartBaud;
};

[[nodiscard]] inline core::Result<UartEndpoint> parse_uart_endpoint(std::string_view text) {
  if (text.empty()) {
    return core::Result<UartEndpoint>::failure("want device[,baud]");
  }
  std::string_view device = text;
  unsigned baud = kDefaultUartBaud;
  const std::size_t comma = text.rfind(',');
  if (comma != std::string_view::npos) {
    device = text.substr(0, comma);
    const std::string_view rate = text.substr(comma + 1);
    if (device.empty() || rate.empty()) {
      return core::Result<UartEndpoint>::failure("want device[,baud]");
    }
    unsigned v = 0;
    for (char c : rate) {
      if (c < '0' || c > '9') {
        return core::Result<UartEndpoint>::failure("baud must be numeric");
      }
      v = v * 10u + static_cast<unsigned>(c - '0');
      if (v > 4000000u) {
        return core::Result<UartEndpoint>::failure("baud out of range");
      }
    }
    if (!baud_supported(v)) {
      return core::Result<UartEndpoint>::failure("unsupported baud (want 9600..921600)");
    }
    baud = v;
  }
  if (device.empty()) {
    return core::Result<UartEndpoint>::failure("want device[,baud]");
  }
  UartEndpoint ep;
  ep.device.assign(device.data(), device.size());
  ep.baud = baud;
  return core::Result<UartEndpoint>::success(std::move(ep));
}

// --- Consistent Overhead Byte Stuffing (Stuart, 1997) ---
//
// encode appends the COBS frame WITHOUT the 0x00 delimiter (callers add
// exactly one); decode takes one delimiter-free frame. Empty input encodes
// to a single 0x01 code byte; empty frames never decode (no silent empty
// commands). Overhead is at most ceil(n/254)+1 bytes (<0.5% past 254 B).

[[nodiscard]] inline core::Status cobs_encode(std::string_view in, std::string &out) {
  // Worst case: every 254 data bytes cost one code byte, plus the final.
  if (in.size() > kMaxFrameBytes) {
    return {core::StatusCode::kInvalidArgument};
  }
  out.reserve(out.size() + in.size() + in.size() / 254 + 2);
  std::size_t code_at = out.size();
  out.push_back('\x01'); // placeholder code byte
  unsigned run = 1;
  for (unsigned char c : in) {
    if (c == 0) {
      out[code_at] = static_cast<char>(run);
      code_at = out.size();
      out.push_back('\x01');
      run = 1;
    } else {
      out.push_back(static_cast<char>(c));
      ++run;
      if (run == 255) {
        out[code_at] = static_cast<char>(255);
        code_at = out.size();
        out.push_back('\x01');
        run = 1;
      }
    }
  }
  out[code_at] = static_cast<char>(run);
  return core::Status::success();
}

[[nodiscard]] inline core::Result<std::string> cobs_decode(std::string_view frame) {
  using R = core::Result<std::string>;
  if (frame.empty() || frame.size() > kMaxFrameBytes + kMaxFrameBytes / 254 + 2) {
    return R::failure("bad COBS frame");
  }
  std::string out;
  out.reserve(frame.size());
  std::size_t i = 0;
  while (i < frame.size()) {
    const unsigned code = static_cast<unsigned char>(frame[i++]);
    if (code == 0) {
      return R::failure("bad COBS frame"); // 0x00 only delimits, never inside
    }
    const std::size_t span = static_cast<std::size_t>(code - 1);
    if (i + span > frame.size()) {
      return R::failure("bad COBS frame"); // span overruns the frame
    }
    out.append(frame.data() + i, span);
    i += span;
    if (code < 255 && i < frame.size()) {
      out.push_back('\0'); // implicit zero (not after a 0xFF block or at end)
    }
  }
  return R::success(std::move(out));
}

// --- Stream line: <step>,<time>,<sig_id>,<value>,<quantity> ---
//
// Identical to one signals-CSV row (same field order, same %.17g doubles)
// so golden corpora and parsers are shared, not duplicated.

[[nodiscard]] inline std::string build_stream_line(std::uint64_t step, double time,
                                                   std::uint64_t sig_id, double value,
                                                   std::string_view quantity) {
  char tbuf[32];
  char vbuf[32];
  const int tn = std::snprintf(tbuf, sizeof(tbuf), "%.17g", time);
  const int vn = std::snprintf(vbuf, sizeof(vbuf), "%.17g", value);
  const std::string_view t = (tn > 0) ? std::string_view(tbuf, static_cast<std::size_t>(tn))
                                      : std::string_view("0");
  const std::string_view v = (vn > 0) ? std::string_view(vbuf, static_cast<std::size_t>(vn))
                                      : std::string_view("0");
  std::string line;
  line.reserve(64);
  line += std::to_string(step);
  line.push_back(',');
  line.append(t.data(), t.size());
  line.push_back(',');
  line += std::to_string(sig_id);
  line.push_back(',');
  line.append(v.data(), v.size());
  line.push_back(',');
  line.append(quantity.data(), quantity.size());
  return line;
}

struct StreamSample {
  std::uint64_t step = 0;
  double time = 0.0;
  std::uint64_t sig_id = 0;
  double value = 0.0;
  std::string quantity; // "U"/"I" (owned: views would dangle past the line)
};

namespace detail {

[[nodiscard]] inline bool parse_u64_field(std::string_view s, std::uint64_t &v) noexcept {
  if (s.empty()) {
    return false;
  }
  std::uint64_t acc = 0;
  for (char c : s) {
    if (c < '0' || c > '9') {
      return false;
    }
    const unsigned d = static_cast<unsigned>(c - '0');
    if (acc > (0xFFFFFFFFFFFFFFFFull - d) / 10u) {
      return false;
    }
    acc = acc * 10u + d;
  }
  v = acc;
  return true;
}

[[nodiscard]] inline bool parse_f64_field(std::string_view s, double &v) noexcept {
  if (s.empty()) {
    return false;
  }
  // Strict decimal: digits with one optional dot + exponent (no inf/nan,
  // no hex, no whitespace — matches what %.17g emits).
  std::size_t i = 0;
  if (s[i] == '+' || s[i] == '-') {
    if (++i >= s.size()) {
      return false;
    }
  }
  bool digits = false;
  while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
    ++i;
    digits = true;
  }
  if (i < s.size() && s[i] == '.') {
    ++i;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
      ++i;
      digits = true;
    }
  }
  if (!digits) {
    return false;
  }
  if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
    if (++i >= s.size()) {
      return false;
    }
    if (s[i] == '+' || s[i] == '-') {
      if (++i >= s.size()) {
        return false;
      }
    }
    bool exp_digits = false;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
      ++i;
      exp_digits = true;
    }
    if (!exp_digits) {
      return false;
    }
  }
  if (i != s.size()) {
    return false;
  }
  // Shape-validated above, so from_chars (never throws, unlike strtod
  // family error paths the engine forbids) must accept it exactly.
  double acc = 0.0;
  const auto res = std::from_chars(s.data(), s.data() + s.size(), acc);
  if (res.ec != std::errc{} || res.ptr != s.data() + s.size()) {
    return false;
  }
  v = acc;
  return true;
}

} // namespace detail

// Parse one stream line (no trailing newline). Failure is kInvalidArgument;
// `out` is untouched. Quantity accepts exactly the stamped suffixes
// ("U"/"I" — same table as the CSV log).
[[nodiscard]] inline core::Status parse_stream_line(std::string_view line, StreamSample &out) {
  std::string_view f[5];
  std::size_t n = 0;
  std::size_t begin = 0;
  for (std::size_t k = 0; k < line.size() && n < 4; ++k) {
    if (line[k] == ',') {
      f[n++] = line.substr(begin, k - begin);
      begin = k + 1;
    }
  }
  if (n != 4) {
    return {core::StatusCode::kInvalidArgument};
  }
  f[4] = line.substr(begin);
  StreamSample s;
  if (!detail::parse_u64_field(f[0], s.step) || !detail::parse_f64_field(f[1], s.time) ||
      !detail::parse_u64_field(f[2], s.sig_id) || !detail::parse_f64_field(f[3], s.value)) {
    return {core::StatusCode::kInvalidArgument};
  }
  if (f[4] != "U" && f[4] != "I") {
    return {core::StatusCode::kInvalidArgument};
  }
  s.quantity.assign(f[4].data(), f[4].size());
  out = std::move(s);
  return core::Status::success();
}

} // namespace wengine::link::uart
