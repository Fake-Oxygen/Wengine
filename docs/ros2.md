# ROS2 support (state-out landed, control plane pending)

Status: ROS2-1 landed the state-out half — `include/wengine/ros2/publisher.hpp`
+ `src/ros2/ros_publisher.cpp` (transport-neutral `emit::ISink`, always built
as `wengine_ros2`) + `rclcpp_transport.{hpp,cpp}` (rmw TU, `BUILD_ROS2=ON`
only) + live `examples/ros2_bridge.cpp` (grouped + per-serial JointState,
PointCloud2 particles, sim_time stamps, QoS per D-ROS-5, stride/subset
wiring). `include/wengine/ros2/*` stays ROS-free vocabulary
(config/naming/verb mapping, topic helpers); no `rclcpp` is included or
linked unless `BUILD_ROS2=ON`. Unit tests (`test_ros2_mapping`,
`test_ros2_publisher`) cover the pure mapping + publisher without a ROS
install; the rmw TU is covered by live HIL (`ros2_bridge` + `ros2 topic
echo`), not by ctest.

## Why no redesign is needed

The engine already exposes the two seams a ROS2 bridge needs:

1. **State out (topics):** `emit::ISink::on_snapshot` — the same seam
   `ShmPublisher`/`TcpPublisher`/`CsvSink` use. A ROS publisher is an
   `ISink` owned at the app edge (CLI/GUI/bridge process), adapts via
   `link::frame_from_signal_snapshot` / `frame_from_snapshot` (or the
   `ros2::block_from_snapshot` grouped payload), and wraps in
   `StrideSink`/`SignalSubsetSink` for per-topic rate/filter. It is never
   in `RunConfig`/`Runner`/`Snapshot`.
2. **Control in (services):** `signal/control.hpp` vocabulary +
   `solvers::ControlBus` step-boundary drain — the same path the TCP/UART
   control servers use. A ROS service callback parses + enqueues; the
   driver drains at the step boundary in slot order, then steps once.
   Services never mutate live state directly, so `dt`/ordering/determinism
   are untouched.

Dependency direction stays `ros -> link/world/emit` (never the reverse),
headers stay ROS-free (the rmw TU replaces the platform, like
`src/link/*.cpp`), and `Snapshot` does not grow (one adapter, not fields).

## Design decisions

- **D-ROS-1 — edge-only bridge, `BUILD_ROS2=OFF` default.** Mirrors
  `BUILD_GUI=OFF`: `find_package(rclcpp ... REQUIRED)` fails fast only
  when enabled. Tests build/run without ROS.
- **D-ROS-2 — serials address signals.** Topics use stable serials
  (`<stem>/sig_<serial>`, grouped slot order + serial array otherwise),
  never slot indices. Slots recycle, serials don't (same rule as CSV
  `sig_id`, Frame `sig.<serial>.U`, control-plane addressing). ROS topic
  segments must not start with a digit (rmw refuses `sig/3`), so the
  `s`-less digit-leading segments take an `s` prefix under
  `sanitize_channel` (`sig.3.U` -> `sig/s3/U`); message *field* names
  keep the plain `sig.<serial>.U` form.
- **D-ROS-3 — SI doubles on the wire.** Unit suffixes parse at the CLI/GUI
  edge (`cli::parse_quantity_value`); services carry SI + quantity tag.
- **D-ROS-4 — four clocks stay separate.** Sim `dt`, emit stride
  (`emit_every` + `StrideSink` + `BridgeConfig::stride`), ROS wall packet
  rate, RViz refresh. Waveform frequency is none of those.
- **D-ROS-5 — QoS by delivery class** (see `docs/delivery.md`):
  latest-state viewing = best-effort/depth-1; sample streaming = reliable
  bounded with counted drops; complete recording stays in CSV/block files,
  never a topic promise.
- **D-ROS-6 — services enqueue, drain steps.** Same loud failure vocabulary
  (`OK/ERR` mirroring `core::Status`, sticky `Status`, never throws;
  over-bound frames dropped + counted, never truncated).
- **D-ROS-7 — sim time stamps messages.** `Snapshot::sim_time` -> header
  stamp; ROS wall time never drives physics.

## Message sketch (landed in ROS2-1, std_msgs-only)

- Grouped signals: `sensor_msgs/JointState` on `/<ns>/<signal_topic_stem>`
  (header.stamp = sim_time, name `sig.<serial>.U`, position SI value;
  quantity rides in the suffix from the single quantity table).
- Per-signal: single-entry `JointState` on `/<ns>/<signal_topic_stem>/sig_<serial>`
  (the `s`-less digit-leading form `sig/<serial>` is refused by rmw —
  topic segments must not start with a digit).
- Particles: `sensor_msgs/PointCloud2` (x/y/vx/vy FLOAT64) on
  `/<ns>/<particle_topic_stem>` via `frame_from_snapshot` items.
- Services: `sig_add/sig_set/sig_start/sig_stop/sig_drop/sig_list`
  (see `ros2/control.hpp`); table/Lua ADD rejected as `not_implemented`.
  (Planned with ROS2-2; the custom `wengine_msgs/SignalBlock` IDL for
  serials+quantities follows with ROS2-3.)

## What is NOT in this task (follow-ups under the same epic)

Services (ROS2-2), `wengine_msgs` IDL + `sim run`/`sig run --emit-ros` flags
(ROS2-3), lifecycle/time-sync/bag notes (ROS2-4), CI parity/HIL tests (ROS2-5),
security/SROS. Those are Plane follow-ups (parent + subtasks filed under the
signals epic).
