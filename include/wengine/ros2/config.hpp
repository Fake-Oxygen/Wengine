#pragma once

// ROS2 bridge configuration (engine leaf, ROS-free).
//
// This header owns the ROS2-facing *data* contract and nothing else: node /
// topic stems, QoS classes, and decimation policy. It never includes rclcpp
// (or any ROS header) and performs no I/O — the transport TU under
// src/ros2/ (or examples/ros2_bridge.cpp, built only with BUILD_ROS2=ON)
// owns the rmw calls. Same layering as link/* (headers platform-free, TUs
// own syscalls) and signal/control.hpp (engine owns vocabulary, caller owns
// transport + step-boundary drain).
//
// Delivery mapping (see docs/delivery.md, docs/ros2.md):
//   * latest-state viewing  -> SENSOR_DATA-ish (best-effort, keep-last 1,
//     transient-local off): writer drops oldest, reader takes latest.
//   * ordered sample stream -> RELIABLE-ish (bounded queue): backpressure is
//     counted, never a zero-loss promise to arbitrarily slow receivers.
//   * complete recording    -> stays in the CSV/block recorder (driver-paced,
//     never a ROS topic promise).

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace wengine::ros2 {

// QoS intent, resolved to an rmw profile at the edge (rclcpp TU only).
enum class QosClass : std::uint8_t {
  kLatestState, // live viewing: best-effort, depth 1, drops oldest
  kSampleStream, // telemetry streaming: reliable, bounded, drops counted
};

struct BridgeConfig {
  std::string_view node_name = "wengine";
  std::string_view ns = "wengine";
  // Topic stems (per-source topics append the serial, see mapping.hpp).
  std::string_view signal_topic_stem = "signals";
  std::string_view particle_topic_stem = "particles";
  QosClass qos = QosClass::kLatestState;
  // Publish every Nth emitted snapshot (>= 1). This is the emit-side
  // StrideSink budget, independent of sim dt and of the ROS wall rate:
  // sim dt advances physics, emit_every/stride decimates samples, rmw
  // pacing sets wall packets, RViz sets refresh. Never conflate them.
  std::uint64_t stride = 1;
  // Group all live signals onto one topic (Float64MultiArray-style, dense
  // slot order) when true; one topic per serial otherwise. Default grouped:
  // fewer publishers, stable subscription set across despawn/respawn.
  bool grouped_signals = true;
  // Optional serial allowlist for SignalSubsetSink wiring at the edge.
  // Empty (default) publishes every live source; non-empty forwards only
  // the listed serials (ticks still flow every step so lag accounting
  // stays exact — only rows are filtered, never ticks).
  std::vector<std::uint64_t> select_serials{};
};

[[nodiscard]] inline constexpr bool is_valid_config(const BridgeConfig &c) noexcept {
  return !c.node_name.empty() && !c.signal_topic_stem.empty() && !c.particle_topic_stem.empty() && c.stride >= 1;
}

// Nominal (unbounded-checked) publisher count for grouped vs per-signal
// mode: grouped needs 2 (signals + particles), per-signal needs
// signal_count + 1. Callers size rmw resources from this; the sim never
// blocks when the count is exceeded — over-bound frames drop loudly.
[[nodiscard]] inline constexpr std::size_t publisher_count(std::size_t signal_count, bool grouped) noexcept {
  return grouped ? 2 : signal_count + 1;
}

} // namespace wengine::ros2
