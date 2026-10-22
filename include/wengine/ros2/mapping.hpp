#pragma once

// Snapshot/Frame -> ROS topic mapping (pure, ROS-free, header-only).
//
// Owns the *naming* half of the bridge: how a stable signal serial or a
// particle slot becomes a topic/field name, and which standard message each
// projection maps to. The rclcpp TU performs the actual publish; this header
// never includes ROS types so unit tests link wengine_core alone.
//
// Conventions (see docs/ros2.md):
//   * Serials, never slot indices, address signals (slots recycle, serials
//     don't — same rule as CSV sig_id and Frame channels `sig.<serial>.U`).
//   * Values cross in SI doubles + quantity tag (zero-cost hot path);
//     dimensional safety lives at parse/display edges (signal/types.hpp).
//   * sim_time (double, seconds) maps to the message header stamp; the ROS
//     wall clock is never the simulation clock.
//   * Message choice (resolved at the edge, not here):
//       grouped signals -> Float64MultiArray (values in slot order) +
//                          Int64MultiArray-ish serial layout, or a custom
//                          wengine_msgs/SignalBlock for serials+quantities.
//       per-signal topic -> Float64Stamped (one publisher per serial).
//       particles        -> PointCloud-ish / Float64MultiArray via
//                          frame_from_snapshot items (views stay on Frame).

#include "wengine/link/frame.hpp"
#include "wengine/signal/types.hpp"
#include "wengine/world/state.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace wengine::ros2 {

// ROS topic segments allow [A-Za-z0-9_/]; Frame channels contain dots
// (`sig.3.U`). Sanitized form replaces '.' with '/' so `sig.3.U` becomes
// `<stem>/sig/3/U`. Pure string transform, no allocation policy beyond the
// returned string (config edge only, never the sim hot path).
[[nodiscard]] inline std::string sanitize_channel(std::string_view channel) {
  std::string out;
  out.reserve(channel.size());
  for (char c : channel) {
    out.push_back(c == '.' ? '/' : c);
  }
  return out;
}

// Per-signal topic for one serial: `<stem>/sig/<serial>`.
// Serial-addressed, so despawn/respawn never reuses a live subscription.
[[nodiscard]] inline std::string topic_for_serial(std::string_view stem, std::uint64_t serial) {
  std::string out(stem);
  out += "/sig/";
  out += std::to_string(serial);
  return out;
}

// Full topic for one Frame channel under a stem.
[[nodiscard]] inline std::string topic_for_channel(std::string_view stem, std::string_view channel) {
  std::string out(stem);
  out += '/';
  out += sanitize_channel(channel);
  return out;
}

// Quantity suffix for display/unit metadata (from the single quantity
// table — no ad-hoc switches here).
[[nodiscard]] inline std::string_view unit_suffix(signal::QuantityId q) noexcept {
  return signal::quantity_suffix(q);
}

// Grouped payload: dense slot-order serials + values + quantity bytes,
// mirroring world::Snapshot layout. The edge TU copies this into the
// message (Float64MultiArray or custom SignalBlock); the Snapshot ref is
// never retained (see emit::ISink lifetime contract).
struct SignalBlock {
  std::uint64_t step = 0;
  double sim_time = 0.0;
  std::vector<std::uint64_t> serial;
  std::vector<double> value;
  std::vector<std::uint8_t> quantity;
};

[[nodiscard]] inline SignalBlock block_from_snapshot(const world::Snapshot &s) {
  SignalBlock b;
  b.step = s.step_index;
  b.sim_time = s.sim_time;
  b.serial.assign(s.sig_serial.begin(), s.sig_serial.end());
  b.value.assign(s.sig_value.begin(), s.sig_value.end());
  b.quantity.assign(s.sig_quantity.begin(), s.sig_quantity.end());
  return b;
}

[[nodiscard]] inline SignalBlock block_from_frame_channels(const link::Frame &f) {
  // Grouped mode carries channels only; items (particles) route to the
  // particle topic via frame_from_snapshot. Channels here are informational.
  SignalBlock b;
  b.step = f.step;
  b.sim_time = f.time;
  b.value.reserve(f.channels.size());
  for (const auto &ch : f.channels) {
    b.value.push_back(ch.value);
  }
  return b;
}

} // namespace wengine::ros2
