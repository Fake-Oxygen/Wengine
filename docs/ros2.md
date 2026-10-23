# ROS2 support (state-out + CLI batch emit landed, control plane landed)

Status: ROS2-1 landed the state-out half, ROS2-2 the control-in half (see
below). ROS2-3 landed the `wengine_msgs/SignalBlock` IDL
(`src/ros2/msgs/msg/SignalBlock.msg`: `step`/`sim_time` + dense slot-order
`serial[]`/`value[]`/`quantity[]`, built as a subdirectory only with
`BUILD_ROS2=ON`) + its grouped exact-record publisher
(`RclcppTransport` publishes the block on `/<ns>/<signal_stem>/block`
alongside the RViz JointState) + the `sim run` / `sig run --emit-ros` +
`--ros-*` batch flags (see below). Headers in `include/wengine/ros2/*`
stay ROS-free vocabulary (config/naming/verb mapping, topic helpers,
QoS text, service contract); no `rclcpp` is included or linked unless
`BUILD_ROS2=ON`. Unit tests (`test_ros2_mapping`, `test_ros2_publisher`,
`test_ros2_control`, CLI arg/validation tests) cover the pure mapping +
publisher + service + flag vocabulary without a ROS install; the rmw TUs
are covered by live HIL (`ros2_bridge` + `ros2 topic echo` /
`ros2 service call` + `sig run --emit-ros`), not by ctest.

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

## Message sketch (ROS2-1 std_msgs + ROS2-3 SignalBlock)

- Grouped signals: `sensor_msgs/JointState` on `/<ns>/<signal_topic_stem>`
  (header.stamp = sim_time, name `sig.<serial>.U`, position SI value;
  quantity rides in the suffix from the single quantity table) AND the
  exact `wengine_msgs/SignalBlock` record on
  `/<ns>/<signal_topic_stem>/block` (`step`/`sim_time` + dense slot-order
  `serial[]`/`value[]`/`quantity[]` — quantity codes mirror
  `signal::QuantityId`; see `src/ros2/msgs/msg/SignalBlock.msg`). One
  message per emitted tick; no header stamp on the block — `sim_time`
  carries the engine clock, never wall time.
- Per-signal: single-entry `JointState` on `/<ns>/<signal_topic_stem>/sig_<serial>`
  (the `s`-less digit-leading form `sig/<serial>` is refused by rmw —
  topic segments must not start with a digit). No block in per-serial
  mode (one topic per live serial, retained across despawn/respawn).
- Particles: `sensor_msgs/PointCloud2` (x/y/vx/vy FLOAT64) on
  `/<ns>/<particle_topic_stem>` via `frame_from_snapshot` items.
- Services: `wengine_msgs/srv/SigCommand` (`string line` in, `bool ok` +
  `string message` + `int32 code` out) on `/<ns>/sig_add|set|start|stop|drop`
  (request carries arguments only, e.g. `sine freq 1 amp 3.3` — the service
  name fixes the verb) and `/<ns>/sig_list` (empty request, answers the
  driver-refreshed census cache, stale by <1 step). Callbacks enqueue only
  and answer the enqueue verdict (`OK queued` / `ERR <code> <detail>`);
  execution happens at the next step-boundary drain. Table/Lua ADD is
  refused as `not_implemented`. (Landed with ROS2-2.)

## Batch emit flags (ROS2-3)

`sim run` and `sig run` publish through the same edge chain as every
other transport (`RosPublisher` ISink owned by the command, wrapped in
`SignalSubsetSink`/`StrideSink` at the edge, never in `RunConfig`):

```
sim run --steps 100 --emit-ros
sig run --sig-kind sine --sig-freq 1kHz --steps 100 --emit-ros
sig run --sig-kind sine --steps 100 --emit-ros --ros-per-serial --ros-qos reliable
sig run --sig-kind dc --sig-count 3 --steps 10 --emit-ros --emit-ros-select 0,2
sig run --sig-kind sine --steps 200 --control-ros   (services only, no topics)
```

- `--emit-ros` enables publishing (flag, no value). Without a ROS2
  build (`-DBUILD_ROS2=ON`) it parses + validates like every flag and
  then fails loudly (`rebuild with -DBUILD_ROS2=ON`, exit 1) — never a
  silent drop (same split as `--plot` without SDL2).
- `--emit-ros-stride N` / `--emit-ros-select <s0,s1>` are the usual
  per-output sampling (`emit_options.hpp`): stride decimates step-aligned
  (`0` = muted), selects filter signal rows by stable serial (never ticks,
  so lag accounting stays exact). Orphan stride/select without `--emit-ros`
  is a usage error (exit 2); selects need solver `signal` (stable serials
  exist only there); `sim run --solver signal` addresses serial 0; unfiltered
  runs need `count + 1 <= 64` channels (subscribe to fit, like SHM/TCP/UDP).
- `--ros-node NAME` (default `wengine`): the node name. `--ros-ns NS`
  (default `wengine`): the topic namespace — relative segments only
  (`a/b`, no leading slash; the bridge prepends exactly one `/`).
- `--ros-signal-stem STEM` (default `signals`) /
  `--ros-particle-stem STEM` (default `particles`): topic stems under
  the namespace. Same segment rules; digit-leading segments are refused
  (rmw rule — serials already use the flat `sig_<serial>` form).
- `--ros-qos latest|reliable` (default `latest`): the D-ROS-5 delivery
  class — latest-state viewing (best-effort, keep-last 1) vs ordered
  sample streaming (reliable, keep-last 10).
- `--ros-grouped` (default layout) / `--ros-per-serial`: grouped
  publishes JointState + SignalBlock on the stem topics; per-serial
  publishes one single-entry JointState per live serial (retained across
  despawn/respawn) and no block. Mutually exclusive.

Validation (same discipline as every other emit: usage errors exit 2,
never silently ignored): publish layout/decimation orphans without
`--emit-ros` (`--emit-ros-stride/select`, `--ros-grouped/per-serial`,
`--ros-signal-stem/particle-stem/qos`), `--ros-node/ns` without either
plane (`--emit-ros` or `--control-ros`), grouped + per-serial together,
bad node/namespace/stem shapes, unknown QoS, out-of-range serials,
over-capacity batches. `rclcpp::init`
runs once per process with signal handling off (the CLI owns SIGINT for
paced/`--forever` runs); the node spins in the root namespace so
`ros2 topic/service list` shows the absolute names verbatim. Subscribers must be
running concurrently (volatile durability — late joiners miss earlier
ticks); the final `ros:` summary line reports frames + QoS + layout +
stride/select counters, and sticky publish failures exit 1 loudly.

`--control-ros` (flag, ROS2-6) starts the six `SigCommand` services on
`/<ns>/sig_add|set|start|stop|drop|list` in the batch process — the same
step-synchronous plane as `--control-tcp` (`spin_some` + bus drain before
every step, slot order TCP/UART/ROS; service callbacks enqueue only and
answer `OK queued`/`ERR ...` at call time, `sig_list` serves the
driver-refreshed census). Needs a ROS2 build like `--emit-ros`, needs no
`--emit-ros` (planes attach independently), and shares the bus counters in
the `control-ros:` summary line (services/enqueued/refused/applied).

Running with ROS needs a sourced ROS env plus the fresh build-tree
message libraries on the loader path (rosidl loads the rmw typesupport
by soname at runtime):

```
source /opt/ros/<distro>/setup.bash
export LD_LIBRARY_PATH="$PWD/build/src/ros2/msgs:$LD_LIBRARY_PATH"
./build/PhysicsSimApp sig run --sig-kind sine --steps 100 --emit-ros &
ros2 topic list          # /wengine/signals, /wengine/signals/block, ...
ros2 topic echo /wengine/signals/block
```

(`build/install/lib` may hold a stale copy from an earlier install —
prefer the build-tree dir above, which is always fresh after building.
`test_sim`'s live-publish checks get this path from CTest automatically.)

## What is NOT in this task (follow-ups under the same epic)

Lifecycle/time-sync/bag notes (ROS2-4), CI parity/HIL tests (ROS2-5),
security/SROS. Those are Plane follow-ups (parent + subtasks filed under the
signals epic).
