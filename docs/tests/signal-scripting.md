# Signals and scripting

The signal-generation slice of the suite: stamped sources evaluated at
end-of-step, the quantity/kind tables, expression/table/custom sources,
the SIG-10 control plane, and the Lua scripting runtime. Sources carry
stable serials, slot-order snapshots, and deterministic RNG — every test
below pins one of those contracts against an independent reference.

## test_signal

**What it checks**: hand-derived eval goldens per kind (sine quarter/half/
full period, square/triangle/saw exact points, DC time-independence, and
the noise golden `u01 = 0.5665615751722809` for seed 1 verified against a
separate Python splitmix64 implementation), kind/quantity parsing with
cross-dimension rejection (`2mA` as volts fails), finite/unknown validation,
the solver golden with end-of-step snapshot stamps, seeded noise-sequence
determinism, detached-slot skipping with bad-`dt` rejection, headless
signal runs, the `Frame` adapter channels (`sig.count`, `sig.<serial>.<U>`),
signals-CSV round-trips with slot-recycle exactness (despawn/respawn
mid-log, subset routing preserving slot order, duplicate/drift/decreasing
parses failing loudly with cleared output), v2/v3/v4 serialization
loaders, registry `create()` parity, the quantity table (`kQuantities`
suffix/human-name/`DimensionFor` agreement enforced by `static_assert`),
`StampedValue` typed-sample stamping, the dimension-scaling proof (a
test-only `TestTemp` dimension proving new quantities need no production
changes), expression parse/eval goldens with div-by-zero freezing the step
loudly, table CSV loading with midpoint-linear interpolation, custom
sources through the extras seam, and NaN-payload bit-exactness.

**Purpose**: signal generation, stamping, and serialization must be
exact and deterministic — snapshots at time `T` carry `value(T)`, serials
never recycle, and sums always read fresh operands.

**Guards against**: eval-order changes, serial reuse, slot-order vs
request-order confusion, serialization version drift, and quantity-table
disagreement across parse/display/control edges.

**How to run**:

```sh
ctest --test-dir build -R "^signal$"
./build/test_signal
```

**Prerequisites**: standard build only.

**Boundaries**: does not cover routing wrappers (see `test_routing`),
runtime reconfiguration (see `test_signal_control`), or scripted sources
(see `test_lua`).

## test_signal_control

**What it checks**: the SIG-10 text-line verb parser (`PING`, `HELLO`,
`LIST`, `ADD`/`SET`/`START`/`STOP`/`DROP` with shapes and refusals),
hello/ping/list flows, add/set/list flows, refusals leaving the pool
untouched, idempotent start/stop with phase-exact pause/resume (sine
continuity and noise-sequence continuity, including save/load while
stopped), noise freeze and seed restart, sum add with dangling-operand
refusal, reserved verbs, multi-channel fixed-order drain with
last-writer-wins (channel order, not arrival timing), replay-log exactness
across a two-channel session, stopped save/load/resume round-trips, and
sum quantity reconfiguration.

**Purpose**: the generic control plane lets external peers (TCP, UART,
ROS services) drive sources at step boundaries without breaking
determinism — parse shapes, refusal atomicity, and drain order are its
contract.

**Guards against**: partial application on refusal, phase jumps on
pause/resume, arrival-timing-dependent drain order, and replay divergence
from live sessions.

**How to run**:

```sh
ctest --test-dir build -R signal_control
./build/test_signal_control
```

**Prerequisites**: standard build only; engine-core tests, no sockets
(the TCP server half is `test_control_server` on the integrations page).

**Boundaries**: does not cover socket framing or hello/version gates (see
`test_link_sockets`) or UART transport of the same verbs (see
`test_uart_pub`).

## test_lua

**What it checks**: the pinned interpreter (`language_version() == 504`,
`release() == "Lua 5.4.9"` — a submodule bump fails here until goldens
are re-verified), script validation (empty/syntax-error/oversize
refusals), the full sandbox refusal list (`os`, `io`, `require`, `load`,
`print`, `debug`, coroutines, metatables, `math.random`, inf/nan/nil/
string results — each failing the step loudly with the clock frozen and
counters exact), the allowed surface (`math.floor`, string ops, `pairs()`
count pinned at exactly 25), the Lua-vs-parametric sine cross-check
(exact, same libm on same doubles), inputs, instruction budgets, purity
and determinism, save/load continuation, headless extras wiring, sums
over Lua sources, configure-time rejections, shipped-example goldens via
`WENGINE_LUA_EXAMPLE_DIR`, v5 serialization with dead-Lua round-trips,
and deterministic `pairs()` iteration (key order, identity-key rejection,
snapshot-mutation isolation, save/load continuation).

**Purpose**: Lua is the sandboxed scripting source: arbitrary user code
must evaluate deterministically inside a sealed runtime that can never
escape, persist, or perturb the clock on failure.

**Guards against**: sandbox escapes, interpreter-bump behavior drift,
budget overruns, nondeterministic iteration leaking into values, and
serialization losing script state.

**How to run**:

```sh
ctest --test-dir build -R "^lua$"
./build/test_lua
```

**Prerequisites**: standard build (vendored Lua compiles as C++). No
network, PTY, or hardware needed.

**Boundaries**: does not cover CLI `--sig-kind lua` plumbing (see
`test_sim`) or GUI controller Lua wiring (see the GUI-models page).
