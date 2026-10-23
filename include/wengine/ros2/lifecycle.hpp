#pragma once

// ROS2 lifecycle + sim-time sync policy (ROS-free vocabulary, header-only).
//
// ROS2-4 pins the bridge to the managed-lifecycle contract without linking
// rclcpp: this header owns the *transition* half (states, allowed edges,
// publish gating, use_sim_time handling) so unit tests cover it with
// wengine_core alone. The rmw TU (BUILD_ROS2=ON) maps it 1:1 onto
// rclcpp_lifecycle::LifecycleNode callbacks:
//
//   Unconfigured --configure--> Inactive --activate--> Active
//   Active --deactivate--> Inactive --cleanup--> Unconfigured
//   <any non-final> --shutdown--> Finalized
//
// Rules (see docs/ros2.md, docs/delivery.md):
//   * Publishers publish only in Active (can_publish). Service servers may
//     exist earlier but the driver drains the bus only while Active; a batch
//     run that never activates never steps.
//   * Transitions are explicit and total: every request returns accepted /
//     rejected, never throws, never skips a state. Illegal edges (e.g.
//     activate from Unconfigured, any request from Finalized) are rejected
//     with the state unchanged — the same loud-refusal discipline as the
//     control plane (ERR, never silent).
//   * use_sim_time is a latched parameter: settable only in
//     Unconfigured/Inactive (mirrors the ROS parameter set before
//     activation). When true the bridge stamps every message with engine
//     sim_time and downstream nodes interpret stamps against /clock; the
//     engine never subscribes to /clock to step physics — sim dt stays
//     authoritative and ROS wall time never drives the Runner (D-ROS-7).
//     Flipping the flag while Active or after Finalized is rejected.
//   * Sim-time sync is stamp equality, not clock servoing: header.stamp
//     must equal sim_time_to_stamp(sim_time) on every emitted tick
//     (see ros2/publisher.hpp). stamps_match_sim_time pins that check for
//     tests and the rmw TU; non-finite/negative sim_time stamps zero by
//     the same rule as the publisher (never wall time).
//
// Dependency direction stays ros -> world/emit (never the reverse); this
// header includes only the stamp helper + status vocabulary.

#include "wengine/core/status.hpp"
#include "wengine/ros2/publisher.hpp"

namespace wengine::ros2::lifecycle {

// Managed-node states (mirrors rclcpp_lifecycle::State ids 1-4 + finalized).
enum class LifecycleState : std::uint8_t {
  kUnconfigured = 0,
  kInactive = 1,
  kActive = 2,
  kFinalized = 3,
};

// Managed-node transitions (mirrors the standard lifecycle graph).
enum class LifecycleTransition : std::uint8_t {
  kConfigure = 0,
  kActivate = 1,
  kDeactivate = 2,
  kCleanup = 3,
  kShutdown = 4,
};

[[nodiscard]] inline constexpr std::string_view state_name(LifecycleState s) noexcept {
  switch (s) {
    case LifecycleState::kUnconfigured:
      return "unconfigured";
    case LifecycleState::kInactive:
      return "inactive";
    case LifecycleState::kActive:
      return "active";
    case LifecycleState::kFinalized:
      return "finalized";
  }
  return "?";
}

[[nodiscard]] inline constexpr std::string_view transition_name(LifecycleTransition t) noexcept {
  switch (t) {
    case LifecycleTransition::kConfigure:
      return "configure";
    case LifecycleTransition::kActivate:
      return "activate";
    case LifecycleTransition::kDeactivate:
      return "deactivate";
    case LifecycleTransition::kCleanup:
      return "cleanup";
    case LifecycleTransition::kShutdown:
      return "shutdown";
  }
  return "?";
}

// Allowed edges of the standard lifecycle graph. Shutdown is accepted from
// every non-final state; nothing leaves Finalized.
[[nodiscard]] inline constexpr bool can_request(LifecycleState from, LifecycleTransition t) noexcept {
  switch (t) {
    case LifecycleTransition::kConfigure:
      return from == LifecycleState::kUnconfigured;
    case LifecycleTransition::kActivate:
      return from == LifecycleState::kInactive;
    case LifecycleTransition::kDeactivate:
      return from == LifecycleState::kActive;
    case LifecycleTransition::kCleanup:
      return from == LifecycleState::kInactive;
    case LifecycleTransition::kShutdown:
      return from != LifecycleState::kFinalized;
  }
  return false;
}

[[nodiscard]] inline constexpr LifecycleState next_state(LifecycleState from, LifecycleTransition t) noexcept {
  switch (t) {
    case LifecycleTransition::kConfigure:
      return LifecycleState::kInactive;
    case LifecycleTransition::kActivate:
      return LifecycleState::kActive;
    case LifecycleTransition::kDeactivate:
      return LifecycleState::kInactive;
    case LifecycleTransition::kCleanup:
      return LifecycleState::kUnconfigured;
    case LifecycleTransition::kShutdown:
      (void)from;
      return LifecycleState::kFinalized;
  }
  return from;
}

// Stamp check for the sim-time sync policy: with use_sim_time the header
// stamp must carry the engine clock, never wall time. Compares field-wise
// so a wall-sourced stamp fails loudly instead of drifting silently.
[[nodiscard]] inline bool stamps_match_sim_time(double sim_time, RosStamp stamp) noexcept {
  const RosStamp want = sim_time_to_stamp(sim_time);
  return stamp.sec == want.sec && stamp.nanosec == want.nanosec;
}

// ROS-free lifecycle latch for the bridge process / batch driver.
class Lifecycle {
 public:
  explicit Lifecycle(bool use_sim_time = false) noexcept : use_sim_time_(use_sim_time) {}

  [[nodiscard]] LifecycleState state() const noexcept { return state_; }
  [[nodiscard]] bool use_sim_time() const noexcept { return use_sim_time_; }
  // Publish/emit gate: Active only. Inactive still owns topics/services;
  // Unconfigured/Finalized own nothing.
  [[nodiscard]] bool can_publish() const noexcept { return state_ == LifecycleState::kActive; }

  // Latched parameter: settable only before activation (Unconfigured or
  // Inactive), mirroring the ROS use_sim_time parameter. Returns false and
  // keeps the old value when called from Active/Finalized.
  bool set_use_sim_time(bool v) noexcept {
    if (state_ != LifecycleState::kUnconfigured && state_ != LifecycleState::kInactive) {
      return false;
    }
    use_sim_time_ = v;
    return true;
  }

  // Request one transition. Returns true and advances on a legal edge,
  // false with the state unchanged otherwise. Never throws.
  bool request(LifecycleTransition t) noexcept {
    if (!can_request(state_, t)) {
      return false;
    }
    state_ = next_state(state_, t);
    return true;
  }

 private:
  LifecycleState state_ = LifecycleState::kUnconfigured;
  bool use_sim_time_ = false;
};

}  // namespace wengine::ros2::lifecycle
