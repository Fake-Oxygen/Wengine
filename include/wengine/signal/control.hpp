#pragma once

// SIG-10 generic control plane: text-line protocol vocabulary (engine leaf).
//
// This header owns the wire-facing half of the control plane and nothing
// else: verb/param enums, strict line parsing, and response-line builders.
// It never touches simulation state (no world/ include — signal/ stays a
// leaf per AGENTS.md) and performs no I/O: callers own the transport
// (TCP server, UART COBS framer, CLI, GUI) and the step-boundary drain.
//
// Protocol (v1, documented in docs/signals.md sec. 6). One command per
// line, space-separated, uppercase verbs, `\n`-terminated (a trailing `\r`
// is stripped so telnet-style clients work). Transports carry SI doubles;
// unit suffixes (`3.3V`, `2mA`) are a frontend concern parsed with
// cli::parse_quantity_value before enqueue — the engine only ever sees SI.
//
//   HELLO <version> <client-name...>  version handshake + run provenance
//   PING                              liveness probe
//   LIST                              read-only source census (slot order)
//   ADD <kind> <quantity> [KEY VALUE]... [OPS s0,s1] [EXPR <program...>]
//                                     spawn a source (serial in response)
//   SET <serial> <PARAM> <value>       retune one parameter mid-run
//   START <serial> / STOP <serial>     resume (phase-exact) / freeze
//   DROP <serial>                      detach the source (serial never reused)
//   SELECT <serial> <index>            mux input select (reserved: rejected
//                                     until the mux combinator exists)
//
// PARAM is one of FREQ AMP OFFSET PHASE DUTY SEED QUANTITY. ADD keys are
// the same names (FREQ/AMP/OFFSET/PHASE/DUTY/SEED) plus OPS (sum operand
// serials, comma-separated, required for kind=sum) and EXPR (expression
// program: must be last, the rest of the line is the program text).
// Table/Lua sources need bulk payloads (CSV points / script text) that do
// not fit a text line: ADD rejects them as not_implemented — batch runs
// (`sig run --sig-table/--sig-lua`) and the SIG-6 per-source family own
// those payloads.
//
// Responses are single lines, except LIST which answers `OK list <n>`
// followed by exactly <n> `SRC` lines (all lines `\n`-terminated on the
// wire; the engine appends them to a caller-owned vector here):
//
//   OK hello 1 run=<run_id>     (exact version match required)
//   OK pong
//   OK list <n>  +  SRC <serial> <kind> <quantity> <freq> <amp> <offset>
//                   <phase> <duty> <seed> <running>   (%.17g doubles, exact)
//   OK added <serial>
//   OK set <serial> <PARAM>
//   OK started|stopped|dropped <serial>   (START/STOP are idempotent)
//   ERR <code> <message>        code mirrors core::Status: invalid_argument,
//                              out_of_range, not_implemented, internal
//
// Parsing is strict and total: any shape violation (unknown verb, arity,
// bad number, unknown kind/quantity/param, overlong operand list) fails
// with kInvalidArgument and a literal message — never a throw, never
// partial state. Applying a parsed command is the driver's half (see
// solvers/signal_control.hpp): parse here mutates nothing.

#include "wengine/core/status.hpp"
#include "wengine/signal/types.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace wengine::signal::control {

// Wire protocol version. Exact-match at HELLO: any other version is
// rejected loudly (never a silent downgrade), like the socket hello gates.
inline constexpr std::uint32_t kProtoVersion = 1;
// Longest accepted command line in bytes (transports refuse longer lines
// with ERR before the engine ever sees them; the engine re-checks).
inline constexpr std::size_t kMaxLineBytes = 4096;

enum class Verb : std::uint8_t {
  kHello,
  kPing,
  kList,
  kAdd,
  kSet,
  kStart,
  kStop,
  kDrop,
  kSelect,
};

enum class Param : std::uint8_t {
  kFreq,
  kAmp,
  kOffset,
  kPhase,
  kDuty,
  kSeed,
  kQuantity,
  kUnknown,
};

[[nodiscard]] inline constexpr std::string_view param_name(Param p) noexcept {
  switch (p) {
  case Param::kFreq:
    return "FREQ";
  case Param::kAmp:
    return "AMP";
  case Param::kOffset:
    return "OFFSET";
  case Param::kPhase:
    return "PHASE";
  case Param::kDuty:
    return "DUTY";
  case Param::kSeed:
    return "SEED";
  case Param::kQuantity:
    return "QUANTITY";
  case Param::kUnknown:
    return "?";
  }
  return "?";
}

[[nodiscard]] inline constexpr Param parse_param(std::string_view s) noexcept {
  if (s == "FREQ") {
    return Param::kFreq;
  }
  if (s == "AMP") {
    return Param::kAmp;
  }
  if (s == "OFFSET") {
    return Param::kOffset;
  }
  if (s == "PHASE") {
    return Param::kPhase;
  }
  if (s == "DUTY") {
    return Param::kDuty;
  }
  if (s == "SEED") {
    return Param::kSeed;
  }
  if (s == "QUANTITY") {
    return Param::kQuantity;
  }
  return Param::kUnknown;
}

[[nodiscard]] inline constexpr std::string_view status_name(core::StatusCode c) noexcept {
  switch (c) {
  case core::StatusCode::kOk:
    return "ok";
  case core::StatusCode::kInvalidArgument:
    return "invalid_argument";
  case core::StatusCode::kOutOfRange:
    return "out_of_range";
  case core::StatusCode::kNotImplemented:
    return "not_implemented";
  case core::StatusCode::kInternal:
    return "internal";
  }
  return "internal";
}

namespace detail {

// %.17g round-trips IEEE doubles exactly (same guarantee as the CSV log).
[[nodiscard]] inline std::string fmt_f64(double v) {
  char buf[32];
  const int n = std::snprintf(buf, sizeof(buf), "%.17g", v);
  if (n <= 0 || static_cast<std::size_t>(n) >= sizeof(buf)) {
    return "0";
  }
  return std::string(buf, static_cast<std::size_t>(n));
}

} // namespace detail

// One SRC census row, shared by the LIST apply path (solvers/) and edge
// census caches (single formatter, never two — a byte-equality test locks
// this): "SRC <serial> <kind> <qty> <freq> <amp> <offset> <phase> <duty>
// <seed> <running>" with exact doubles.
[[nodiscard]] inline std::string format_src_row(std::uint64_t serial, std::string_view kind_name,
                                               std::string_view qty_suffix, double freq, double amp,
                                               double offset, double phase, double duty,
                                               std::uint64_t seed, bool running) {
  return "SRC " + std::to_string(serial) + " " + std::string(kind_name) + " " +
         std::string(qty_suffix) + " " + detail::fmt_f64(freq) + " " + detail::fmt_f64(amp) +
         " " + detail::fmt_f64(offset) + " " + detail::fmt_f64(phase) + " " +
         detail::fmt_f64(duty) + " " + std::to_string(seed) + " " +
         (running ? "1" : "0");
}

// Parsed command. Views (`client`, `expr`) borrow the queued line: the
// caller keeps the line alive until the command is applied (the bus drains
// from its own stored strings, so this always holds there).
struct Command {
  Verb verb = Verb::kPing;
  std::uint64_t serial = 0; // SET/START/STOP/DROP/SELECT target
  // HELLO:
  std::uint32_t hello_version = 0;
  std::string_view client;
  // ADD:
  SignalKind kind = SignalKind::kSine;
  QuantityId quantity = QuantityId::kVoltage;
  SignalParams params; // parametric keys over SignalParams defaults
  std::array<std::uint64_t, SignalParams::kMaxOps> ops{};
  std::uint8_t op_count = 0;
  std::string_view expr; // rest-of-line program text (kind=expression)
  bool has_expr = false;
  // SET:
  Param param = Param::kUnknown;
  double number = 0.0; // FREQ/AMP/OFFSET/PHASE/DUTY value (SI, finite)
  std::uint64_t seed = 0; // SEED value
  QuantityId set_quantity = QuantityId::kVoltage; // QUANTITY value
  // SELECT:
  std::uint64_t select_index = 0;
};

namespace detail {

// Strict u64: full-string from_chars, no signs, no whitespace, no empty.
[[nodiscard]] inline core::Result<std::uint64_t>
parse_u64(std::string_view s) noexcept {
  if (s.empty()) {
    return core::Result<std::uint64_t>::failure("expected integer");
  }
  std::uint64_t v = 0;
  const char *beg = s.data();
  const char *end = s.data() + s.size();
  const auto res = std::from_chars(beg, end, v);
  if (res.ec != std::errc{} || res.ptr != end) {
    return core::Result<std::uint64_t>::failure("invalid integer");
  }
  return core::Result<std::uint64_t>::success(v);
}

// Strict f64: full-string from_chars. from_chars accepts no inf/nan
// spellings, so success implies a finite value; overflow fails loudly.
[[nodiscard]] inline core::Result<double> parse_f64(std::string_view s) noexcept {
  if (s.empty()) {
    return core::Result<double>::failure("expected number");
  }
  double v = 0.0;
  const char *beg = s.data();
  const char *end = s.data() + s.size();
  const auto res = std::from_chars(beg, end, v);
  if (res.ec != std::errc{} || res.ptr != end || !std::isfinite(v)) {
    return core::Result<double>::failure("invalid number");
  }
  return core::Result<double>::success(v);
}

// Next space-separated token; `rest` advances past it. Quoting is
// deliberately unsupported: values are numbers, names, or the rest-of-line
// EXPR program — spaces only split before EXPR.
[[nodiscard]] inline std::string_view next_word(std::string_view &rest) noexcept {
  while (!rest.empty() && rest.front() == ' ') {
    rest.remove_prefix(1);
  }
  std::size_t i = 0;
  while (i < rest.size() && rest[i] != ' ') {
    ++i;
  }
  const std::string_view tok = rest.substr(0, i);
  rest.remove_prefix(i);
  while (!rest.empty() && rest.front() == ' ') {
    rest.remove_prefix(1);
  }
  return tok;
}

[[nodiscard]] inline std::string_view trim(std::string_view s) noexcept {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
    s.remove_prefix(1);
  }
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
    s.remove_suffix(1);
  }
  return s;
}

} // namespace detail

// Parse one control line. Views in the result borrow `line` (see Command);
// the bus drains from stored strings so they stay alive through apply.
// Failure is kInvalidArgument with a literal diagnostic; no partial state.
[[nodiscard]] inline core::Result<Command> parse_line(std::string_view line) {
  using R = core::Result<Command>;
  if (line.size() > kMaxLineBytes) {
    return R::failure("line too long");
  }
  std::string_view rest = detail::trim(line);
  if (rest.empty()) {
    return R::failure("empty command");
  }
  const std::string_view verb = detail::next_word(rest);
  Command cmd;
  if (verb == "PING") {
    if (!detail::trim(rest).empty()) {
      return R::failure("PING takes no arguments");
    }
    cmd.verb = Verb::kPing;
    return R::success(cmd);
  }
  if (verb == "HELLO") {
    const std::string_view ver = detail::next_word(rest);
    const auto v = detail::parse_u64(ver);
    if (!v.ok()) {
      return R::failure("HELLO wants <version> <client>");
    }
    cmd.verb = Verb::kHello;
    cmd.hello_version = (v.value > 0xFFFFFFFFu) ? 0xFFFFFFFFu
                                                : static_cast<std::uint32_t>(v.value);
    cmd.client = detail::trim(rest);
    return R::success(cmd);
  }
  if (verb == "LIST") {
    if (!detail::trim(rest).empty()) {
      return R::failure("LIST takes no arguments");
    }
    cmd.verb = Verb::kList;
    return R::success(cmd);
  }
  if (verb == "START" || verb == "STOP" || verb == "DROP") {
    const std::string_view id = detail::next_word(rest);
    if (!detail::trim(rest).empty()) {
      return R::failure("want <VERB> <serial>");
    }
    const auto s = detail::parse_u64(id);
    if (!s.ok()) {
      return R::failure("bad source serial");
    }
    cmd.verb = (verb == "START") ? Verb::kStart : (verb == "STOP" ? Verb::kStop : Verb::kDrop);
    cmd.serial = s.value;
    return R::success(cmd);
  }
  if (verb == "SELECT") {
    const std::string_view id = detail::next_word(rest);
    const std::string_view idx = detail::next_word(rest);
    if (!detail::trim(rest).empty()) {
      return R::failure("want SELECT <serial> <index>");
    }
    const auto s = detail::parse_u64(id);
    const auto k = detail::parse_u64(idx);
    if (!s.ok() || !k.ok()) {
      return R::failure("bad SELECT arguments");
    }
    cmd.verb = Verb::kSelect;
    cmd.serial = s.value;
    cmd.select_index = k.value;
    return R::success(cmd);
  }
  if (verb == "SET") {
    const std::string_view id = detail::next_word(rest);
    const std::string_view pname = detail::next_word(rest);
    const std::string_view pval = detail::next_word(rest);
    if (!detail::trim(rest).empty()) {
      return R::failure("want SET <serial> <PARAM> <value>");
    }
    const auto s = detail::parse_u64(id);
    const Param p = parse_param(pname);
    if (!s.ok()) {
      return R::failure("bad source serial");
    }
    if (p == Param::kUnknown) {
      return R::failure("unknown param (want FREQ|AMP|OFFSET|PHASE|DUTY|SEED|QUANTITY)");
    }
    if (pval.empty()) {
      return R::failure("want SET <serial> <PARAM> <value>");
    }
    cmd.verb = Verb::kSet;
    cmd.serial = s.value;
    cmd.param = p;
    if (p == Param::kSeed) {
      const auto n = detail::parse_u64(pval);
      if (!n.ok()) {
        return R::failure("bad SEED value");
      }
      cmd.seed = n.value;
    } else if (p == Param::kQuantity) {
      const auto q = parse_quantity(pval);
      if (!q.ok()) {
        return R::failure("unknown quantity (want U|I)");
      }
      cmd.set_quantity = q.value;
    } else {
      const auto n = detail::parse_f64(pval);
      if (!n.ok()) {
        return R::failure("bad numeric value");
      }
      cmd.number = n.value;
    }
    return R::success(cmd);
  }
  if (verb == "ADD") {
    const std::string_view kind_text = detail::next_word(rest);
    const std::string_view qty_text = detail::next_word(rest);
    const auto kind = parse_kind(kind_text);
    if (!kind.ok()) {
      return R::failure("unknown kind (want sine|square|triangle|saw|dc|pulse|noise|sum|"
                        "expression|table|lua)");
    }
    cmd.kind = kind.value;
    cmd.params.kind = kind.value;
    cmd.verb = Verb::kAdd;
    if (!qty_text.empty()) {
      const auto q = parse_quantity(qty_text);
      if (!q.ok()) {
        return R::failure("unknown quantity (want U|I)");
      }
      cmd.quantity = q.value;
      cmd.params.quantity = q.value;
    }
    if (cmd.kind == SignalKind::kTable || cmd.kind == SignalKind::kLua) {
      // Bulk payloads ride batch paths, never a text line (see header).
      // Parsed (so the shape error is precise) but flagged for the
      // not_implemented refusal at apply time.
      return R::success(cmd);
    }
    while (!detail::trim(rest).empty()) {
      const std::string_view key = detail::next_word(rest);
      if (key == "EXPR") {
        if (cmd.kind != SignalKind::kExpression) {
          return R::failure("EXPR needs kind=expression");
        }
        // Rest of the line IS the program (spaces allowed); must be last.
        cmd.expr = detail::trim(rest);
        if (cmd.expr.empty()) {
          return R::failure("EXPR needs program text");
        }
        cmd.has_expr = true;
        rest = {};
        break;
      }
      if (key == "OPS") {
        if (cmd.kind != SignalKind::kSum) {
          return R::failure("OPS needs kind=sum");
        }
        const std::string_view list = detail::next_word(rest);
        if (list.empty()) {
          return R::failure("OPS needs serials (want OPS s0,s1)");
        }
        // Comma-separated operand serials, 1..kMaxOps.
        std::string_view tail = list;
        cmd.op_count = 0;
        while (!tail.empty()) {
          const std::size_t comma = tail.find(',');
          const std::string_view item =
              (comma == std::string_view::npos) ? tail : tail.substr(0, comma);
          const auto n = detail::parse_u64(item);
          if (!n.ok() || cmd.op_count >= SignalParams::kMaxOps) {
            return R::failure("bad OPS serials");
          }
          cmd.ops[cmd.op_count++] = n.value;
          tail = (comma == std::string_view::npos) ? std::string_view{} : tail.substr(comma + 1);
        }
        if (cmd.op_count == 0) {
          return R::failure("OPS needs serials (want OPS s0,s1)");
        }
        continue;
      }
      const Param p = parse_param(key);
      if (p == Param::kUnknown || p == Param::kQuantity) {
        return R::failure("unknown ADD key (want FREQ|AMP|OFFSET|PHASE|DUTY|SEED|OPS|EXPR)");
      }
      const std::string_view val = detail::next_word(rest);
      if (p == Param::kSeed) {
        const auto n = detail::parse_u64(val);
        if (!n.ok()) {
          return R::failure("bad SEED value");
        }
        cmd.params.seed = n.value;
        continue;
      }
      const auto n = detail::parse_f64(val);
      if (!n.ok()) {
        return R::failure("bad numeric value");
      }
      switch (p) {
      case Param::kFreq:
        cmd.params.freq_hz = n.value;
        break;
      case Param::kAmp:
        cmd.params.amp = n.value;
        break;
      case Param::kOffset:
        cmd.params.offset = n.value;
        break;
      case Param::kPhase:
        cmd.params.phase_rad = n.value;
        break;
      case Param::kDuty:
        cmd.params.duty = n.value;
        break;
      case Param::kSeed: // handled above; unreachable
      case Param::kQuantity: // rejected above; unreachable
      case Param::kUnknown: // rejected above; unreachable
        return R::failure("unknown ADD key");
      }
    }
    if (cmd.kind == SignalKind::kSum && cmd.op_count == 0) {
      return R::failure("sum needs OPS serials");
    }
    if (cmd.kind == SignalKind::kExpression && !cmd.has_expr) {
      return R::failure("expression needs EXPR text");
    }
    return R::success(cmd);
  }
  return R::failure("unknown verb (want HELLO|PING|LIST|ADD|SET|START|STOP|DROP|SELECT)");
}

// --- Response builders (append one line, no trailing newline) ---

inline void ok_hello(std::uint64_t run_id, std::vector<std::string> &responses) {
  responses.push_back("OK hello 1 run=" + std::to_string(run_id));
}

inline void ok_pong(std::vector<std::string> &responses) {
  responses.emplace_back("OK pong");
}

inline void ok_list_begin(std::size_t n, std::vector<std::string> &responses) {
  responses.push_back("OK list " + std::to_string(n));
}

inline void ok_added(std::uint64_t serial, std::vector<std::string> &responses) {
  responses.push_back("OK added " + std::to_string(serial));
}

inline void ok_set(std::uint64_t serial, Param p, std::vector<std::string> &responses) {
  responses.push_back("OK set " + std::to_string(serial) + " " + std::string(param_name(p)));
}

inline void ok_started(std::uint64_t serial, std::vector<std::string> &responses) {
  responses.push_back("OK started " + std::to_string(serial));
}

inline void ok_stopped(std::uint64_t serial, std::vector<std::string> &responses) {
  responses.push_back("OK stopped " + std::to_string(serial));
}

inline void ok_dropped(std::uint64_t serial, std::vector<std::string> &responses) {
  responses.push_back("OK dropped " + std::to_string(serial));
}

inline void err_status(core::Status st, std::string_view what,
                       std::vector<std::string> &responses) {
  responses.push_back("ERR " + std::string(status_name(st.code)) + " " + std::string(what));
}

} // namespace wengine::signal::control
