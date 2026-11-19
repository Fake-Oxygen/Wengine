# ROS 2 bridge

`ros2` bridges simulation state onto ROS 2: state-out publishing
(grouped signals as `sensor_msgs/JointState` plus the exact
`wengine_msgs/SignalBlock` record, particles as `PointCloud2`), a
service control plane (`sig_add`/`sig_set`/`sig_start`/`sig_stop`/
`sig_drop`/`sig_list`), and the managed-lifecycle policy. The headers
are ROS-free by construction — mapping, config, publisher, control,
and lifecycle own the naming rules, QoS intent, sim-time stamp
conversion, over-bound gates, and transition graph, and unit-test
against `wengine_core` alone; the rmw calls live in
`src/ros2/rclcpp_transport.cpp`, built only with `BUILD_ROS2=ON`. Live
use therefore requires a ROS 2 stack (rclcpp, sensor_msgs, and the
`wengine_msgs` IDL package); the transport-neutral surface below runs
without one. Design and decision log: [ROS 2 bridge](../ros2.md);
layering: [Architecture](../dev/architecture.md).

## Usage pattern

Compile with `g++ -std=c++20 -I include demo.cpp -Lbuild
-lwengine_ros2` (the ROS-free publisher TU — no ROS install needed),
run, expect exit status 0. The fake transport stands in for
`RclcppTransport`, which a live bridge builds from a `rclcpp::Node`
under `BUILD_ROS2=ON`.

```cpp
#include "wengine/ros2/publisher.hpp"
#include "wengine/signal/types.hpp"
#include "wengine/world/state.hpp"

// Live use implements this seam over rclcpp (BUILD_ROS2=ON); tests (and
// this pattern) run it without a ROS install.
class OkTransport final : public wengine::ros2::RosTransport {
public:
  wengine::ros2::TransportOutcome publish_grouped(const wengine::ros2::SignalBlock &,
                                                  const wengine::link::Frame &) noexcept override {
    return wengine::ros2::TransportOutcome::kAccepted;
  }
  wengine::ros2::TransportOutcome publish_one(std::uint64_t, double, std::uint8_t,
                                              std::uint64_t, double) noexcept override {
    return wengine::ros2::TransportOutcome::kAccepted;
  }
  wengine::ros2::TransportOutcome publish_particles(const wengine::link::Frame &) noexcept override {
    return wengine::ros2::TransportOutcome::kAccepted;
  }
};

int main() {
  OkTransport transport;
  const wengine::ros2::BridgeConfig cfg; // defaults: grouped signals, latest-state QoS
  wengine::ros2::RosPublisher pub(cfg, transport);

  wengine::world::Snapshot snap; // what a Runner hands to every sink
  snap.step_index = 1;
  snap.sim_time = 0.01;
  snap.sig_serial = {1};
  snap.sig_value = {5.0};
  snap.sig_quantity = {static_cast<std::uint8_t>(wengine::signal::QuantityId::kVoltage)};
  pub.on_snapshot(snap); // owned copies, over-bound gate, then fan-out
  return (pub.ok() && pub.stats().published == 1) ? 0 : 1;
}
```

## Key entities

Contract highlights: signals are addressed by stable serial, never
slot indices (`<stem>/sig_<serial>`; despawn/respawn never reuses a
live subscription). QoS intent is two pinned classes — latest-state
viewing (best-effort, keep-last 1) and reliable sample streaming
(keep-last 10, backpressure counted) — complete recording stays in the
CSV/block recorder, never a topic promise. Message stamps carry
`sim_time`; the ROS wall clock never drives physics. Service callbacks
enqueue only and the driver drains at the step boundary, so service
timing never perturbs `dt` or ordering. `use_sim_time` is latched
(settable only before activation), and publishers publish only in the
Active lifecycle state.

```{doxygenclass} wengine::ros2::RosPublisher
:project: wengine
```

```{doxygenclass} wengine::ros2::RosTransport
:project: wengine
```

```{doxygenenum} wengine::ros2::QosClass
:project: wengine
```

```{doxygenstruct} wengine::ros2::BridgeConfig
:project: wengine
```

```{doxygenstruct} wengine::ros2::SignalBlock
:project: wengine
```

```{doxygenclass} wengine::ros2::lifecycle::Lifecycle
:project: wengine
```

## Complete module listing

Everything public in the module, including the service mapping and
census cache (`ros2::control`), the lifecycle transition graph
(`ros2::lifecycle`), and the batch-emit CLI option parsing
(`ros2::cli_options`).

```{doxygennamespace} wengine::ros2
:project: wengine
```
