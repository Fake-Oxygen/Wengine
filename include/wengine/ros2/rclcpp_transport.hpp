#pragma once

/**
 * @file
 * rclcpp binding for the ROS2 state-out publisher (edge-only rmw TU).
 * Included only when WENGINE_HAS_RCLCPP is defined (BUILD_ROS2=ON);
 * without a ROS install this header is intentionally uncompilable
 * standalone (it requires rclcpp plus the generated wengine_msgs
 * headers), so verification for this file is the comment-only checker
 * plus Doxygen - see the task report. The transport-neutral vocabulary
 * in ros2/publisher.hpp stays ROS-free and this header never leaks into
 * it: it maps the fan-out onto standard messages plus the exact
 * wengine_msgs/SignalBlock record (grouped signals as sensor_msgs JointState
 * on the stem topic with header.stamp = sim_time plus SignalBlock on the
 * stem/block topic carrying step/sim_time/serial/value/quantity;
 * per-serial single-entry JointStates with lazily created, serial-
 * retained publishers and no block; particles as PointCloud2 FLOAT64
 * x/y/vx/vy via frame_from_snapshot). QoS resolves from
 * BridgeConfig::qos per D-ROS-5 (ros_qos_for below): latest-state viewing
 * is best-effort/keep-last-1, sample streaming is reliable/keep-last-10,
 * both durability-volatile. Message staging reuses member buffers;
 * publish copies into DDS before returning, so reuse after publish is
 * safe and no Snapshot reference is ever retained. Consumers: the
 * `sim run` / `sig run --emit-ros` batch paths and the ros2_bridge tool.
 */

// rclcpp binding for the ROS2 state-out publisher (edge-only, rmw TU).
//
// Included only when WENGINE_HAS_RCLCPP is defined (BUILD_ROS2=ON); the
// core publisher header (ros2/publisher.hpp) stays ROS-free and this
// header never leaks into it. Maps the transport-neutral fan-out onto
// standard messages plus the exact wengine_msgs/SignalBlock record:
//   * grouped signals -> sensor_msgs::msg::JointState on
//     `<ns>/<signal_stem>` (header.stamp = sim_time, name
//     `sig.<serial>.U`, position SI value; quantity in the suffix) AND
//     wengine_msgs::msg::SignalBlock on `<ns>/<signal_stem>/block`
//     (step + sim_time + serial/value/quantity arrays — the exact
//     record; see msg/SignalBlock.msg).
//   * per-signal      -> single-entry JointState on
//     `<ns>/<signal_stem>/sig_<serial>`, publishers created lazily
//     and retained (serial survival across despawn/respawn by
//     construction; the publisher-side high-water gate bounds the set).
//     No SignalBlock in per-serial mode.
//   * particles       -> sensor_msgs::msg::PointCloud2 (x/y/vx/vy FLOAT64)
//     on `<ns>/<particle_stem>` via frame_from_snapshot.
// QoS resolves from BridgeConfig::qos per D-ROS-5 (see ros_qos_for):
// latest-state viewing = best-effort/keep-last-1, sample streaming =
// reliable/keep-last-10, both transient-local off. Message staging
// reuses member buffers; publish(const Msg&) copies into DDS before
// returning, so reuse after publish is safe and no Snapshot reference
// is ever retained.

#ifndef WENGINE_HAS_RCLCPP
#error "wengine/ros2/rclcpp_transport.hpp requires WENGINE_HAS_RCLCPP (BUILD_ROS2=ON)"
#endif

#include "rclcpp/rclcpp.hpp"
#include "sensor_msgs/msg/joint_state.hpp"
#include "sensor_msgs/msg/point_cloud2.hpp"
#include "wengine/ros2/publisher.hpp"
#include "wengine_msgs/msg/signal_block.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>

namespace wengine::ros2 {

// QoS profile for one delivery class (D-ROS-5). Latest-state viewing
// drops oldest (best-effort, depth 1); sample streaming is reliable
// bounded (depth 10, drops counted DDS-side beyond history).
/**
 * @brief Resolve a QoS delivery class to its rmw profile (single place).
 * @param[in] q Delivery class from BridgeConfig (D-ROS-5 intent).
 * @return Best-effort keep-last-1 durability-volatile for latest-state
 *         viewing; reliable keep-last-10 durability-volatile for ordered
 *         bounded sample streaming.
 * @note The only spot that names rmw profiles; headers resolve intent
 *       only (qos_is_reliable/qos_depth in ros2/publisher.hpp).
 */
[[nodiscard]] inline rclcpp::QoS ros_qos_for(QosClass q) {
  if (qos_is_reliable(q)) {
    return rclcpp::QoS(qos_depth(q)).reliable().durability_volatile();
  }
  return rclcpp::QoS(qos_depth(q)).best_effort().durability_volatile();
}

/**
 * @brief rclcpp RosTransport: grouped/per-serial signals plus particles.
 * @note Caller-owned at the app edge (like every sink); the node pointer
 *       must outlive this object. Single-writer model: the driver calls
 *       publish_* from one thread only. Message staging reuses owned
 *       member buffers; publish copies into DDS before returning, so no
 *       Snapshot reference is ever retained. Never throws out of the
 *       noexcept overrides (a throwing rmw call reports kDead).
 */
class RclcppTransport : public RosTransport {
public:
  /**
   * @brief Build the transport over an existing node and config.
   * @param[in] node Caller-owned rclcpp node (must outlive this object).
   * @param[in] cfg Validated bridge identity plus delivery policy (copied).
   */
  explicit RclcppTransport(rclcpp::Node &node, const BridgeConfig &cfg);

  /**
   * @brief Publish the grouped pair (JointState plus exact SignalBlock).
   * @param[in] block Owned grouped signal record (dense slot order).
   * @param[in] particles Owned particle frame copy.
   * @return kAccepted on local DDS acceptance; kBackpressure on a
   *         transient full queue (counted, never sticky); kDead when the
   *         transport is unusable.
   */
  TransportOutcome publish_grouped(const SignalBlock &block, const link::Frame &particles) noexcept override;
  /**
   * @brief Publish one single-entry per-serial JointState.
   * @param[in] serial Stable source serial (topic key, never a slot).
   * @param[in] value SI value of the source this tick.
   * @param[in] quantity Quantity code mirroring signal::QuantityId.
   * @param[in] step Engine step index of the emitted tick.
   * @param[in] sim_time Engine sim time in seconds (SI); the header
   *                     stamp, never wall time.
   * @return kAccepted on local DDS acceptance; kBackpressure on a
   *         transient full queue; kDead when unusable.
   */
  TransportOutcome publish_one(std::uint64_t serial, double value, std::uint8_t quantity, std::uint64_t step, double sim_time) noexcept override;
  /**
   * @brief Publish the particle cloud frame.
   * @param[in] particles Owned particle frame copy.
   * @return kAccepted on local DDS acceptance; kBackpressure on a
   *         transient full queue; kDead when unusable.
   */
  TransportOutcome publish_particles(const link::Frame &particles) noexcept override;

  /**
   * @brief Bridge identity plus delivery policy this transport was built with.
   * @return Const reference to the owned config copy.
   */
  [[nodiscard]] const BridgeConfig &config() const noexcept { return config_; }
  // Lazily created per-serial publishers retained so far (serial
  // survival: despawn never removes one; the publisher-side
  // high-water gate bounds the set).
  /**
   * @brief Count of lazily created per-serial publishers retained so far.
   * @return Serial publisher count (despawn never removes one; the
   *         publisher-side high-water gate bounds the set).
   */
  [[nodiscard]] std::size_t serial_publisher_count() const noexcept { return serial_pubs_.size(); }

private:
  using JointStatePub = rclcpp::Publisher<sensor_msgs::msg::JointState>::SharedPtr;
  using CloudPub = rclcpp::Publisher<sensor_msgs::msg::PointCloud2>::SharedPtr;

  TransportOutcome ensure_serial_pub(std::uint64_t serial, JointStatePub &out) noexcept;
  static void fill_stamp(std_msgs::msg::Header &header, double sim_time) noexcept;
  void fill_particles_msg(const link::Frame &particles) noexcept;

  rclcpp::Node *node_; // caller-owned (outlives this transport)
  BridgeConfig config_;
  rclcpp::QoS qos_;
  JointStatePub grouped_pub_; // null in per-serial mode
  using SignalBlockPub = rclcpp::Publisher<wengine_msgs::msg::SignalBlock>::SharedPtr;
  SignalBlockPub block_pub_; // grouped exact record; null in per-serial mode
  std::map<std::uint64_t, JointStatePub> serial_pubs_; // retained per serial
  CloudPub particles_pub_;
  sensor_msgs::msg::JointState grouped_msg_; // reused staging (owned copy)
  wengine_msgs::msg::SignalBlock block_msg_; // reused staging (owned copy)
  sensor_msgs::msg::JointState one_msg_; // reused staging (owned copy)
  sensor_msgs::msg::PointCloud2 particles_msg_; // reused staging (owned copy)
};

} // namespace wengine::ros2
