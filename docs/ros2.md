# ROS2 support (scaffolding)

Status: scaffolding only. `include/wengine/ros2/*` is ROS-free vocabulary
(config/naming/verb mapping); no `rclcpp` is included or linked unless
`BUILD_ROS2=ON`. Unit tests cover the pure mapping without a ROS install.

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
  (`<stem>/sig/<serial>` or grouped slot-order + serial array), never
  slot indices. Slots recycle, serials don't (same rule as CSV `sig_id`,
  Frame `sig.<serial>.U`, control-plane addressing).
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

## Message sketch (edge TU decides, not the engine)

- Grouped signals: custom `wengine_msgs/SignalBlock` (step, sim_time,
  serial[], value[], quantity[]) or `Float64MultiArray` + serial layout.
- Per-signal: `Float64Stamped` on `<stem>/sig/<serial>`.
- Particles: positions via `frame_from_snapshot` items.
- Services: `sig_add/sig_set/sig_start/sig_stop/sig_drop/sig_list`
  (see `ros2/control.hpp`); table/Lua ADD rejected as `not_implemented`.

## What is NOT in this scaffolding

rclcpp TU, `wengine_msgs` package, `sim run --emit-ros` flags, lifecycle
node, time-sync, bag recording, security/SROS. Those are Plane follow-ups
(parent + subtasks filed under the signals epic).
