# Transport and signal benchmarks

Snapshot, bus, socket, batching, and signal-eval costs. Delivery vocabulary
throughout follows `docs/delivery.md`: generated vs. published vs.
received/missed vs. tail.

## bench_link

**Measured path**: end-to-end SHM bus goodput/latency across six scenarios —
`solo` (solver only), `snap` (solver + snapshot + encode micro-loop),
`flood`, `paced` (1 kHz driver), `stalled` (late attach), `fanout` — for a
particle payload and a one-signal payload.

**Why it matters**: pins the latest-state viewing contract (writer drops
oldest on full, lag via `frame_seq` gaps) and localizes cost: solver vs.
snapshot vs. seqlock publish.

**How to run**:

```sh
./build-bench/bench_link --mode flood --steps 200 --particles 8
./build-bench/bench_link --payload signal --mode flood --steps 200
```

Flags `--payload signal|particle`, `--particles N`, `--steps N`,
`--mode all|solo|snap|flood|paced|stalled|fanout`, `--rate HZ`; legacy
positional form `bench_link [particles] [steps]`. The binary sets up its own
segment (`/wengine-bench-link-<pid>-*`) and spawns its own active reader
thread per scenario; no caller-provided bus needed. `all` clamps the paced
leg to 2 s and repeats flood/paced/stalled on the second payload.

**Interpretation**: header line reports compiler, `link_frame_bytes`,
wire version, `ndebug`. Each row gives `attempted/goodput per s`,
`published`, `dropped_over_bound`, `received/missed/max_gap`, `tail`,
`decoded/bad`, `p50/p99_age_us`, `cpu_s/pct`, `checksum`. Flood adds
`ns/frame`; paced adds `late_steps/max_late_us`; stalled adds a
`stalled reader` line (loss is policy: latest-only view of a full ring).

**Pitfalls**: inputs are `argc`-seeded xorshift worlds plus a relaxed
`SendTable` timestamped per `frame_seq`; checksums and decode validation
hold the stream against DCE. Flood rows intentionally show missed frames —
that is backpressure visibility, not failure.

## bench_link_sockets

**Measured path**: loopback TCP/UDP goodput/latency per transport and payload:
flood, paced 1 kHz, stalled (backpressure/loss), plus transport-independent
`solo`/`snap` and a TCP+UDP fanout. Reader validates values against a
deterministic per-step pattern, not just sequence IDs.

**Why it matters**: separates socket backpressure (`dropped_send`,
`pending`) from SHM seqlock behavior; the ordered-vs-lossy comparison point
for the delivery-classes policy.

**How to run**:

```sh
./build-bench/bench_link_sockets --transport tcp --frames 200 --items 8 --mode flood
./build-bench/bench_link_sockets --transport udp --frames 200 --items 8 --mode flood
```

Flags `--transport tcp|udp|all`, `--frames N`, `--items N`,
`--payload signal|particle`, `--mode ...`, `--rate HZ`; legacy positional
`bench_link_sockets [tcp|udp] [frames] [items]`. Listeners/receivers are
created on loopback ephemeral ports in-process; reader threads join before
descriptors close. No external network setup needed.

**Interpretation**: header gives compiler, `ndebug`, `tcp_wire_max`,
socket schema. Rows mirror `bench_link` plus `dropped_send`,
`pending_blocked`, `disabled`, `pending`, `value_mismatch`, byte counts, and
`p50/p99_age_us`. Stalled TCP checks a pending-tail accounting identity;
stalled UDP drains whatever the kernel buffer retained.

**Pitfalls**: the `Pattern` base vectors are runtime xorshift (seeded from
frame count) with per-step `1e-6` stamping, so frames are never
constant-foldable; `send_ns` uses relaxed atomics (the socket handoff is the
real edge). Timing is wall/CPU around the publish loop — compare rates, not
absolute latencies, across machines.

## bench_sample_block

**Measured path**: batching cost for consecutive ticks: builder push/flush
over `ticks` synthetic signal snapshots, then per-block wire encode;
contrasted against per-step compact-frame bytes.

**Why it matters**: quantifies the bytes/sample and encode-cost win of
`SampleBlock` batching that the TCP worker relies on.

**How to run**:

```sh
./build-bench/bench_sample_block 64 4 16
```

Positional args: ticks, channels, samples-per-block (defaults 640 8 64).

**Interpretation**: `ticks/channels/per_block/blocks`, then `frames:
total_bytes bytes/sample`, `blocks: total_bytes bytes/sample bytes/value`,
`ratio(frame/block)`, `build_us (us/tick)`, `encode_us`, and
`owned_blocks`. Higher ratio favors batching; `us/tick` is the builder cost.

**Pitfalls**: values derive from a step/channel modular pattern times a
`volatile`-sourced `wobble` (plus 0.5 when `argc > 4`), so the tick stream
is opaque to the optimizer. Over-bound channel counts print the
`over-bound ... not encodable per step` line instead of frame bytes.

## bench_tcp_worker

**Measured path**: worker-backed TCP output: per-`on_snapshot` callback
latency (driver deltas p50/max cross-checked with worker counters) plus
end-to-end goodput through a validating reader, in `flood` (ordered/latest
synthetic ticks) and `engine` (live `signal` `run_headless` beside a solo
reference) scenarios.

**Why it matters**: the metric the worker exists to minimize is sim-callback
latency, and the engine case proves networking leaves numerics bit-identical
(`state_equal=yes`) with a gap-free block stream.

**How to run**:

```sh
./build-bench/bench_tcp_worker 500 4 ordered
```

Positional args: snapshots, sources, `ordered|latest` (defaults 20000 8
ordered); the `engine` case always runs 5000 steps after the flood. Listener
and reader are loopback in-process; nothing to provision.

**Interpretation**: flood prints `callback_us` (p50/max/driver-side plus
`worker_max_ns`), `wall_s/cpu_s/bytes`, `received` blocks/samples with
`value_errors/step_gaps/goodput_samples_per_s`, and `worker:` attempted,
published, queue_full, coalesced, send/over-bound drops, `finish`. Latest
mode coalesces by design (gaps expected); ordered must be gap-free into its
absorbent queue. Engine prints `run/ref/state_equal`, stream block/sample
counts, and worker health; success requires zero gaps and zero drops.

**Pitfalls**: tick values are `step * 10 + c` (self-describing, verified per
value by the reader); a `volatile` wobble guard keeps the pattern opaque.
The 100 ms start settle and 200 ms tail settle are indicative pacing, not
completeness proof — tests own exactness.

## bench_signal

**Measured path**: `Runner` + `SignalSolver` over N live sine sources for M
fixed steps at dt=0.001.

**Why it matters**: the signal-eval counterpart to `bench_projectile`;
baseline per-source-step cost before routing, capture, or transport.

**How to run**:

```sh
./build-bench/bench_signal 32 200
```

Positional args: sources then steps (defaults 256 x 2000).

**Interpretation**: `bench_signal: <s> sources x <m> steps,
<x> ns/source-step (checksum=<c>)`; compare `ns/source-step`. Checksum sums
final snapshot signal values.

**Pitfalls**: frequencies/amplitudes/phases from an `argc`-seeded xorshift
PRNG; `do_not_optimize` on the snapshot sum blocks DCE. Literal params
would fold the sine eval loop.

## bench_signal_scale

**Measured path**: pool-layout scaling risks: dense-vs-sparse worlds, serial
lookup sweep, kSum fan-in sweep, subset-subscriber fan-out, custom-source
churn (expression/table/Lua), a mixed-world separate-native/Lua/snapshot/
filter/serialize split, and a derived crossover summary.

**Why it matters**: measures the linear-scan and retained-store costs
(`find_serial`, per-slot columns, uncompacted table/Lua stores) that decide
future indexing work; the guide line maps each slope to its candidate fix.

**How to run**:

```sh
./build-bench/bench_signal_scale 32 4 200 4 4 2 2
```

Positional args: live gap steps sums fanin subs customs (defaults
256 16 2000 8 8 4 8); out-of-range values clamp with a notice.

**Interpretation**: one line per scenario with `ns/step`, `ns/lookup`,
`ns/tick`, `ns/pair`, byte counts, and order-sensitive FNV + value-sum
checksums (dense/sparse twins must match). The `crossover:` lines report
per-slot scan vs. per-source eval, `gap*`, lookup slope with `cap@1us`,
per-operand fan-in slope, and retained bytes/round; the `guide:` line maps
them to fixes (active-slot iteration, serial index, cold-column split,
store compaction).

**Pitfalls**: all waveform params are runtime xorshift (seeded from `argc`);
twin builds from equal seeds are bit-identical, so checksum mismatch is a
real defect signal. Per-scenario step counts derive from `steps` with
documented divisors — compare slopes, not raw step times.

## bench_snapshot

**Measured path**: snapshot-capture cost isolated from solver and transport:
warmed `fill_snapshot` reuse, owning `take_snapshot` control, cold-buffer
growth, and Runner fan-out to 1 vs. 4 counting sinks — with a global
allocation counter over the timed regions.

**Why it matters**: proves steady-state capture is allocation-free and
localizes multi-sink emission cost; the zero-alloc expectation is a
determinism/hot-path invariant.

**How to run**:

```sh
./build-bench/bench_snapshot 32 32 2000
```

Positional args: particles, sources, captures (defaults 256 256 20000).

**Interpretation**: `capture-reuse` reports `ns/capture allocs` (expect 0
after warmup); `capture-owning` is the fresh-vectors control (higher allocs
expected); `growth` reports cold-fill allocs (bounded ~7 vectors) vs.
steady fills (0); `fanout-N` reports `ns/emit-step allocs`. Checksums pin
each region against DCE.

**Pitfalls**: populations from an `argc`-seeded xorshift PRNG with the sim
clock advanced per capture, so successive fills observe new data and cannot
be CSE-merged. The global `operator new` counter only counts while
`g_counting` is set — allocs outside timed regions are invisible by design.
