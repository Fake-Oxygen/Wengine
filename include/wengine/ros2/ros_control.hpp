#pragma once

// rclcpp binding for the ROS2 control plane (edge-only, rmw TU).
//
// Included only when WENGINE_HAS_RCLCPP is defined (BUILD_ROS2=ON); the
// service vocabulary in ros2/control.hpp stays ROS-free and this header
// never leaks into it. Exposes the six control verbs as uniform
// wengine_msgs/SigCommand services (args in, ok/message/code out):
//   /<ns>/sig_add|set|start|stop|drop|list
// Callbacks enqueue only (parse + validate via the same strict parser
// the TCP/UART servers use, then ControlBus::enqueue) and respond the
// ENQUEUE verdict immediately; nothing mutates state, nothing blocks,
// nothing throws out of a callback. The driver drains the bus at the
// step boundary in slot order, then steps exactly once — service timing
// never perturbs dt/order (same path as the TCP/UART control servers).
// sig_list serves the driver-refreshed census cache (stale by <1 step).

#ifndef WENGINE_HAS_RCLCPP
#error "wengine/ros2/ros_control.hpp requires WENGINE_HAS_RCLCPP (BUILD_ROS2=ON)"
#endif

#include "rclcpp/rclcpp.hpp"
#include "wengine/ros2/control.hpp"
#include "wengine/solvers/signal_control.hpp"
#include "wengine_msgs/srv/sig_command.hpp"

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

namespace wengine::ros2 {

class RosControlService {
public:
  // `bus` + `census` are driver-owned and must outlive this object.
  // The census address stays fixed; the driver refreshes its contents
  // after every step-boundary drain. Single driver thread only (the
  // callbacks run inside the driver's spin_some, like every other sink).
  RosControlService(rclcpp::Node &node, std::string_view ns, solvers::ControlBus &bus, const control::CensusCache *census);

  // Six when fully constructed (add/set/start/stop/drop/list), fewer
  // after a partial construction failure (ok() false then).
  [[nodiscard]] std::size_t service_count() const noexcept { return service_count_; }
  [[nodiscard]] bool ok() const noexcept { return ok_; }
  [[nodiscard]] std::uint32_t channel() const noexcept { return channel_; }
  [[nodiscard]] std::uint64_t enqueued() const noexcept { return enqueued_; }
  [[nodiscard]] std::uint64_t refused() const noexcept { return refused_; }

private:
  using Srv = wengine_msgs::srv::SigCommand;
  using ServicePtr = rclcpp::Service<Srv>::SharedPtr;

  void handle(std::string_view service, const std::shared_ptr<Srv::Request> &req, const std::shared_ptr<Srv::Response> &resp) noexcept;

  rclcpp::Node *node_; // caller-owned (outlives this object)
  solvers::ControlBus *bus_; // driver-owned (outlives this object)
  const control::CensusCache *census_; // driver-owned (outlives this object)
  std::uint32_t channel_ = 0;
  std::array<ServicePtr, 6> services_{};
  std::size_t service_count_ = 0;
  bool ok_ = true;
  std::uint64_t enqueued_ = 0;
  std::uint64_t refused_ = 0;
};

} // namespace wengine::ros2
