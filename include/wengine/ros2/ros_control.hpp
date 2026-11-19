#pragma once

/**
 * @file
 * rclcpp binding for the ROS2 control plane (edge-only rmw TU). Included
 * only when WENGINE_HAS_RCLCPP is defined (BUILD_ROS2=ON); without a ROS
 * install this header is intentionally uncompilable standalone (it
 * requires rclcpp plus the generated wengine_msgs headers), so
 * verification for this file is the comment-only checker plus Doxygen -
 * see the task report. The service vocabulary in ros2/control.hpp stays
 * ROS-free and this header never leaks into it: it exposes the six
 * control verbs as uniform wengine_msgs/SigCommand services
 * (arguments in, ok/message/code out) on sig_add/set/start/stop/drop/list
 * under the node namespace. Callbacks enqueue only (parse plus validate
 * via the same strict parser the TCP/UART servers use, then
 * ControlBus::enqueue) and respond the enqueue verdict immediately;
 * nothing mutates state, nothing blocks, nothing throws out of a
 * callback. The driver drains the bus at the step boundary in slot order,
 * then steps exactly once - service timing never perturbs dt or ordering
 * (the same path as the TCP/UART control servers). sig_list serves the
 * driver-refreshed census cache (stale by less than one step).
 * Consumers: the `--control-ros` batch path and the ros2_bridge tool.
 */

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

/**
 * @brief Six SigCommand service servers (add/set/start/stop/drop/list).
 * @note Caller-owned at the app edge. The bus plus census are
 *       driver-owned and must outlive this object; the census address
 *       stays fixed while the driver refreshes its contents after every
 *       step-boundary drain. Single driver thread only (callbacks run
 *       inside the driver spin_some, like every other sink). Partial
 *       construction failure leaves fewer than six servers with ok()
 *       false - never a half-wired plane presented as healthy.
 */
class RosControlService {
public:
  // `bus` + `census` are driver-owned and must outlive this object.
  // The census address stays fixed; the driver refreshes its contents
  // after every step-boundary drain. Single driver thread only (the
  // callbacks run inside the driver's spin_some, like every other sink).
  /**
   * @brief Create the six service servers on one node and namespace.
   * @param[in] node Caller-owned rclcpp node (must outlive this object).
   * @param[in] ns Topic namespace the six services live under.
   * @param[in] bus Driver-owned control bus receiving enqueued lines.
   * @param[in] census Driver-owned census cache served by sig_list.
   */
  RosControlService(rclcpp::Node &node, std::string_view ns, solvers::ControlBus &bus, const control::CensusCache *census);

  // Six when fully constructed (add/set/start/stop/drop/list), fewer
  // after a partial construction failure (ok() false then).
  /**
   * @brief Live service server count.
   * @return Six when fully constructed, fewer after a partial
   *         construction failure (ok() false then).
   */
  [[nodiscard]] std::size_t service_count() const noexcept { return service_count_; }
  /**
   * @brief Construction health.
   * @return True when all six servers were created.
   */
  [[nodiscard]] bool ok() const noexcept { return ok_; }
  /**
   * @brief Control channel id shared with the bus accounting.
   * @return The channel this plane enqueues on.
   */
  [[nodiscard]] std::uint32_t channel() const noexcept { return channel_; }
  /**
   * @brief Enqueued service lines so far (monotonic).
   * @return Count accepted onto the bus.
   */
  [[nodiscard]] std::uint64_t enqueued() const noexcept { return enqueued_; }
  /**
   * @brief Refused service lines so far (monotonic).
   * @return Count rejected at validation time (loud ERR verdicts).
   */
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
