// ROS2 bridge example (built only with -DBUILD_ROS2=ON).
//
// Live state-out wiring: a Runner drives one SignalSolver (two stamped
// sources) plus one ProjectileSolver (three particles); each emitted
// Snapshot fans out through an emit::ISink chain
//   Runner -> StrideSink -> SignalSubsetSink -> RosPublisher -> RclcppTransport
// (the sig.cpp chain shape: publisher <- Subset <- Stride) and lands on
// standard topics with sim_time header stamps (docs/ros2.md):
//   /wengine/signals            grouped JointState (or per-signal below)
//   /wengine/signals/sig_<serial>
//   /wengine/particles          PointCloud2 x/y/vx/vy
// At step 250 the first source is despawned and a replacement spawned:
// the old per-serial topic goes silent and a new serial topic appears —
// serial-addressed topics survive despawn/respawn by construction.
//
// Flags: [--per-serial] [--stride N] [--select a,b,...] [--steps N]
//   --per-serial  one JointState topic per live serial (default: grouped)
//   --stride N    publish every Nth emitted snapshot (default 1)
//   --select      comma-separated serial allowlist (default: all live)
//   --steps N     fixed step count (default 500)
// The node spins (spin_some) on the driver thread after every step, so
// nothing reenters the Runner from inside on_snapshot and no Snapshot
// reference crosses the publish call.

#include "rclcpp/rclcpp.hpp"
#include "wengine/core/runner.hpp"
#include "wengine/emit/routing.hpp"
#include "wengine/ros2/config.hpp"
#include "wengine/ros2/publisher.hpp"
#include "wengine/ros2/rclcpp_transport.hpp"
#include "wengine/ros2/ros_control.hpp"
#include "wengine/solvers/projectile.hpp"
#include "wengine/solvers/signal.hpp"
#include "wengine/solvers/signal_control.hpp"
#include "wengine/world/state.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

std::vector<std::uint64_t> parse_serials(const char *text) {
  std::vector<std::uint64_t> out;
  const char *cur = text;
  while (*cur != '\0') {
    char *end = nullptr;
    const unsigned long v = std::strtoul(cur, &end, 10);
    if (end == cur) {
      break;
    }
    out.push_back(static_cast<std::uint64_t>(v));
    cur = (*end == ',') ? end + 1 : end;
  }
  return out;
}

} // namespace

int main(int argc, char **argv) {
  bool per_serial = false;
  std::uint64_t stride = 1;
  std::vector<std::uint64_t> select;
  std::uint64_t steps = 500;
  for (int k = 1; k < argc; ++k) {
    if (std::strcmp(argv[k], "--per-serial") == 0) {
      per_serial = true;
    } else if (std::strcmp(argv[k], "--stride") == 0 && k + 1 < argc) {
      stride = static_cast<std::uint64_t>(std::strtoul(argv[++k], nullptr, 10));
    } else if (std::strcmp(argv[k], "--select") == 0 && k + 1 < argc) {
      select = parse_serials(argv[++k]);
    } else if (std::strcmp(argv[k], "--steps") == 0 && k + 1 < argc) {
      steps = static_cast<std::uint64_t>(std::strtoul(argv[++k], nullptr, 10));
    } else {
      std::printf("usage: ros2_bridge [--per-serial] [--stride N] [--select a,b] [--steps N]\n");
      return 2;
    }
  }
  if (stride == 0) {
    std::printf("ros2_bridge: --stride must be >= 1\n");
    return 2;
  }

  rclcpp::init(argc, argv);
  auto node = std::make_shared<rclcpp::Node>("wengine");

  wengine::core::Runner run({.dt = 0.01, .emit_every = 1});
  auto signal_solver = wengine::solvers::SignalSolver::create();
  auto projectile_solver = wengine::solvers::ProjectileSolver::create();
  run.add_solver(*signal_solver);
  run.add_solver(*projectile_solver);

  // Two stamped sources (serials 0,1) + three particles.
  wengine::world::Entity sig0 = run.state().spawn_entity();
  wengine::world::Entity sig1 = run.state().spawn_entity();
  wengine::signal::SignalParams sine;
  sine.kind = wengine::signal::SignalKind::kSine;
  sine.quantity = wengine::signal::QuantityId::kVoltage;
  sine.freq_hz = 1.0;
  sine.amp = 3.3;
  if (!wengine::world::configure_source(sig0, run.state().signals, sine, 0.0).ok()) {
    std::printf("ros2_bridge: cannot configure source 0\n");
    return 1;
  }
  wengine::signal::SignalParams dc = sine;
  dc.kind = wengine::signal::SignalKind::kDc;
  dc.quantity = wengine::signal::QuantityId::kCurrent;
  dc.offset = 0.02;
  if (!wengine::world::configure_source(sig1, run.state().signals, dc, 0.0).ok()) {
    std::printf("ros2_bridge: cannot configure source 1\n");
    return 1;
  }
  for (int k = 0; k < 3; ++k) {
    const wengine::world::Entity e = run.state().spawn_entity();
    run.state().particles.attach(e.index);
    run.state().particles.pos_x[e.index] = static_cast<double>(k);
    run.state().particles.vel_x[e.index] = 1.0;
  }

  wengine::ros2::BridgeConfig cfg;
  cfg.grouped_signals = !per_serial;
  cfg.stride = stride;
  cfg.select_serials = select;
  if (!wengine::ros2::is_valid_config(cfg)) {
    std::printf("ros2_bridge: invalid bridge config\n");
    return 2;
  }
  wengine::ros2::RclcppTransport transport(*node, cfg);
  wengine::ros2::RosPublisher publisher(cfg, transport);
  if (!publisher.ok()) {
    std::printf("ros2_bridge: publisher failed to construct\n");
    return 1;
  }
  // Edge chain (sig.cpp shape): publisher <- Subset (if select) <- Stride.
  std::optional<wengine::emit::SignalSubsetSink> subset;
  std::optional<wengine::emit::StrideSink> stride_sink;
  wengine::emit::ISink *head = &publisher;
  if (!cfg.select_serials.empty()) {
    subset.emplace(publisher);
    subset->set_subscription(cfg.select_serials);
    head = &*subset;
  }
  if (cfg.stride != 1) {
    stride_sink.emplace(*head, cfg.stride);
    head = &*stride_sink;
  }
  run.add_sink(*head);

  // Control-in half (ROS2-2): one bus channel for the six services.
  // Callbacks enqueue only; the driver drains at the step boundary in
  // slot order, then steps exactly once — service timing never perturbs
  // dt/order (same path as the TCP/UART control servers).
  const std::uint64_t run_id =
      static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
  wengine::solvers::ControlBus bus(run_id);
  wengine::ros2::control::CensusCache census;
  wengine::ros2::RosControlService control(*node, "wengine", bus, &census);
  if (!control.ok()) {
    std::printf("ros2_bridge: control services failed to construct\n");
    return 1;
  }
  wengine::ros2::control::refresh_census(run.state(), run.state().step_index, census);
  std::uint64_t bus_applied = 0;
  std::uint64_t bus_readonly = 0;
  std::uint64_t bus_rejected = 0;

  for (std::uint64_t k = 0; k < steps; ++k) {
    // Service callbacks run here, on the driver thread: they only
    // enqueue, so nothing reenters the Runner and no state is touched.
    // A SIGTERM/SIGINT shutdown (rclcpp::ok() false, or spin_some
    // throwing once the context is gone) ends the run with a summary,
    // never an exception out of main.
    if (!rclcpp::ok()) {
      break;
    }
    try {
      rclcpp::spin_some(node);
    } catch (...) {
      break;
    }
    if (k == 250) {
      // Despawn/respawn mid-run: serial 0 goes silent, the replacement
      // (new serial) appears on its own topic; grouped mode keeps a
      // stable topic with the new serial row.
      run.state().despawn_entity(sig0);
      sig0 = run.state().spawn_entity();
      wengine::signal::SignalParams tri = sine;
      tri.kind = wengine::signal::SignalKind::kTriangle;
      if (!wengine::world::configure_source(sig0, run.state().signals, tri, run.state().sim_time).ok()) {
        std::printf("ros2_bridge: cannot respawn source\n");
        return 1;
      }
    }
    // Step boundary: drain service lines first, then step exactly once.
    const wengine::solvers::DrainStats ds = bus.drain(run.state(), run.state().step_index);
    bus_applied += ds.applied;
    bus_readonly += ds.readonly;
    bus_rejected += ds.rejected;
    // Service responses were already sent at enqueue time (enqueue
    // verdicts); the execution outbox is driver-side accounting only.
    (void)bus.take_responses(control.channel());
    wengine::ros2::control::refresh_census(run.state(), run.state().step_index, census);
    const wengine::core::Status st = run.run(1);
    if (!st.ok()) {
      std::printf("ros2_bridge: step failed\n");
      return 1;
    }
    if (!publisher.ok()) {
      std::printf("ros2_bridge: publisher went sticky\n");
      return 1;
    }
  }
  const wengine::link::PublisherStats &stats = publisher.stats();
  std::printf("ros2_bridge: attempted=%llu published=%llu over_bound=%llu backpressure=%llu "
              "disabled=%llu bytes=%llu serial_pubs=%zu services=%zu enqueued=%llu refused=%llu "
              "bus applied=%llu readonly=%llu rejected=%llu\n",
              static_cast<unsigned long long>(stats.attempted),
              static_cast<unsigned long long>(stats.published),
              static_cast<unsigned long long>(stats.dropped_over_bound),
              static_cast<unsigned long long>(stats.dropped_send),
              static_cast<unsigned long long>(stats.disabled_after_error),
              static_cast<unsigned long long>(stats.bytes_accepted),
              transport.serial_publisher_count(), control.service_count(),
              static_cast<unsigned long long>(control.enqueued()),
              static_cast<unsigned long long>(control.refused()), static_cast<unsigned long long>(bus_applied),
              static_cast<unsigned long long>(bus_readonly),
              static_cast<unsigned long long>(bus_rejected));
  rclcpp::shutdown();
  return 0;
}
