#pragma once

/**
 * @file
 * Snapshot/Frame to ROS topic mapping: the naming half of the bridge
 * (pure, ROS-free, header-only). Owns how a stable signal serial or a
 * particle slot becomes a topic/field name and which standard message
 * each projection targets; the rclcpp TU performs the actual publish.
 * Never includes ROS types, so unit tests link wengine_core alone with
 * no ROS install. Dependency direction: ros -> link/world/emit, never
 * the reverse (AGENTS.md section 4). Consumers: ros2/publisher.hpp
 * (topic construction, block payload), ros2/rclcpp_transport.hpp (rmw
 * publish), and test_ros2_mapping/test_ros2_publisher.
 */

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
//     Topic segments must not start with a digit (rmw rule), so per-signal
//     topics are `<stem>/sig_<serial>` and channel-derived topics take an
//     `s` prefix on digit-leading segments (see sanitize_channel).
//   * Values cross in SI doubles + quantity tag (zero-cost hot path);
//     dimensional safety lives at parse/display edges (signal/types.hpp).
//   * sim_time (double, seconds) maps to the message header stamp; the ROS
//     wall clock is never the simulation clock.
//   * Message choice: grouped signals ride BOTH the RViz-friendly
//     sensor_msgs/JointState on `<ns>/<signal_stem>` AND the exact
//     wengine_msgs/SignalBlock on `<ns>/<signal_stem>/block` (step +
//     sim_time + serial/value/quantity arrays, see msg/SignalBlock.msg);
//     per-serial mode keeps single-entry JointStates and publishes no
//     block. The edge TU resolves the choice, never here.
//       grouped signals -> sensor_msgs/JointState (header.stamp = sim_time,
//                          name `sig.<serial>.U`, position SI value).
//       per-signal topic -> single-entry JointState on the serial topic.
//       particles        -> sensor_msgs/PointCloud2 x/y/vx/vy FLOAT64 via
//                          frame_from_snapshot items (views stay on Frame).

#include "wengine/link/frame.hpp"
#include "wengine/ros2/config.hpp"
#include "wengine/signal/types.hpp"
#include "wengine/world/state.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace wengine::ros2 {

// ROS topic segments allow [A-Za-z0-9_/] but must not start with a digit
// (rmw refuses `sig/3` — "topic name token must not start with a number").
// Frame channels contain digits (`sig.3.U`), so the sanitized form
// replaces '.' with '/' AND prefixes digit-leading segments with 's':
// `sig.3.U` becomes `<stem>/sig/s3/U`, `p0.x` stays `<stem>/p0/x`.
// Pure string transform, config edge only, never the sim hot path.
/**
 * @brief Turn one Frame channel name into a legal ROS topic suffix.
 * @param[in] channel Frame channel name, e.g. `sig.3.U` or `p0.x`.
 * @return Topic suffix with `.` replaced by `/` and digit-leading
 *         segments prefixed `s` (rmw refuses digit-leading segments):
 *         `sig.3.U` -> `sig/s3/U`, `p0.x` -> `p0/x`.
 * @note Pure string transform at the config edge; never the sim hot
 *       path. Message field names keep the plain `sig.<serial>.U` form -
 *       only topic segments are sanitized.
 */
[[nodiscard]] inline std::string sanitize_channel(std::string_view channel) {
  std::string out;
  out.reserve(channel.size() + 4);
  bool segment_start = true;
  for (char c : channel) {
    if (c == '.') {
      out.push_back('/');
      segment_start = true;
      continue;
    }
    if (segment_start && c >= '0' && c <= '9') {
      out.push_back('s');
    }
    segment_start = false;
    out.push_back(c);
  }
  return out;
}

// Per-signal topic for one serial: `<stem>/sig_<serial>` (flat, valid by
// construction — see the digit rule above). Serial-addressed, so
// despawn/respawn never reuses a live subscription.
/**
 * @brief Per-signal topic name for one stable serial.
 * @param[in] stem Config signal stem (validated by the caller).
 * @param[in] serial Stable source serial (never reused per State).
 * @return `<stem>/sig_<serial>` (valid by construction: the flat form
 *         avoids digit-leading segments).
 * @note Serial-addressed (D-ROS-2): slots recycle, serials do not, so a
 *       despawn/respawn never reuses a live subscription.
 */
[[nodiscard]] inline std::string topic_for_serial(std::string_view stem, std::uint64_t serial) {
  std::string out(stem);
  out += "/sig_";
  out += std::to_string(serial);
  return out;
}

// Full topic for one Frame channel under a stem.
/**
 * @brief Full topic for one Frame channel under a stem.
 * @param[in] stem Config stem (namespace-relative).
 * @param[in] channel Frame channel name (see sanitize_channel).
 * @return `<stem>/<sanitized channel>`.
 */
[[nodiscard]] inline std::string topic_for_channel(std::string_view stem, std::string_view channel) {
  std::string out(stem);
  out += '/';
  out += sanitize_channel(channel);
  return out;
}

// Exact grouped record topic: the JointState on the signal stem stays the
// RViz-friendly view; the SignalBlock on this sub-topic carries the same
// payload with serial + quantity code arrays (see msg/SignalBlock.msg).
// Grouped mode only — per-serial mode publishes single-entry JointStates
// and no block.
/**
 * @brief Absolute topic of the exact grouped record (SignalBlock).
 * @param[in] c Bridge config (namespace + signal stem).
 * @return `/<ns>/<signal_topic_stem>/block`.
 * @note Grouped mode only: the JointState on the stem stays the
 *       RViz-friendly view; this sub-topic carries the exact record
 *       (step + sim_time + serial/value/quantity arrays). Per-serial
 *       mode publishes no block.
 */
[[nodiscard]] inline std::string signals_block_topic(const BridgeConfig &c) {
  std::string out("/");
  out += std::string(c.ns);
  out += '/';
  out += std::string(c.signal_topic_stem);
  out += "/block";
  return out;
}

// Quantity suffix for display/unit metadata (from the single quantity
// table — no ad-hoc switches here).
/**
 * @brief Quantity suffix (unit letter) for display metadata.
 * @param[in] q Quantity id from the signal table.
 * @return Suffix spelling from the single quantity table
 *         (signal/types.hpp kQuantities - no ad-hoc switches here).
 */
[[nodiscard]] inline std::string_view unit_suffix(signal::QuantityId q) noexcept {
  return signal::quantity_suffix(q);
}

// Grouped payload: dense slot-order serials + values + quantity bytes,
// mirroring world::Snapshot layout. The edge TU copies this into the
// message (Float64MultiArray or custom SignalBlock); the Snapshot ref is
// never retained (see emit::ISink lifetime contract).
/**
 * @brief Grouped payload for one emitted tick (dense slot order).
 * @note The edge TU copies this into the ROS message and never retains
 *       the Snapshot reference (emit::ISink lifetime contract); the
 *       publisher reuses its staging buffers across emissions.
 */
struct SignalBlock {
  /**
   * @brief Engine step index of the emitted tick.
   */
  std::uint64_t step = 0;
  /**
   * @brief Engine simulation time in seconds (SI) - never wall time
   *        (D-ROS-7).
   */
  double sim_time = 0.0;
  /**
   * @brief Stable source serials, dense slot order (mirrors Snapshot).
   */
  std::vector<std::uint64_t> serial;
  /**
   * @brief SI values (volts/amps/etc. per the quantity byte) in the same
   *        order as serial.
   */
  std::vector<double> value;
  /**
   * @brief Quantity codes mirroring signal::QuantityId, same order.
   */
  std::vector<std::uint8_t> quantity;
};

/**
 * @brief Copy the grouped signal arrays out of a snapshot.
 * @param[in] s Snapshot taken by the Runner (dense slot order).
 * @return Owned SignalBlock with step, sim_time (seconds), and the
 *         serial/value/quantity arrays copied out.
 * @note Pure copy: the caller owns the result; no Snapshot reference is
 *       retained.
 */
[[nodiscard]] inline SignalBlock block_from_snapshot(const world::Snapshot &s) {
  SignalBlock b;
  b.step = s.step_index;
  b.sim_time = s.sim_time;
  b.serial.assign(s.sig_serial.begin(), s.sig_serial.end());
  b.value.assign(s.sig_value.begin(), s.sig_value.end());
  b.quantity.assign(s.sig_quantity.begin(), s.sig_quantity.end());
  return b;
}

/**
 * @brief Informational block from Frame channels (grouped mode).
 * @param[in] f Frame view-model (channels carry name + SI value).
 * @return SignalBlock with step, sim_time (seconds), and channel values;
 *         serial/quantity stay empty - channels here are informational.
 * @note Grouped mode carries channels only; particle items route to the
 *       particle topic via frame_from_snapshot.
 */
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
