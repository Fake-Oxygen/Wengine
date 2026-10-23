#pragma once

// rclcpp binding for the ROS2 state-out publisher (edge-only, rmw TU).
//
// Included only when WENGINE_HAS_RCLCPP is defined (BUILD_ROS2=ON); the
// core publisher header (ros2/publisher.hpp) stays ROS-free and this
// header never leaks into it. Maps the transport-neutral fan-out onto
// standard messages so no custom IDL is needed for the state-out half
// (wengine_msgs/SignalBlock lands with ROS2-3):
//   * grouped signals -> sensor_msgs::msg::JointState on
//     `<ns>/<signal_topic_stem>` (header.stamp = sim_time, name
//     `sig.<serial>.U`, position SI value; quantity in the suffix).
//   * per-signal      -> single-entry JointState on
//     `<ns>/<signal_topic_stem>/sig/<serial>`, publishers created lazily
//     and retained (serial survival across despawn/respawn by
//     construction; the publisher-side high-water gate bounds the set).
//   * particles       -> sensor_msgs::msg::PointCloud2 (x/y/vx/vy FLOAT64)
//     on `<ns>/<particle_topic_stem>` via frame_from_snapshot.
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

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>

namespace wengine::ros2 {

// QoS profile for one delivery class (D-ROS-5). Latest-state viewing
// drops oldest (best-effort, depth 1); sample streaming is reliable
// bounded (depth 10, drops counted DDS-side beyond history).
[[nodiscard]] inline rclcpp::QoS ros_qos_for(QosClass q) {
  if (qos_is_reliable(q)) {
    return rclcpp::QoS(qos_depth(q)).reliable().durability_volatile();
  }
  return rclcpp::QoS(qos_depth(q)).best_effort().durability_volatile();
}

class RclcppTransport : public RosTransport {
public:
  explicit RclcppTransport(rclcpp::Node &node, const BridgeConfig &cfg);

  TransportOutcome publish_grouped(const SignalBlock &block, const link::Frame &particles) noexcept override;
  TransportOutcome publish_one(std::uint64_t serial, double value, std::uint8_t quantity, std::uint64_t step, double sim_time) noexcept override;
  TransportOutcome publish_particles(const link::Frame &particles) noexcept override;

  [[nodiscard]] const BridgeConfig &config() const noexcept { return config_; }
  // Lazily created per-serial publishers retained so far (serial
  // survival: despawn never removes one; the publisher-side
  // high-water gate bounds the set).
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
  std::map<std::uint64_t, JointStatePub> serial_pubs_; // retained per serial
  CloudPub particles_pub_;
  sensor_msgs::msg::JointState grouped_msg_; // reused staging (owned copy)
  sensor_msgs::msg::JointState one_msg_; // reused staging (owned copy)
  sensor_msgs::msg::PointCloud2 particles_msg_; // reused staging (owned copy)
};

} // namespace wengine::ros2
