# Link transport

The `wengine-link` slice of the suite: the versioned wire protocol, the
POSIX SHM seqlock bus, the compact socket codec, the batched sample-block
codec, the CSV↔bus replay bridge, receiver-side loss accounting, socket
publishers, the worker-backed TCP path, and the UART HIL track. Golden
fixture bytes throughout are hand-written from the wire spec — never
produced by the encoder — so encode and decode agree with the document,
not just with each other. Exact-binary values assert with `==`, including
non-finite doubles and NaN payloads bit-exact.

## test_link_shm

**What it checks**: `run_headless` + publisher fan-out where every
emitted snapshot lands on a real POSIX segment with the latest decoding
bit-exact to final state, drop-oldest under a slow reader, loud failure
paths (bad open, over-bound frames dropped loudly with sticky status),
move-transfer of ownership, reader attach/latest semantics with foreign
segment refusal, exact version negotiation (version 0 reserved, mismatches
refused with the found version), reader-move, attached-reader survival
past publisher exit, stale-reader parking on same-name republish with
explicit re-attach, and the ordered-drain family (small-burst lossless,
overrun reporting with resume, latest-only accounting separation, empty
and post-exit drains).

**Purpose**: the SHM bus is the latest-state viewing path; publishers own
segment lifetime, readers never unlink, and same-name restarts require
re-attach — all pinned here against real segments.

**Guards against**: silent drops, reader/publisher lifetime confusion,
stale-mapping reads after republish, and version-mismatch interop.

**How to run**:

```sh
ctest --test-dir build -R link_shm
./build/test_link_shm
```

**Prerequisites**: standard build; POSIX SHM (`shm_open`/`mmap`), so
Linux/macOS only. Segments are PID-tagged and unlinked by the test.

**Boundaries**: does not cover the wire codec versions (see
`test_link_protocol`), socket transports (see `test_link_sockets`), or
the replay bridge (see `test_link_bridge`).

## test_link_protocol

**What it checks**: header init/check (version, capacities, domain size,
longest-legal tags; every corrupt/foreign mutation refused loudly at
open), the v3 compatibility matrix with v1/v2 rows still refusing
(different sizes/sync/identity), solver-agnostic payload encode/decode
round-trips (spatial items plus named scalar channels, inexact-binary
`0.1 + 0.2` proving bitwise fidelity), the particle adapter, over-bound
refusal without truncation, seqlock states, ring and lag accounting, lag
stats, header counters, staged-unready init, and concurrent attach plus
seqlock hammering (single and multi-reader) for race cover.

**Purpose**: the versioned seqlock protocol is the interop contract every
endpoint and external reader shares; exact-version attach and loud
refusal keep mixed-version buses from corrupting silently.

**Guards against**: version/capacity/domain mismatches accepted silently,
over-bound truncation, torn seqlock reads, and attach races.

**How to run**:

```sh
ctest --test-dir build -R link_protocol
./build/test_link_protocol
```

**Prerequisites**: standard build with threads for the concurrency
hammers. Race cover benefits from a Clang TSAN build (GCC `libtsan`
cannot start on this host).

**Boundaries**: does not cover SHM lifetimes (see `test_link_shm`) or
the compact socket framing (see `test_link_compact`).

## test_link_compact

**What it checks**: the 118-byte golden fixture for one signal source
(hand-written per spec: `WLV2` magic, schema 3, run/seq/step/time,
`sig.count` + `sig.0.U` channels — 577x smaller than a 76328-byte TCP
full frame, with `sizeof(LinkFrame)` pinned by `static_assert`), empty
records valid at header size, one-signal shapes bounded well below a KiB
with the codec bound tied to the max UDP payload, round-trip sweeps,
max-shape encoding, over-bound refusal, malformed-input refusal, and
v1/v2 record refusal.

**Purpose**: the compact codec is the bandwidth story for sockets — one
codec for TCP and UDP, every encodable record fitting a maximum-size
datagram.

**Guards against**: encoder/spec divergence, size-bound creep past the
UDP budget, and legacy wire versions decoding as current.

**How to run**:

```sh
ctest --test-dir build -R link_compact
./build/test_link_compact
```

**Prerequisites**: standard build only.

**Boundaries**: does not cover batching multiple ticks (see
`test_link_sample_block`) or live socket behavior (see
`test_link_sockets`).

## test_link_bridge

**What it checks**: CSV parse fidelity (bitwise `0.1 + 0.2` round-trip;
empty step-2 snapshots yield only the steps present; header-only logs
valid-and-empty), the 13-case parse rejection battery (short/long rows,
non-numeric, negative/plus-prefixed steps, trailing garbage, empty
lines, decreasing steps, time drift, slot gaps/reorders — all with the
error string set and output cleared), the `Frame`↔`Snapshot` inverse law
within CSV bounds (z-columns and named channels dropped by format, exact
within what CSV carries), and the task acceptance: CSV → bus → reader
matches the source log and a bus recording of the playback is
byte-identical to the source file.

**Purpose**: snapshot CSV is output replay, and the bridge proves the
loop closes — logs replay onto the bus losslessly and re-record
identically.

**Guards against**: parse leniency (gaps, reorders, drift accepted),
inverse-law breaks, and replay/record divergence.

**How to run**:

```sh
ctest --test-dir build -R link_bridge
./build/test_link_bridge
```

**Prerequisites**: standard build; POSIX SHM plus `/tmp` scratch files
(removed after the run).

**Boundaries**: does not cover checkpoints (binary `save_state`, see
`test_world`) or live publisher fan-out (see `test_link_shm`).

## test_link_receiver

**What it checks**: receiver loss accounting with hand-derived
expectations — reorder-window fill (`1,3,2,3,4` → four unique, one
reordered, one duplicate, no interior loss), seq-0 ignored, window-close
confirmation (jump to 100: 63 pending + 35 confirmed; stale seqs refused),
session reset on publisher run change with new-run pre-attach, per-sender
cursors for interleaved UDP sources, pre-attach vs skipped vs confirmed
separation (`receiver_observe_latest` jumps count as skipped, never
confirmed), and trailing-loss gating (unknown without an authoritative
final, exact once authoritative).

**Purpose**: receivers must account every gap exactly — pre-attach
history, intentional latest-only skips, and confirmed loss are three
different things and must never be conflated.

**Guards against**: duplicate double-counting, stale fills, session
leakage across publisher restarts, and trailing loss inferred without
authority.

**How to run**:

```sh
ctest --test-dir build -R link_receiver
./build/test_link_receiver
```

**Prerequisites**: standard build only; pure accounting, no sockets.

**Boundaries**: does not cover the publisher side or the lag stats on the
SHM path (see `test_link_protocol`, `test_link_shm`).

## test_link_sample_block

**What it checks**: the 156-byte golden block (hand-written per spec: two
channels, two samples, `encoded_size` agreement), empty selections still
recording every tick (step/time preserved, no values), schema changes
sealing blocks, partial blocks with count seals, flatten batch-vs-stream
equality, above-`LinkFrame`-capacity channels, over-bound and malformed
refusals, and byte-budget seals — all round-trips asserting `==` with NaN
payloads bit-exact.

**Purpose**: sample blocks batch consecutive signal ticks without
decimating values — the complete-recording path that complements the
latest-state bus.

**Guards against**: sample loss at schema changes, partial-block drops,
budget overruns, and NaN payload corruption.

**How to run**:

```sh
ctest --test-dir build -R link_sample_block
./build/test_link_sample_block
```

**Prerequisites**: standard build only.

**Boundaries**: does not cover the queue that carries sealed blocks (see
`test_output_queue`) or the worker that serves them (see
`test_tcp_worker`).

## test_link_sockets

**What it checks**: endpoint parsing, hello/version gates, TCP round-trips
against engine truth, backpressure drops without stalling (200-particle
floods past kernel buffering with a never-read peer), over-bound
poisoning, UDP round-trips, sub-Hz TCP pacing, 100 kHz TCP and UDP soaks,
service-pending idle behavior (no dummy snapshots), finish
completion/timeout/disconnect accounting, empty and max records, stall/
pending/finish interplay, and the UDP payload budget (representative
sizes, `need=` diagnostics on indivisible records).

**Purpose**: sockets are the ordered bounded sample-streaming path —
transient backpressure counts in `dropped_send`, never stalls the sim,
with loopback peers standing in for Zephyr-side readers.

**Guards against**: stalls under backpressure, silent over-bound sends,
hello/version bypass, finish-accounting leaks, and budget overruns.

**How to run**:

```console
$ ctest --test-dir build -R link_sockets
$ ./build/test_link_sockets
```

**Prerequisites**: standard build; real loopback TCP/UDP sockets with
10–15 s I/O timeouts bounding stalls. No external network or hardware.

**Boundaries**: does not cover the worker-threaded TCP path (see
`test_tcp_worker`) or UART framing (see `test_uart_codec`).

## test_tcp_worker

**What it checks**: the mixed framed/block stream decoder with trailing
partials, ordered loopback exactness (values and step IDs rebuilt through
the `record_stream` batch decoder against the sealed-block stream),
latest-mode coalescing under pressure, stalled-sibling isolation (one
stalled reader never blocks another; final `State` still exact), and
bounded shutdown accounting with the block/sample conservation law
(`pushed == published + dropped + coalesced + abandoned + queued +
held`) checked at rest and drained.

**Purpose**: the worker moves encoding and serving off the sim callback —
which only fills/seals/enqueues owned blocks — so socket pressure can
never stall physics.

**Guards against**: sim-callback blocking on sockets, cross-client
interference, conservation leaks at shutdown, and latest-mode value
staleness.

**How to run**:

```sh
ctest --test-dir build -R tcp_worker
./build/test_tcp_worker
```

**Prerequisites**: standard build with threads; real loopback TCP
sockets. Pressure comes from byte volumes past kernel buffers with fully
stalled readers — no timing thresholds asserted.

**Boundaries**: does not cover the queue internals (see
`test_output_queue`) or the non-worker socket publishers (see
`test_link_sockets`).

## test_uart_codec

**What it checks**: COBS vectors (empty, zero-free, embedded zeros,
254/255-byte block splits, all-zeros, refusal on embedded NUL/overrun/
truncation, 600-byte round-trip with bounded overhead), C-encoder parity
(the Zephyr `wuart_cobs_encode` byte-identical on a corpus, refusing
empty/tiny outputs without truncating), endpoint/mode parsing, stream
line goldens with the shared Zephyr C parsers cross-checked against the
C++ builders, ASCII/COBS demux with garbage resync, and the compact v2
record decoder fed with real encoder bytes.

**Purpose**: the UART HIL track shares C parsers with Zephyr firmware —
host and firmware must decode identically, and COBS framing must resync
after garbage.

**Guards against**: host/firmware codec divergence, COBS desync on
corrupt bytes, and silent truncation on small buffers.

**How to run**:

```sh
ctest --test-dir build -R uart_codec
./build/test_uart_codec
```

**Prerequisites**: standard build (the Zephyr C sources compile as C++
here for the host cross-check). No serial hardware needed.

**Boundaries**: does not cover the publisher or pty loopback (see
`test_uart_pub`).

## test_uart_pub

**What it checks**: ASCII and COBS stream exactness over real pty pairs
(master peer, termios-configured slave exactly like `/dev/ttyUSB0`),
control ASCII round-trips through `solvers::ControlBus`, COBS control
round-trips with resync, flood drops without stalls, a decimated soak
through the real CLI, control-UART smoke, and CLI usage errors.

**Purpose**: the UART publisher and control path work over genuine ttys
without any embedded hardware — `posix_openpt` gives a faithful loopback
stand-in.

**Guards against**: stream corruption over real ttys, control round-trip
breaks, stalls under flood, and CLI/UART plumbing divergence.

**How to run**:

```sh
ctest --test-dir build -R uart_pub
./build/test_uart_pub
```

**Prerequisites**: standard build; pty support (`posix_openpt`,
`/dev/pts`) — Linux CI has it, bare containers without `devpts` do not,
in which case the open asserts fail loudly rather than skip.

**Boundaries**: does not cover TCP/UDP transports (see
`test_link_sockets`) or the engine-side control verbs without transport
(see `test_signal_control`).
