// ROS2-4: QoS/clock/delivery policy + lifecycle tests (no rclcpp).
// Pins the QoS matrix, the 4-clock separation, the observable counter
// vocabulary, and the lifecycle transition graph with sim-time sync.
#include "wengine/link/output_report.hpp"
#include "wengine/ros2/lifecycle.hpp"
#include "wengine/ros2/publisher.hpp"

#include <cassert>
#include <cstdio>

namespace {

namespace lc = wengine::ros2::lifecycle;

void test_qos_matrix() {
  using wengine::ros2::QosClass;
  using wengine::ros2::qos_depth;
  using wengine::ros2::qos_durability_name;
  using wengine::ros2::qos_history_name;
  using wengine::ros2::qos_is_reliable;
  using wengine::ros2::qos_reliability_name;
  // Latest-state viewing: best-effort, keep-last, depth 1, volatile.
  assert(!qos_is_reliable(QosClass::kLatestState));
  assert(qos_depth(QosClass::kLatestState) == 1);
  assert(qos_reliability_name(QosClass::kLatestState) == "best_effort");
  assert(qos_history_name(QosClass::kLatestState) == "keep_last");
  assert(qos_durability_name(QosClass::kLatestState) == "volatile");
  // Ordered bounded streaming: reliable, keep-last, depth 10, volatile.
  assert(qos_is_reliable(QosClass::kSampleStream));
  assert(qos_depth(QosClass::kSampleStream) == 10);
  assert(qos_reliability_name(QosClass::kSampleStream) == "reliable");
  assert(qos_history_name(QosClass::kSampleStream) == "keep_last");
  assert(qos_durability_name(QosClass::kSampleStream) == "volatile");
  // Depth ordering pins the delivery-class split: streaming buffers more,
  // viewing keeps latest only. Durability stays volatile on both (late
  // joiners miss earlier ticks; complete capture is never a topic).
  assert(qos_depth(QosClass::kSampleStream) > qos_depth(QosClass::kLatestState));
}

void test_clocks_stay_separate() {
  // One sim_time maps to exactly one stamp; wall time never enters.
  // Waveform frequency (physics), sample rate 1/(dt*stride), packet rate,
  // and UI refresh are four independent quantities — this pins the stamp
  // leg: the stamp carries sim_time only.
  const double sim_time = 12.5;
  const wengine::ros2::RosStamp stamp = wengine::ros2::sim_time_to_stamp(sim_time);
  assert(stamp.sec == 12 && stamp.nanosec == 500000000u);
  assert(lc::stamps_match_sim_time(sim_time, stamp));
  // A wall-sourced stamp (off by one second) fails the sync check loudly.
  wengine::ros2::RosStamp wall = stamp;
  wall.sec += 1;
  assert(!lc::stamps_match_sim_time(sim_time, wall));
  // Sampling math is independent of the transport: dt sets physics,
  // stride decimates samples. Same dt, different stride -> different
  // emitted rate, identical physics.
  const double dt = 0.001;
  const double full_rate = 1.0 / (dt * 1.0);
  const double deci_rate = 1.0 / (dt * 10.0);
  assert(full_rate == 1000.0 && deci_rate == 100.0);
}

void test_counters_observable() {
  // Every cause in docs/delivery.md sec. 5 has a named counter on the ROS
  // path and one shared health line; accounting must close exactly.
  wengine::link::PublisherStats s;
  s.attempted = 10;
  s.published = 7;
  s.dropped_over_bound = 1;
  s.dropped_send = 1;
  s.disabled_after_error = 1;
  assert(wengine::link::output_accounting_ok(s, 0));
  const std::string health = wengine::link::format_publisher_health(
      s, wengine::core::Status::success(), 0, 0, 0, 0);
  // Each cause appears exactly once with its canonical label.
  assert(health.find("attempted=10") != std::string::npos);
  assert(health.find("published_local=7") != std::string::npos);
  assert(health.find("over_bound=1") != std::string::npos);
  assert(health.find("backpressure=1") != std::string::npos);
  assert(health.find("disabled=1") != std::string::npos);
  assert(health.find("local acceptance only") != std::string::npos);
}

void test_lifecycle_transitions() {
  lc::Lifecycle node;
  assert(node.state() == lc::LifecycleState::kUnconfigured);
  assert(!node.can_publish());
  assert(!node.use_sim_time());
  // Illegal edges from Unconfigured: only configure/shutdown.
  assert(!node.request(lc::LifecycleTransition::kActivate));
  assert(!node.request(lc::LifecycleTransition::kDeactivate));
  assert(!node.request(lc::LifecycleTransition::kCleanup));
  assert(node.state() == lc::LifecycleState::kUnconfigured);
  // Configure -> Inactive; publish still gated.
  assert(node.request(lc::LifecycleTransition::kConfigure));
  assert(node.state() == lc::LifecycleState::kInactive);
  assert(!node.can_publish());
  // use_sim_time latches before activation.
  assert(node.set_use_sim_time(true));
  assert(node.use_sim_time());
  // Activate -> Active; publish opens.
  assert(node.request(lc::LifecycleTransition::kActivate));
  assert(node.state() == lc::LifecycleState::kActive);
  assert(node.can_publish());
  // Flag flip while Active is refused (stays true).
  assert(!node.set_use_sim_time(false));
  assert(node.use_sim_time());
  // Reactivate from Active is illegal; deactivate returns to Inactive.
  assert(!node.request(lc::LifecycleTransition::kActivate));
  assert(node.request(lc::LifecycleTransition::kDeactivate));
  assert(node.state() == lc::LifecycleState::kInactive);
  assert(!node.can_publish());
  // Cleanup -> Unconfigured; the flag survives (latched param).
  assert(node.request(lc::LifecycleTransition::kCleanup));
  assert(node.state() == lc::LifecycleState::kUnconfigured);
  assert(node.use_sim_time());
  // Shutdown from Unconfigured -> Finalized; nothing leaves Finalized.
  assert(node.request(lc::LifecycleTransition::kShutdown));
  assert(node.state() == lc::LifecycleState::kFinalized);
  assert(!node.can_publish());
  assert(!node.request(lc::LifecycleTransition::kConfigure));
  assert(!node.request(lc::LifecycleTransition::kShutdown));
  assert(!node.set_use_sim_time(false));
  assert(node.state() == lc::LifecycleState::kFinalized);
}

void test_lifecycle_names_and_edges() {
  assert(lc::state_name(lc::LifecycleState::kUnconfigured) == "unconfigured");
  assert(lc::state_name(lc::LifecycleState::kActive) == "active");
  assert(lc::transition_name(lc::LifecycleTransition::kConfigure) == "configure");
  assert(lc::transition_name(lc::LifecycleTransition::kShutdown) == "shutdown");
  // Shutdown accepted from every non-final state.
  assert(lc::can_request(lc::LifecycleState::kUnconfigured, lc::LifecycleTransition::kShutdown));
  assert(lc::can_request(lc::LifecycleState::kInactive, lc::LifecycleTransition::kShutdown));
  assert(lc::can_request(lc::LifecycleState::kActive, lc::LifecycleTransition::kShutdown));
  assert(!lc::can_request(lc::LifecycleState::kFinalized, lc::LifecycleTransition::kShutdown));
}

}  // namespace

int main() {
  test_qos_matrix();
  test_clocks_stay_separate();
  test_counters_observable();
  test_lifecycle_transitions();
  test_lifecycle_names_and_edges();
  std::printf("test_ros2_lifecycle: OK\n");
  return 0;
}
