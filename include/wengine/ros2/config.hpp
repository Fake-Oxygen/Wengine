#pragma once

/**
 * @file
 * ROS2 bridge data contract (engine leaf, ROS-free by construction):
 * node/topic identity, QoS classes, and decimation policy - the
 * validated values every ROS-facing consumer shares. It never includes
 * rclcpp (or any ROS header) and performs no I/O; rmw calls live in the
 * transport TU under src/ros2/ (or examples/ros2_bridge.cpp, built only
 * with BUILD_ROS2=ON) - same layering as the link module (platform-free
 * headers, platform TUs owning syscalls) and signal/control.hpp (engine
 * owns
 * vocabulary, caller owns transport + step-boundary drain). Consumers:
 * ros2/cli_options.hpp, ros2/mapping.hpp, ros2/publisher.hpp, the
 * rclcpp TUs, and the CLI `sim run`/`sig run` batch paths. Unit tests
 * cover this header with wengine_core alone - nothing here needs a ROS
 * install. Delivery classes follow docs/delivery.md and docs/ros2.md:
 * complete recording never rides a topic (class C stays in the
 * CSV/block recorder).
 */

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
/**
 * @brief QoS intent for one ROS output, resolved to an rmw profile only
 *        in the rclcpp TU (never in ROS-free headers).
 * @note Two pinned delivery classes (docs/ros2.md QoS matrix): no
 *       per-topic tuning, volatile durability on both (late joiners
 *       miss earlier ticks by design).
 */
enum class QosClass : std::uint8_t {
  /**
   * @brief Latest-state viewing (delivery class A).
   */
  kLatestState, // live viewing: best-effort, depth 1, drops oldest
  /**
   * @brief Ordered bounded sample streaming (delivery class B).
   */
  kSampleStream, // telemetry streaming: reliable, bounded, drops counted
};

/**
 * @brief Validated identity + delivery policy for one bridge process.
 * @note All views point at caller-owned storage (CLI argv / string
 *       literals); the bridge copies nothing - keep the source alive
 *       for the process lifetime. Sinks built from this config are
 *       owned at the app edge, never in RunConfig (same rule as every
 *       other emit transport).
 */
struct BridgeConfig {
  /**
   * @brief Node name registered with the rmw layer (default `wengine`).
   */
  std::string_view node_name = "wengine";
  /**
   * @brief Topic namespace, relative path form (default `wengine`); the
   *        bridge prepends exactly one leading `/`.
   */
  std::string_view ns = "wengine";
  // Topic stems (per-source topics append the serial, see mapping.hpp).
  /**
   * @brief Topic stem for signal outputs (default `signals`).
   */
  std::string_view signal_topic_stem = "signals";
  /**
   * @brief Topic stem for particle outputs (default `particles`).
   */
  std::string_view particle_topic_stem = "particles";
  /**
   * @brief QoS delivery class for every topic this bridge publishes.
   */
  QosClass qos = QosClass::kLatestState;
  // Publish every Nth emitted snapshot (>= 1). This is the emit-side
  // StrideSink budget, independent of sim dt and of the ROS wall rate:
  // sim dt advances physics, emit_every/stride decimates samples, rmw
  // pacing sets wall packets, RViz sets refresh. Never conflate them.
  /**
   * @brief Per-output decimation: publish every Nth emitted snapshot.
   * @note One of four independent clocks (docs/ros2.md): sim dt
   *       advances physics, emit_every/stride decimates samples, rmw
   *       pacing sets wall packets, RViz sets UI refresh - never
   *       conflate them.
   */
  std::uint64_t stride = 1;
  // Group all live signals onto one topic (Float64MultiArray-style, dense
  // slot order) when true; one topic per serial otherwise. Default grouped:
  // fewer publishers, stable subscription set across despawn/respawn.
  /**
   * @brief Topic layout: grouped (one topic for all signals) vs
   *        per-serial (one topic per live source).
   * @note Grouped (default) keeps the subscription set stable across
   *       despawn/respawn; per-serial retains publishers per stable
   *       serial so a respawn never reuses a live subscription.
   */
  bool grouped_signals = true;
  // Optional serial allowlist for SignalSubsetSink wiring at the edge.
  // Empty (default) publishes every live source; non-empty forwards only
  // the listed serials (ticks still flow every step so lag accounting
  // stays exact — only rows are filtered, never ticks).
  /**
   * @brief Optional serial allowlist for edge-side SignalSubsetSink
   *        wiring (empty = every live source).
   * @note Rows are filtered, never ticks: every emitted tick still
   *       forwards (possibly with zero rows) so reader lag accounting
   *       stays exact. Serials are stable per State - slots recycle,
   *       serials do not.
   */
  std::vector<std::uint64_t> select_serials{};
};

/**
 * @brief Check the minimal shape invariants of a bridge config.
 * @param[in] c Config to validate.
 * @return True when names/stems are non-empty and stride >= 1.
 * @note Segment/path character rules live in
 *       ros2/cli_options.hpp (is_valid_ros_*); this checks only the
 *       structural minimum every transport needs.
 */
[[nodiscard]] inline constexpr bool is_valid_config(const BridgeConfig &c) noexcept {
  return !c.node_name.empty() && !c.signal_topic_stem.empty() && !c.particle_topic_stem.empty() && c.stride >= 1;
}

// Nominal (unbounded-checked) publisher count for grouped vs per-signal
// mode: grouped needs 2 (signals + particles), per-signal needs
// signal_count + 1. Callers size rmw resources from this; the sim never
// blocks when the count is exceeded — over-bound frames drop loudly.
/**
 * @brief Nominal publisher count for one bridge (no bound check).
 * @param[in] signal_count Live signal sources (per-serial mode only).
 * @param[in] grouped Grouped layout flag from BridgeConfig.
 * @return Grouped: 2 (signals + particles); per-serial:
 *         signal_count + 1.
 * @note Callers size rmw resources from this; exceeding it never blocks
 *       the sim - over-bound frames drop loudly and are counted
 *       (kOutOfRange), never silently truncated.
 */
[[nodiscard]] inline constexpr std::size_t publisher_count(std::size_t signal_count, bool grouped) noexcept {
  return grouped ? 2 : signal_count + 1;
}

} // namespace wengine::ros2
