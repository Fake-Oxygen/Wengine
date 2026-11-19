#pragma once

/**
 * @file
 * ROS2 service to signal control-plane mapping (transport-neutral,
 * ROS-free vocabulary). The engine control vocabulary lives in
 * signal/control.hpp (strict HELLO/PING/LIST/ADD/SET/START/STOP/DROP
 * text lines plus OK/ERR responses) and in solvers/signal_control.hpp
 * (step-boundary ControlBus drain into world::State); this header maps
 * each verb onto one uniform wengine_msgs/SigCommand service contract
 * (arguments in, ok/message/code out, verb fixed by the service name) so
 * the rclcpp TU exposes the services without inventing a second command
 * language. Service callbacks enqueue only (same strict parser and SI
 * doubles as the TCP/UART servers; table/Lua refused as not_implemented,
 * bulk payloads ride batch paths) and answer the enqueue verdict
 * immediately; the driver drains the bus at the step boundary in slot
 * order, then steps exactly once - service timing never perturbs
 * dt or ordering, and execution results surface in the driver log plus
 * the next LIST census, never in a stale response. Transports carry SI
 * doubles; unit suffixes parse at the CLI/GUI edge via
 * cli::parse_quantity_value, never here. sig_list serves the
 * driver-refreshed census cache (stale by less than one step by
 * construction), never the bus. Consumers: the RclcppTransport TU, the
 * RosControlService TU, and the `--control-ros` batch path. Compiles and
 * unit-tests with no ROS install (needs only the engine headers).
 */

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
/**
 * @brief Service name for the ADD verb (relative to the namespace).
 */
inline constexpr std::string_view kServiceAdd = "sig_add";
/**
 * @brief Service name for the SET verb (relative to the namespace).
 */
inline constexpr std::string_view kServiceSet = "sig_set";
/**
 * @brief Service name for the START verb (relative to the namespace).
 */
inline constexpr std::string_view kServiceStart = "sig_start";
/**
 * @brief Service name for the STOP verb (relative to the namespace).
 */
inline constexpr std::string_view kServiceStop = "sig_stop";
/**
 * @brief Service name for the DROP verb (relative to the namespace).
 */
inline constexpr std::string_view kServiceDrop = "sig_drop";
/**
 * @brief Service name for the LIST verb (relative to the namespace).
 */
inline constexpr std::string_view kServiceList = "sig_list";

// Verb for each service (ADD/SET/START/STOP/DROP/LIST subset; HELLO/PING
// stay transport-handshake/liveness on the subscription side).
/**
 * @brief Control verb pinned by a service name.
 * @param[in] service Service name (one of the kService* constants).
 * @return The ADD/SET/START/STOP/DROP verb, or kList for anything else
 *         (unknown names fall through to LIST validation, which then
 *         refuses them loudly).
 * @note HELLO/PING stay transport-handshake/liveness on the
 *       subscription side; services expose the six verbs only.
 */
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
/**
 * @brief Uppercase verb text for a service (the bus line prefix).
 * @param[in] service Service name (one of the kService* constants).
 * @return `ADD`/`SET`/`START`/`STOP`/`DROP`, or `LIST` for anything else.
 */
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
/**
 * @brief Absolute service name under the root namespace.
 * @param[in] ns Topic namespace (relative form, validated by the CLI).
 * @param[in] service Service name (one of the kService* constants).
 * @return `/` plus ns plus `/` plus service (the node spins in the root
 *         namespace, so ros2 service list shows absolute names verbatim).
 * @note Pure string join; allocates the returned string.
 */
[[nodiscard]] inline std::string service_name(std::string_view ns, std::string_view service) {
  std::string out("/");
  out += std::string(ns);
  out += '/';
  out += std::string(service);
  return out;
}

// Service outcome: mirrors the protocol OK/ERR vocabulary plus the
// numeric core::StatusCode (the SigCommand response fields).
/**
 * @brief Service outcome: the protocol OK/ERR vocabulary plus the numeric
 *        core::StatusCode carried in the SigCommand response fields.
 */
struct ServiceOutcome {
  /**
   * @brief True for the enqueue verdict (`OK queued`), false for refusal.
   */
  bool ok = false;
  /**
   * @brief Response text (`OK queued` or `ERR` plus code plus detail).
   */
  std::string message;
  /**
   * @brief Numeric status code (mirrors core::StatusCode; 0 on success).
   */
  std::int32_t code = 0;
};

/**
 * @brief Successful enqueue verdict for an accepted service call.
 * @return Outcome with ok true, message `OK queued`, code 0.
 * @note Execution still happens at the next step-boundary drain; this
 *       verdict only confirms the line reached the bus.
 */
[[nodiscard]] inline ServiceOutcome outcome_enqueued() {
  return {true, "OK queued", 0};
}

/**
 * @brief Refusal verdict for a rejected service call.
 * @param[in] st Status naming the failure (its code and protocol name
 *            are echoed in the message).
 * @param[in] what One-line detail appended after the status name.
 * @return Outcome with ok false, message `ERR` plus code plus detail,
 *         and the numeric status code.
 */
[[nodiscard]] inline ServiceOutcome outcome_error(core::Status st, std::string_view what) {
  ServiceOutcome o;
  o.ok = false;
  o.message = "ERR " + std::string(signal::control::status_name(st.code)) + " " + std::string(what);
  o.code = static_cast<std::int32_t>(st.code);
  return o;
}

// Full bus line for a service call: "<VERB> <args>", or bare "LIST".
// Pure builder — shape validation is validate_service_line below.
/**
 * @brief Build the full bus line for a service call (pure builder).
 * @param[in] service Service name fixing the verb.
 * @param[in] args Argument text after the verb (bare `LIST` ignores it).
 * @return The verb plus space plus args, or bare `LIST`.
 * @note No validation happens here; validate_service_line checks the
 *       shape the way the bus will see it.
 */
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
/**
 * @brief Validate a service call the way the bus will see it.
 * @param[in] service Service name fixing the verb.
 * @param[in] args Argument text after the verb.
 * @param[out] line The full line to enqueue on success.
 * @return kOk with line holding the full line to enqueue; kInvalidArgument
 *         for a non-empty LIST body, an over-long line, or a strict-parse
 *         failure; kNotImplemented for table/Lua ADD (bulk payloads ride
 *         batch paths). Pool-against checks like unknown serials still
 *         happen at drain time, like every other transport.
 * @note Transports carry SI doubles; unit suffixes parse at the CLI/GUI
 *       edge, never here.
 */
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
/**
 * @brief Step-boundary census served by sig_list.
 * @note Refreshed by the driver after every drain (stale by less than
 *       one step by construction) and formatted by the single shared SRC
 *       formatter - byte-identical to a bus LIST answer by test. Served
 *       from this cache, never from the bus: the bus answer would arrive
 *       a step late and mix with already-answered outboxes.
 */
struct CensusCache {
  /**
   * @brief Step index the cached rows were refreshed at.
   */
  std::uint64_t step = 0;
  /**
   * @brief Engine sim time in seconds (SI) at the refresh step.
   */
  double sim_time = 0.0;
  /**
   * @brief Census rows: `OK list` plus one `SRC` row per live source.
   */
  std::vector<std::string> lines; // "OK list <n>" + "SRC ..." rows
};

/**
 * @brief Refresh the census cache from live state after a drain.
 * @param[in] s Live world state to read (slot order; never retained).
 * @param[in] step Step index stamping this refresh.
 * @param[out] cache Cache overwritten with the fresh census.
 * @note Read-only over state; the driver calls it once per step-boundary
 *       drain while Active.
 */
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
/**
 * @brief Join cached census rows into the single-string response field.
 * @param[in] cache Census cache refreshed by refresh_census.
 * @return Protocol-exact rows joined by newline (no trailing newline).
 * @note Pure join; the byte-identity with a bus LIST answer comes from
 *       the shared SRC formatter used at refresh time.
 */
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
