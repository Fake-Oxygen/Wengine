#pragma once

/**
 * @file
 * ROS2 lifecycle + sim-time sync policy (transport-neutral, ROS-free
 * vocabulary, header-only). Pins the bridge to the managed-lifecycle
 * contract without linking rclcpp: states, allowed edges, publish
 * gating, and the latched use_sim_time parameter - so unit tests cover
 * it with wengine_core alone and no ROS install, while the rmw TU
 * (BUILD_ROS2=ON only) maps each edge 1:1 onto
 * rclcpp_lifecycle::LifecycleNode callbacks. Dependency direction: ros
 * -> world/emit, never the reverse; this header includes only the
 * status vocabulary and the stamp helper. Consumers: the bridge
 * process, the `sim run`/`sig run` batch drivers, and
 * test_ros2_lifecycle. Rules (docs/ros2.md): data flows only while
 * Active; every transition is an accepted/rejected answer with the
 * state unchanged on rejection (loud refusal, never a throw, never a
 * silent skip); message stamps always carry the engine sim_time - the
 * engine never subscribes to /clock to step physics (D-ROS-7).
 */

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
/**
 * @brief Managed-node states (mirrors the standard lifecycle graph).
 * @note Values: kUnconfigured (owns nothing yet), kInactive (topics and
 *       services exist, data does not flow), kActive (the only state
 *       that publishes/drains), kFinalized (terminal; nothing leaves).
 */
enum class LifecycleState : std::uint8_t {
  kUnconfigured = 0,
  kInactive = 1,
  kActive = 2,
  kFinalized = 3,
};

// Managed-node transitions (mirrors the standard lifecycle graph).
/**
 * @brief Managed-node transitions (standard lifecycle graph edges).
 * @note kConfigure (Unconfigured to Inactive), kActivate (Inactive to
 *       Active), kDeactivate (Active to Inactive), kCleanup (Inactive to
 *       Unconfigured), kShutdown (any non-final state to Finalized).
 *       can_request pins the legal set.
 */
enum class LifecycleTransition : std::uint8_t {
  kConfigure = 0,
  kActivate = 1,
  kDeactivate = 2,
  kCleanup = 3,
  kShutdown = 4,
};

/**
 * @brief Lowercase state name for diagnostics.
 * @param[in] s Lifecycle state.
 * @return `unconfigured`/`inactive`/`active`/`finalized`, or `?` for an
 *         out-of-range value (never produced by this header).
 */
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

/**
 * @brief Lowercase transition name for diagnostics.
 * @param[in] t Lifecycle transition.
 * @return `configure`/`activate`/`deactivate`/`cleanup`/`shutdown`, or
 *         `?` for an out-of-range value (never produced here).
 */
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
/**
 * @brief Check whether one transition request is a legal edge.
 * @param[in] from Current lifecycle state.
 * @param[in] t Requested transition.
 * @return True on a legal edge (configure from Unconfigured, activate or
 *         cleanup from Inactive, deactivate from Active, shutdown from
 *         every non-final state); false for illegal requests (activate
 *         from Unconfigured, anything from Finalized) - a loud refusal,
 *         never a silent skip.
 */
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

/**
 * @brief Destination state of a transition (applied only after can_request).
 * @param[in] from Current state (ignored for shutdown, which always
 *                 lands in Finalized).
 * @param[in] t Transition being applied.
 * @return The edge destination: configure to Inactive, activate to
 *         Active, deactivate to Inactive, cleanup to Unconfigured,
 *         shutdown to Finalized; `from` itself for an out-of-range
 *         transition (never produced here).
 * @note This function does not validate the edge - call can_request
 *       first (Lifecycle::request does).
 */
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
/**
 * @brief Sim-time sync check: the stamp must equal the engine clock.
 * @param[in] sim_time Engine simulation time in seconds (SI).
 * @param[in] stamp Header stamp actually placed on the message.
 * @return True only when stamp equals sim_time_to_stamp(sim_time)
 *         field-wise (sec and nanosec).
 * @note Pinned by test_ros2_lifecycle: a wall-sourced stamp fails the
 *       check loudly instead of drifting silently (D-ROS-7).
 */
[[nodiscard]] inline bool stamps_match_sim_time(double sim_time, RosStamp stamp) noexcept {
  const RosStamp want = sim_time_to_stamp(sim_time);
  return stamp.sec == want.sec && stamp.nanosec == want.nanosec;
}

// ROS-free lifecycle latch for the bridge process / batch driver.
/**
 * @brief ROS-free lifecycle latch: the state machine plus the latched
 *        use_sim_time parameter for one bridge process or batch driver.
 * @note Same loud-refusal discipline as the control plane: rejected
 *       requests return false with the state unchanged, never throw.
 *       Single-threaded by construction (the batch driver owns one).
 */
class Lifecycle {
 public:
  /**
   * @brief Construct in Unconfigured with an optional initial
   *        use_sim_time latch.
   * @param[in] use_sim_time Initial value of the latched parameter.
   */
  explicit Lifecycle(bool use_sim_time = false) noexcept : use_sim_time_(use_sim_time) {}

  /**
   * @brief Current lifecycle state.
   * @return The managed-node state (starts Unconfigured).
   */
  [[nodiscard]] LifecycleState state() const noexcept { return state_; }
  /**
   * @brief The latched use_sim_time parameter.
   * @return Current value (set only via set_use_sim_time).
   */
  [[nodiscard]] bool use_sim_time() const noexcept { return use_sim_time_; }
  // Publish/emit gate: Active only. Inactive still owns topics/services;
  // Unconfigured/Finalized own nothing.
  /**
   * @brief Publish/emit gate: data flows only while Active.
   * @return True iff the state is kActive.
   * @note Inactive still owns topics/services; Unconfigured/Finalized
   *       own nothing.
   */
  [[nodiscard]] bool can_publish() const noexcept { return state_ == LifecycleState::kActive; }

  // Latched parameter: settable only before activation (Unconfigured or
  // Inactive), mirroring the ROS use_sim_time parameter. Returns false and
  // keeps the old value when called from Active/Finalized.
  /**
   * @brief Set the latched use_sim_time parameter (pre-activation only).
   * @param[in] v New latch value.
   * @return True when set; false (value unchanged) when called from
   *         Active/Finalized.
   * @note Mirrors the ROS parameter: it is settable only in
   *       Unconfigured/Inactive. When true, downstream nodes interpret
   *       stamps against /clock - the engine itself never subscribes
   *       to /clock to step physics (sim dt stays authoritative).
   */
  bool set_use_sim_time(bool v) noexcept {
    if (state_ != LifecycleState::kUnconfigured && state_ != LifecycleState::kInactive) {
      return false;
    }
    use_sim_time_ = v;
    return true;
  }

  // Request one transition. Returns true and advances on a legal edge,
  // false with the state unchanged otherwise. Never throws.
  /**
   * @brief Request one lifecycle transition.
   * @param[in] t The transition to apply.
   * @return True and the state advanced on a legal edge; false with the
   *         state unchanged on an illegal request.
   * @note Never throws; illegal edges are loud refusals (can_request
   *       pins the legal set).
   */
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
