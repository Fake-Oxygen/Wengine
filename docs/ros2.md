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

## QoS matrix (ROS2-4, pinned)

Two delivery classes, no per-topic tuning. Pinned in code
(`ros2/publisher.hpp`: `qos_is_reliable`/`qos_depth`/`qos_reliability_name`/
`qos_durability_name`/`qos_history_name`, covered by `test_ros2_lifecycle`)
and resolved to rmw profiles in one place only (`ros_qos_for` in
`ros2/rclcpp_transport.hpp`, never in headers):

| `--ros-qos` | Class | Reliability | History | Depth | Durability | Overflow policy | Guarantee |
|---|---|---|---|---|---|---|---|
| `latest` (default) | Latest-state viewing (delivery class A) | best-effort | keep-last | 1 | volatile | Writer drops oldest; readers take latest | Live viewing only. No replay, no catch-up. |
| `reliable` | Ordered bounded sample streaming (delivery class B) | reliable | keep-last | 10 | volatile, bounded | Backpressure counted (`dropped_send`), never silent; slow readers still lose beyond the bound | Bounded jitter only. Never a zero-loss promise to an arbitrarily slow receiver. |

Durability is volatile on both classes by pin: late joiners miss earlier
ticks (no transient-local replay). Complete recording (class C) stays in
the CSV/block recorder (driver-paced, row counts + footer as proof) —
never a topic promise. See `docs/delivery.md` for the shared three-class
model; the table above is its ROS projection.

QoS compatibility note: a reliable subscriber requesting from a
best-effort (`latest`) publisher is a DDS mismatch and no data flows.
Configure recorders/dashboards that consume the `latest` class with a
matching best-effort subscription profile (same depth 1); the `reliable`
class pairs with reliable subscribers up to its depth-10 bound.

## Clocks (ROS2-4: four rates plus waveform frequency)

Independent quantities — do not conflate them (same split as
`docs/delivery.md` sec. 2):

1. Simulation clock: fixed numerical `dt` (`RunnerConfig::dt`),
   `step_index` / `sim_time`. `run(N)` advances exactly N steps; nothing
   wall-paced. Determinism lives here only.
2. Emitted sample rate: `1 / (dt * emit_every)`, further decimated per
   output by `--emit-ros-stride N` (`StrideSink`, step-aligned) and
   filtered by `--emit-ros-select` (serial allowlist, rows only — every
   tick still flows so lag accounting stays exact). Decimation changes
   sampled bandwidth and may alias a waveform; it reduces packet overhead
   without preserving skipped samples.
3. Wall-clock packet rate: how fast the rmw layer actually emits
   (frames/s, bytes/s). Set by driver pacing (`--paced`/`--rate`),
   transport capacity, and peer readiness — not by `dt`.
4. UI refresh rate: RViz / plot redraw (e.g. 30–60 Hz) over a bounded
   history. Display coalescing is counted separately from transport gaps.

Waveform frequency (`--sig-freq`, e.g. 1 kHz sine) is none of the above:
it is the physical signal being simulated. Sampling it at `dt = 0.1 ms`
(10 kHz sample rate) vs. streaming it over a 1 kHz packet budget vs.
drawing it at 60 Hz are three separate choices; undersampling aliases
regardless of transport.

## Counters (ROS2-4: lag/drop vocabulary, observable)

Counts are per edge object, monotonic within a run, same vocabulary as
`docs/delivery.md` sec. 5. The ROS path exposes every cause:

- Generated: snapshots taken by `Runner` (step-gated by `emit_every`).
- Decimated (intentional, not loss): `StrideSink` stride skips (`0` =
  muted) + `SignalSubsetSink` serial filtering (`matched`/`dropped`/
  `forwarded_rows`). Shown in the `ros:` summary line with
  `--emit-health` (`select=... matched=... dropped=...
  forwarded_rows=...`, `stride=... matched=... dropped=...`).
- Over-bound (loud refusal): `dropped_over_bound`, sticky `kOutOfRange`
  (signal rows > 64, particle items > 1024, per-serial high-water
  backstop). Never silently truncated; later snapshots count
  `disabled_after_error`.
- Backpressure (transient, counted, non-sticky): `dropped_send` (DDS
  queue full at publish). Dead peer is sticky `kInternal`.
- Published (local acceptance): `published` + `bytes_accepted`.
  Formatted by the shared `link::format_publisher_health` line
  (`attempted/published_local/over_bound/queue_full/backpressure/
  pending_blocked/pending/abandoned/disabled/bytes/status`), with the
  standing note: local acceptance only — remote receipt is unknown
  without a receiver report.
- Received/observed: `SignalBlock.step` + `sim_time` gaps on the
  subscriber side (`ros2 topic echo /<ns>/<stem>/block` shows one
  `step` per emitted tick; a skipped step is a drop, whatever the
  cause). JointState carries the same `sim_time` in `header.stamp`.
- Recorded: CSV/block recorder row counts + footer (completeness proof).
- Rendered: display-side eviction (RViz/plot history), never transport loss.

Observable surfaces: the `ros:` summary line on stdout (`frames` +
`qos` + `layout` + stride/select counters, full health with
`--emit-health`; publish failures always loud with or without the flag),
the `ros2_bridge` exit summary (`attempted/published/over_bound/
backpressure/disabled/bytes/serial_pubs/services/enqueued/refused/bus
applied/readonly/rejected`), and the `SignalBlock.step` sequence on the
wire. `output_accounting_ok` closes the books in tests
(`test_ros2_publisher`, `test_ros2_lifecycle`).

## Lifecycle node (ROS2-4)

The bridge follows the managed-lifecycle graph. The graph itself is
ROS-free (`ros2/lifecycle.hpp`: `LifecycleState` Unconfigured / Inactive
/ Active / Finalized, `can_request`/`next_state`, `Lifecycle::request`
+ `can_publish` + latched `use_sim_time`, covered by
`test_ros2_lifecycle`); the rmw TU maps each edge 1:1 onto
`rclcpp_lifecycle::LifecycleNode` callbacks
(`on_configure`/`on_activate`/`on_deactivate`/`on_cleanup`/`on_shutdown`):

```text
Unconfigured --configure--> Inactive --activate--> Active
Active --deactivate--> Inactive --cleanup--> Unconfigured
<any non-final> --shutdown--> Finalized
```

- Publish/emit gate: `Active` only (`can_publish`). Topics/services are
  created on configure (Inactive) and torn down on cleanup/shutdown;
  data flows only while Active. The batch commands (`sig run` /
  `sim run --emit-ros`) and `ros2_bridge` follow the same gating with a
  plain node: publish only while stepping (the Active equivalent), never
  before the first step or after the run ends.
- Every transition returns accepted/rejected with the state unchanged on
  rejection (illegal edges: activate from Unconfigured, any request from
  Finalized). Same loud-refusal discipline as the control plane — never
  a silent skip.
- Services follow the same gate: callbacks enqueue only and the driver
  drains at the step boundary in slot order (ROS2-2 contract, unchanged);
  draining happens while Active.

## Sim-time sync (ROS2-4)

Header stamps carry the engine clock, never wall time (D-ROS-7):
`Snapshot::sim_time` -> `header.stamp` via `sim_time_to_stamp`
(JointState grouped + per-serial, PointCloud2 particles); the exact
`SignalBlock` record carries `step` + `sim_time` doubles with no header
stamp. Non-finite/negative sim_time (never produced by the Runner)
stamps zero.

`use_sim_time` is a latched parameter (`Lifecycle::set_use_sim_time`,
settable only in Unconfigured/Inactive — refused while Active or after
Finalized, mirroring the ROS parameter). When true, downstream nodes
interpret stamps against the ROS `/clock`; the engine still never
subscribes to `/clock` to step physics — sim `dt` stays authoritative
and wall time never perturbs determinism. The sync check is stamp
equality (`lifecycle::stamps_match_sim_time`: the stamp must equal
`sim_time_to_stamp(sim_time)` on every tick), pinned in
`test_ros2_lifecycle`. A wall-sourced stamp fails the check instead of
drifting silently.

## Bag recording (ROS2-4 notes)

`ros2 bag` is a class-C consumer with class-A/B limits: it records what
its subscriptions receive, so the per-class guarantees above apply
unchanged (volatile durability means a late `record` start misses
earlier ticks; the `latest` class keeps depth 1 per topic; the exact
per-tick record is `SignalBlock`, not the RViz JointState view).

- Record the grouped pair together (`/<ns>/<stem>` JointState for
  viewing + `/<ns>/<stem>/block` SignalBlock for exact replay) plus
  `/<ns>/<particle_stem>` when particles run. Per-serial mode records
  one topic per live serial (retained across despawn/respawn; new
  serials appear as new topics).
- Match the subscription QoS to the topic class (best-effort/depth-1
  for `latest`, reliable/bounded for `reliable`) via a QoS override;
  otherwise the recorder connects with an incompatible profile and the
  loss looks like silence.
- The bag is output replay, never a restart checkpoint (same rule as
  CSV in `docs/delivery.md` sec. 6): restarting from a bag replays
  observed output; authoritative restart state is the versioned binary
  checkpoint (`world/serialize.hpp`).
- Completeness still needs a class-C proof: compare the recorded
  `SignalBlock.step` sequence against the run's step budget (gaps are
  drops by whatever cause, counted per the vocabulary above); the CSV
  recorder's row counts + footer remain the durable completeness proof.

## What is NOT in this task (follow-ups under the same epic)

CI parity/HIL tests (ROS2-5), security/SROS. Those are Plane follow-ups
(parent + subtasks filed under the signals epic).
