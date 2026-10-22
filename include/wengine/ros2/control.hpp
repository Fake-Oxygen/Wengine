#pragma once

// ROS2 service -> signal control-plane mapping (ROS-free vocabulary).
//
// The engine control vocabulary lives in signal/control.hpp (strict text
// lines: HELLO/PING/LIST/ADD/SET/START/STOP/DROP + OK/ERR responses) and in
// solvers/signal_control.hpp (step-boundary drain into world::State). This
// header maps each verb to a ROS service name + typed field sketch so the
// rclcpp TU can expose services without inventing a second command language.
//
// Rules (same as TCP/UART transports):
//   * Services enqueue parsed commands; nothing mutates state at service
//     time. The driver drains the queue at the step boundary in slot order,
//     then steps exactly once — service timing never perturbs dt/order.
//   * Transports carry SI doubles; unit suffixes (`3.3V`) are parsed at the
//     CLI/GUI edge via cli::parse_quantity_value, never here.
//   * Table/Lua payloads do not fit a service call: ADD rejects them as
//     not_implemented (batch `sig run --sig-table/--sig-lua` owns those).
//   * Responses mirror the text protocol status: ok + serial/message, or
//     err code + message (codes mirror core::Status, never throws).

#include "wengine/signal/control.hpp"

#include <string_view>

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

} // namespace wengine::ros2::control
