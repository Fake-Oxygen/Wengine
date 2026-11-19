# Emit and routing

The output slice of the suite: sinks that observe snapshots
(`ConsoleSink`, `CsvSink`, `SignalsCsvSink`), the edge-side routing
wrappers (`SignalSubsetSink`, `StrideSink`, `kSum` combinators), the
per-cause health accounting shared by every transport, the bounded SPSC
output queue, and wall-clock-paced runs. Drops are always loud and
counted here — never silent.

## test_emit

**What it checks**: `ConsoleSink` exact line format
(`step 3 t 0.03s particles 2`), CSV content with the 9-column header
(`step,time,slot,entity_index,entity_generation,x,y,vx,vy`),
`max_digits10` exact round-trip including the inexact-binary `0.1 + 0.2`,
stable identity columns, header-only output for empty snapshots, sticky
failure on unwritable paths (never throws, zero rows), and an end-to-end
`Runner` drive where step `k` carries `k` particles in slot order through
both console and file sinks.

**Purpose**: CSV is the output-replay leg of the three-level verification
discipline (solver → CLI → serialized artifact → re-read); its formatting
and identity columns must round-trip exactly.

**Guards against**: serialization formatting drift, dropped identity
columns, silent sink failures, and sink-observed state diverging from
`Runner` state.

**How to run**:

```sh
ctest --test-dir build -R "^emit$"
./build/test_emit
```

**Prerequisites**: standard build only; writes scratch files under
`/tmp` and removes them.

**Boundaries**: does not cover signal CSV logs (see `test_signal`),
routing wrappers (see `test_routing`), or transports (see the
link-transport page).

## test_routing

**What it checks**: serial stability (slots recycle, serials never reuse;
subscriptions survive respawn), 1:N fan-out with per-sink subscriptions
plus N:1 mix-down and mid-run reconfiguration without touching the
`Runner`, tick preservation for never-matching subscriptions (empty rows
forwarded, drops counted), `select_signals` slot-order preservation,
step-aligned `StrideSink` decimation with mid-run stride changes and
muting, the `kSum` golden (`sine + dc` closed form, spawn-time sample,
slot-order independence), sum rejections (unknown/empty operands, sum of
sums, batch-path sums), dangling-operand mid-run failure freezing
time/index with no emission, sink-failure isolation (a poisoned
`SignalsCsvSink` beside a healthy `Capture`), v3 serialization with the
v2 fallback loader (serials synthesized as slot indices), and sum quantity
validation.

**Purpose**: N:M edge-side routing must be exact and loud — subscriptions
address stable serials, combinators read fresh operands, and every drop or
misconfiguration is counted, never silent.

**Guards against**: serial reuse across respawn, route drops without
counters, stride misalignment, sum-operand staleness, and silent sink
failures altering sibling sinks.

**How to run**:

```sh
ctest --test-dir build -R routing
./build/test_routing
```

**Prerequisites**: standard build only.

**Boundaries**: does not cover the control plane that reconfigures these
pools at runtime (see `test_signal_control`) or the transports the
wrappers feed (see the link-transport page).

## test_output_health

**What it checks**: per-cause `PublisherStats` separation (`attempted` /
`published_local` / `over_bound` / `queue_full` / `backpressure` /
`pending_blocked` / `pending` / `abandoned` / `disabled` / `bytes`) with
exact accounting invariants, `format_publisher_health` labeling including
the `ACCOUNTING_MISMATCH` tripwire and the `local acceptance only`
disclaimer, SHM per-cause counting with a poisoned over-bound publisher
beside a recorder (sticky `kOutOfRange`, exact disabled counts), TCP
disconnect fault injection (peer closes immediately; disabled counted,
status `kInternal`), UDP over-bound refusal, and telemetry-failure
isolation (State and recorded snapshots bit-identical with and without the
poisoned sink).

**Purpose**: every delivery guarantee in `docs/delivery.md` rests on this
counter vocabulary; local acceptance must never be mistaken for remote
receipt, and telemetry failures must never perturb numerics.

**Guards against**: conflated drop counters, silent over-bound
truncation, telemetry failures altering simulation state, and health
lines claiming remote receipt.

**How to run**:

```sh
ctest --test-dir build -R output_health
./build/test_output_health
```

**Prerequisites**: standard build; real loopback sockets and POSIX SHM
segments (Linux). No network beyond loopback, no ROS, no hardware.

**Boundaries**: does not cover the wire codecs (see `test_link_protocol`,
`test_link_compact`) or receiver-side loss accounting (see
`test_link_receiver`).

## test_output_queue

**What it checks**: the bounded owning SPSC queue — empty transitions
with untouched outputs, FIFO wraparound with explicit `kFull` refusal
(never overwriting the block under read), byte-budget binding before slot
exhaustion with exact footprint math, producer/consumer stall behavior,
shutdown draining with `kClosed` admission refusal, post-enqueue
immutability (copies, never references), reusable slot storage with no
per-block allocation, and threaded producer/consumer FIFO exactness plus
bounded accounting (exact invariants, never exact drop counts under live
threads).

**Purpose**: the queue between the sim callback and worker threads must be
bounded, lossless-by-contract on refusal, and allocation-free on reuse —
it is the backpressure seam for the TCP worker path.

**Guards against**: silent overwrites on full, unbounded memory growth,
use-after-mutation through retained references, and data races (run under
Clang TSAN; GCC `libtsan` cannot start on this host).

**How to run**:

```sh
ctest --test-dir build -R output_queue
./build/test_output_queue
```

**Prerequisites**: standard build with threads. Race cover benefits from
a Clang TSAN build; plain runs still assert the exact single-threaded
invariants.

**Boundaries**: does not cover the TCP worker that consumes the queue
(see `test_tcp_worker`) or the sample-block codec (see
`test_link_sample_block`).

## test_paced

**What it checks**: `paced::validate`/`period_ns` edges (zero/negative/
non-finite rates, zero steps), paced-vs-unpaced `State` bit-equality for
projectile (chunks 1 and 7) and signal through a deterministic fake clock,
epoch-absolute sleep targets (exact multiples `3P, 6P, 9P, 10P`, no
`now()+period` re-anchoring drift), late-steps-counted-never-skipped
(every chunk late under an idle-burn hook, physics still bit-identical),
stop responsiveness (third chunk never runs; the wait aborts instead of
sleeping through), and the CLI `--paced/--rate` surface on `sim run` and
`sig run` (lateness counted not asserted on wall clock, orphan/bad rates
as usage errors, fast batch mode untouched).

**Purpose**: wall-clock pacing is display pacing only — simulation `dt`
and physics must be identical whether steps run back-to-back or against
absolute wall deadlines.

**Guards against**: deadline drift accumulation, skipped steps under
lateness, pacing changing numerics, and `--rate` without `--paced` being
accepted.

**How to run**:

```sh
ctest --test-dir build -R paced
./build/test_paced
```

**Prerequisites**: standard build; the fake-clock unit tests are fully
deterministic, while the CLI smoke uses real wall-clock rates (fast:
2 steps at 100 kHz).

**Boundaries**: does not cover chunked engine semantics without pacing
(see `test_run_config`) or GUI refresh decoupling.
