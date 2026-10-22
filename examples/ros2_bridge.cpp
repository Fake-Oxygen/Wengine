// ROS2 bridge example (built only with -DBUILD_ROS2=ON).
//
// Reference wiring sketch: owns a Runner + an emit::ISink that adapts each
// Snapshot via link::frame_from_signal_snapshot / ros2::block_from_snapshot
// (grouped) or per-serial topics, wraps it in StrideSink/SignalSubsetSink
// for rate/filter, and drains a ControlBus-fed service queue at the step
// boundary (see signal/control.hpp + solvers/signal_control.hpp, same as
// the TCP control server in src/cli/commands/sig.cpp).
//
// This file intentionally includes rclcpp: it compiles only inside the
// `if(BUILD_ROS2)` block in CMakeLists.txt (hard fail on missing ROS only
// when enabled, like BUILD_GUI). Engine headers stay ROS-free.
//
// Sketch (not compiled in default builds):
//
//   #include "rclcpp/rclcpp.hpp"
//   #include "wengine/core/runner.hpp"
//   #include "wengine/emit/routing.hpp"
//   #include "wengine/link/frame.hpp"
//   #include "wengine/ros2/config.hpp"
//   #include "wengine/ros2/control.hpp"
//   #include "wengine/ros2/mapping.hpp"
//   ... node owns Runner, ISink::on_snapshot adapts->publishes (owned copy
//   before return, never retains the Snapshot ref, never throws, never
//   reenters the Runner), services enqueue control lines, driver drains +
//   steps + spins_some at the boundary ...
//
// See docs/ros2.md for QoS/clock/delivery policy.
int main() { return 0; }
