# Integrations and test fabric

The seams to the outside world plus the machinery that keeps the suite
honest: the ROS 2 bridge vocabulary (ROS-free here, live half covered
by hardware-in-the-loop), the generic TCP control plane, the CLI
parsing/dispatch/session contract, and the assertion-policy probe that
proves `assert` cannot be silently compiled out.

## test_ros2_control

**What it checks**: the control-plane vocabulary with no `rclcpp`:
service names (`/wengine/sig_add` …) mapping to `signal::control` verbs,
request-line shaping (`ADD sine freq 1 amp 3.3`), shape validation, and
step-boundary drain of queued service lines against a two-source state.

**Purpose**: service intent must translate to the exact engine verb and
apply only at step boundaries, never mid-step.

**Guards against**: verb/service-name drift, malformed lines reaching the
engine, and mid-step control application.

**How to run**:

```sh
ctest --test-dir build -R ros2_control
./build/test_ros2_control
```

**Prerequisites**: standard build only (no ROS 2 needed; the rmw
services are covered by live HIL with `ros2_bridge` instead).

**Boundaries**: transport, QoS, and lifecycle live in the sibling ROS 2
binaries; live topic/service behavior needs a ROS stack.

## test_ros2_lifecycle

**What it checks**: the ROS2-4 policy matrix with no `rclcpp`: QoS
classes (latest-state best-effort depth 1 vs sample-stream reliable
depth 10, both volatile), the four-clock separation (stamp carries
`sim_time` only; wall-sourced stamps fail sync; rate math from
`dt × stride`), observable counter vocabulary, and the lifecycle
transition graph with sim-time sync.

**Purpose**: delivery classes must never be conflated and wall time must
never enter stamps; this pins the policy the live bridge implements.

**Guards against**: QoS-class confusion, clock conflation (waveform vs
sample vs packet vs UI rates), and lifecycle-transition holes.

**How to run**:

```sh
ctest --test-dir build -R ros2_lifecycle
./build/test_ros2_lifecycle
```

**Prerequisites**: standard build only (no ROS 2 needed).

**Boundaries**: live `rclcpp` behavior is HIL-only; mapping and
publishing live in the sibling binaries.

## test_ros2_mapping

**What it checks**: pure topic/config/mapping vocabulary with no
`rclcpp`: per-serial and per-channel topic names with channel
sanitization, block-topic derivation, `BridgeConfig` validity and
publisher counts, snapshot→block layout, service-verb mapping, the
single-table quantity suffix proof, and `--emit-ros` CLI option parsing.

**Purpose**: names, counts, and block layout must be stable before any
transport publishes them.

**Guards against**: topic-name drift, invalid configs accepted, block
layout divergence, and CLI/bridge option mismatch.

**How to run**:

```sh
ctest --test-dir build -R ros2_mapping
./build/test_ros2_mapping
```

**Prerequisites**: standard build only (no ROS 2 needed).

**Boundaries**: the transport half lives in `test_ros2_publisher`; the
live `rclcpp` TU is HIL-only.

## test_ros2_publisher

**What it checks**: the transport-neutral `RosPublisher` ISink half with
a scripted fake transport (no `rclcpp`): grouped/block and per-serial
publishes with owned copies, outcome scripting (accepted vs backpressure
shapes), stride decimation, serial filtering, and drop/backpressure
counting.

**Purpose**: the publisher must adapt, encode, and account loss loudly
while the transport stays injectable for tests.

**Guards against**: silent drops, stride/filter bypass, and counter
vocabulary drift.

**How to run**:

```sh
ctest --test-dir build -R ros2_publisher
./build/test_ros2_publisher
```

**Prerequisites**: standard build only (no ROS 2 needed; the rmw TU is
covered by live HIL with `ros2 topic echo` instead).

**Boundaries**: live delivery over DDS needs a ROS stack and is never
asserted here.

## test_control_server

**What it checks**: the SIG-10 generic control plane over real loopback
TCP: version handshake, mid-run `set`/`start`/`stop` over the wire,
malformed and overlong rejection, two-client conflict resolution by
channel order (not arrival timing), `LIST` census, disconnect survival,
the `ControlClient` transport wrapper, the `sig` family live path, and a
`sig run --control-tcp` CLI smoke (flag parse, step-synchronous drive,
summary).

**Purpose**: external drivers must steer a run exactly like in-process
verbs, with conflicts resolved deterministically.

**Guards against**: handshake bypass, mid-run races, arrival-order
nondeterminism in conflicts, and CLI/server glue drift.

**How to run**:

```sh
ctest --test-dir build -R control_server
./build/test_control_server
```

**Prerequisites**: standard build only (binds loopback ports
19001–19200; find a free one per run).

**Boundaries**: signal-source semantics live in `test_signal`; ROS 2
service equivalents live in `test_ros2_control`.

## test_cli_args

**What it checks**: the CLI contract surface: tokenizer (quoting,
overflow counting), scalar/unit parsing through the single tables,
`ArgCursor` options/positionals, compile-time registry pins
(`CliRegistry` size 9 with per-command `complete`, `LogicRegistry`
size 22), `ohm`/`sig` handler goldens and usage shapes, `Shell`
one-shot/headless/run-fallback/history behavior, the full logic session
verb matrix plus completion, and the `sim logic` sequential path.

**Purpose**: parsing, dispatch, completion, and session behavior must
agree in one place so every command inherits the same exit-code and
stray-positional discipline.

**Guards against**: registry-size drift (new command without
registration), stray-argument acceptance, completion/text divergence,
and history-path splits.

**How to run**:

```sh
ctest --test-dir build -R cli_args
./build/test_cli_args
```

**Prerequisites**: standard build only.

**Boundaries**: per-solver numerics live on the solvers page; TUI
executor parity lives in `test_tui_exec`.

## test_assert_policy

**What it checks**: the suite-wide assertion policy: a compile-time
`#error` if `NDEBUG` is ever defined for a test TU, plus a runtime
side-effect check proving `assert()` executes its expression
(`assert((flag = true, true))` then verifying the flag).

**Purpose**: tests use plain `assert`, often around operations with side
effects; a release-style `NDEBUG` build would silently skip both the
checks and the operations.

**Guards against**: accidentally disabled assertions from build-type
`‑DNDEBUG` leaking into test targets (every `test_*` links
`wengine_test_assert` with `-UNDEBUG` to prevent exactly this).

**How to run**:

```sh
ctest --test-dir build -R assert_policy
./build/test_assert_policy
```

**Prerequisites**: standard build only.

**Boundaries**: the intentional-failure half of the proof lives in
`assert_fail_probe` plus `assert_probe_check.sh` below.

## assert_fail_probe

**What it checks**: the intentional-failure probe binary: `assert(false)`
must abort, even in an optimized build (with the same `#error` guard
against `NDEBUG` as `test_assert_policy`).

**Purpose**: proves the assertion policy holds under optimization, not
just in Debug — the positive control to `test_assert_policy`'s
negative check.

**Guards against**: the test-fabric failure where disabled assertions
turn every `assert` binary into a vacuous pass.

**How to run**:

```sh
ctest --test-dir build -R assert_probe
./build/assert_fail_probe; echo "exit=$?"
```

**Prerequisites**: standard build only; never run expecting exit 0 —
direct execution aborts (nonzero Crash/Abort signal status), which is
the passing shape. CTest never invokes it directly (see next entry).

**Boundaries**: not a behavior test; it exists only for the harness
below to invert.

## assert_probe_check.sh

**What it checks**: the harness script around the probe: runs
`assert_fail_probe`, passes when the probe aborts (assertions active),
and fails loudly when the probe exits 0 (assertions disabled).

**Purpose**: CTest `WILL_FAIL` cannot invert a crash (an abort reports
as `***Exception` regardless), so the inversion lives here instead of
in CTest metadata.

**Guards against**: the same vacuous-pass fabric failure, plus misuse
of `WILL_FAIL`-on-crash as a substitute for this harness.

**How to run**:

```sh
ctest --test-dir build -R assert_probe
sh tests/assert_probe_check.sh ./build/assert_fail_probe
```

**Prerequisites**: standard build only; takes the probe binary path as
its single argument (wrong arity is usage error exit 2).

**Boundaries**: covers only the probe-inversion mechanics; real
assertion behavior is pinned by the two binaries above.
