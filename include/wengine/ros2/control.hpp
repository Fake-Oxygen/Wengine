#pragma once

// ROS2 service -> signal control-plane mapping (ROS-free vocabulary).
//
// The engine control vocabulary lives in signal/control.hpp (strict text
// lines: HELLO/PING/LIST/ADD/SET/START/STOP/DROP + OK/ERR responses) and in
// solvers/signal_control.hpp (step-boundary ControlBus drain into
// world::State). This header maps each verb to a ROS service contract so
// the rclcpp TU exposes services without inventing a second command
// language: one uniform wengine_msgs/SigCommand envelope (args in,
// ok/message/code out), verb fixed by the service name.
//
// Rules (same as TCP/UART transports):
//   * Service callbacks enqueue only: validate the shape here (same
//     parser, SI doubles, table/Lua refused as not_implemented), enqueue
//     the full line on the bus, respond the ENQUEUE verdict immediately
//     ("OK queued" / "ERR <code> <detail>" + numeric StatusCode).
//     Nothing mutates state at service time; the driver drains the bus
//     at the step boundary in slot order, then steps exactly once —
//     service timing never perturbs dt/order. Execution results surface
//     in the driver log + the next LIST census, never in a stale response.
//   * sig_list serves the driver-refreshed census cache (stale by <1 step
//     by construction), never the bus: the bus answer would arrive a step
//     late and mix with already-answered outboxes.
//   * Transports carry SI doubles; unit suffixes (`3.3V`) are parsed at the
//     CLI/GUI edge via cli::parse_quantity_value, never here.
//   * Responses mirror the text protocol status: ok + serial/message, or
//     err code + message (codes mirror core::Status, never throws).

#include "wengine/core/status.hpp"
#include "wengine/signal/control.hpp"
#include "wengine/world/state.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace wengine::ros2::control {

// Service names relative to the node namespace (e.g. `/wengine/sig_add`).
inline constexpr std::string_view kServiceAdd = "sig_add";
inline constexpr std::string_view kServiceSet = "sig_set";
inline constexpr std::string_view kServiceStart = "sig_start";
inline constexpr std::string_view kServiceStop = "sig_stop";
inline constexpr std::string_view kServiceDrop = "sig_drop";
inline constexpr std::string_view kServiceList = "sig_list";

// Verb for each service (ADD/SET/START/STOP/DROP/LIST subset; HELLO/PING
// stay transport-handshake/liveness on the subscription side).
[[nodiscard]] inline constexpr signal::control::Verb verb_for(std::string_view service) noexcept {
  using V = signal::control::Verb;
  if (service == kServiceAdd) {
    return V::kAdd;
  }
  if (service == kServiceSet) {
    return V::kSet;
  }
  if (service == kServiceStart) {
    return V::kStart;
  }
  if (service == kServiceStop) {
    return V::kStop;
  }
  if (service == kServiceDrop) {
    return V::kDrop;
  }
  return V::kList;
}

// Uppercase verb text for a service (the line prefix).
[[nodiscard]] inline constexpr std::string_view verb_text(std::string_view service) noexcept {
  if (service == kServiceAdd) {
    return "ADD";
  }
  if (service == kServiceSet) {
    return "SET";
  }
  if (service == kServiceStart) {
    return "START";
  }
  if (service == kServiceStop) {
    return "STOP";
  }
  if (service == kServiceDrop) {
    return "DROP";
  }
  return "LIST";
}

// Absolute service name (node spins in the root namespace).
[[nodiscard]] inline std::string service_name(std::string_view ns, std::string_view service) {
  std::string out("/");
  out += std::string(ns);
  out += '/';
  out += std::string(service);
  return out;
}

// Service outcome: mirrors the protocol OK/ERR vocabulary plus the
// numeric core::StatusCode (the SigCommand response fields).
struct ServiceOutcome {
  bool ok = false;
  std::string message;
  std::int32_t code = 0;
};

[[nodiscard]] inline ServiceOutcome outcome_enqueued() {
  return {true, "OK queued", 0};
}

[[nodiscard]] inline ServiceOutcome outcome_error(core::Status st, std::string_view what) {
  ServiceOutcome o;
  o.ok = false;
  o.message = "ERR " + std::string(signal::control::status_name(st.code)) + " " + std::string(what);
  o.code = static_cast<std::int32_t>(st.code);
  return o;
}

// Full bus line for a service call: "<VERB> <args>", or bare "LIST".
// Pure builder — shape validation is validate_service_line below.
[[nodiscard]] inline std::string request_line(std::string_view service, std::string_view args) {
  const std::string_view verb = verb_text(service);
  if (verb == "LIST") {
    return "LIST";
  }
  std::string line(verb);
  line += ' ';
  line += std::string(args);
  return line;
}

// Validate a service call the way the bus will see it: same strict parser
// as TCP/UART, verb pinned by the service name, table/Lua refused as
// not_implemented (bulk payloads ride batch paths). On kOk `line` holds
// the full line to enqueue; anything else is the immediate ERR verdict
// (pool-against checks like unknown serials still happen at drain time,
// like every other transport).
[[nodiscard]] inline core::Status validate_service_line(std::string_view service, std::string_view args, std::string &line) {
  namespace sc = signal::control;
  if (service == kServiceList && !args.empty()) {
    return {core::StatusCode::kInvalidArgument};
  }
  line = request_line(service, args);
  if (line.size() > sc::kMaxLineBytes) {
    return {core::StatusCode::kInvalidArgument};
  }
  const auto parsed = sc::parse_line(line);
  if (!parsed.ok()) {
    return {core::StatusCode::kInvalidArgument};
  }
  if (parsed.value.verb != verb_for(service)) {
    return {core::StatusCode::kInvalidArgument};
  }
  if (parsed.value.verb == sc::Verb::kAdd && (parsed.value.kind == signal::SignalKind::kTable || parsed.value.kind == signal::SignalKind::kLua)) {
    return {core::StatusCode::kNotImplemented};
  }
  return core::Status::success();
}

// Step-boundary census served by sig_list: refreshed by the driver after
// every drain (stale by <1 step by construction), formatted by the single
// shared SRC formatter — byte-identical to a bus LIST answer by test.
struct CensusCache {
  std::uint64_t step = 0;
  double sim_time = 0.0;
  std::vector<std::string> lines; // "OK list <n>" + "SRC ..." rows
};

inline void refresh_census(const world::State &s, std::uint64_t step, CensusCache &cache) {
  namespace sc = signal::control;
  cache.step = step;
  cache.sim_time = s.sim_time;
  cache.lines.clear();
  std::size_t n = 0;
  for (std::uint32_t i = 0; i < s.signals.capacity(); ++i) {
    n += (s.signals.has(i) ? 1 : 0);
  }
  cache.lines.reserve(n + 1);
  sc::ok_list_begin(n, cache.lines);
  for (std::uint32_t i = 0; i < s.signals.capacity(); ++i) {
    if (!s.signals.has(i)) {
      continue;
    }
    const std::size_t kind_idx = s.signals.kind[i];
    const std::string_view kind_name = (kind_idx < signal::kKindNames.size()) ? signal::kKindNames[kind_idx] : "?";
    const auto qty = static_cast<signal::QuantityId>(s.signals.quantity[i]);
    cache.lines.push_back(sc::format_src_row(
        s.signals.serial[i], kind_name, signal::quantity_suffix(qty), s.signals.freq[i],
        s.signals.amp[i], s.signals.offset[i], s.signals.phase[i], s.signals.duty[i],
        s.signals.seed[i], s.signals.running[i] != 0));
  }
}

// Census response text: protocol-exact lines joined for the single-string
// SigCommand message field.
[[nodiscard]] inline std::string census_text(const CensusCache &cache) {
  std::string out;
  for (std::size_t k = 0; k < cache.lines.size(); ++k) {
    if (k != 0) {
      out += '\n';
    }
    out += cache.lines[k];
  }
  return out;
}

} // namespace wengine::ros2::control
