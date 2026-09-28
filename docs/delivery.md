# Delivery guarantees and clocks (PHYSI-28)

Decision record. Status: locked 2026-09-25.

Scope: what the current transports promise, which clock each rate
belongs to, and what complete capture requires. No engine behavior
changes; this documents `core::Runner`, `emit`, `link`, GUI history,
and CSV replay as built.

## 1. Delivery classes

There are exactly three, with different overflow policies.
No single transport provides all three at once.

### A. Latest-state viewing (live displays)

Mechanism: SHM seqlock ring (`link/protocol.hpp`, `ShmPublisher`),
GUI `FrameHistory` (`apps/gui/views/frame_source.hpp`).

- Writer drops oldest on full (ring overwrite). Latest-only readers
  take the newest frame and never drain a backlog; ordered readers
  (`ShmReader::poll_next` / `read_batch`) may instead drain the
  retained window (up to `kSlotCount` frames) in ascending order with a
  caller work limit. Bursts within the ring drain losslessly; larger
  bursts report the exact overwritten prefix (`skipped`) and resume
  from the oldest retained frame.
- Intentional coalescing (a latest-only reader skipping to newest) is
  normal operation, not loss to fix. Ordered-drain skips are ring
  overruns (real loss), reported loudly with the same `frame_seq`-gap
  vocabulary and a different interpretation. Both absorb bounded
  jitter only and cannot guarantee complete capture when the reader is
  persistently slower.
- Overflow accounting: per-frame `frame_seq` gaps via
  `reader_missed()` / `LagStats` (`received`, `missed`, `max_gap`).
  First observation counts pre-attach frames as `missed`.
- Receiver lag never alters sim state or timing.

### B. Ordered bounded sample streaming (live telemetry)

Mechanism: `TcpPublisher` / `UdpPublisher`
(`link/socket_publisher.hpp`), Python receivers under `examples/`.

- Bounded and non-blocking (`copy-and-return` in `on_snapshot`):
  TCP uses the kernel send buffer (`kSocketSendBuffer`, 256 KiB
  default, a few full frames); UDP drops a backpressured datagram
  whole. A partially accepted TCP frame is completed before any
  later frame is sent, otherwise stream framing corrupts.
- Overflow policy: transient backpressure drops the frame and counts
  `PublisherStats::dropped_send` without poisoning the publisher;
  over-bound frames are refused on every transport identically
  (`dropped_over_bound`, sticky `kOutOfRange`, never silently
  truncated); dead peer is sticky `kInternal`.
- Readers account loss with `LagStats` over `frame_seq`, same
  vocabulary as SHM. UDP datagrams self-describe
  (`magic/version/domain/frame_seq/clock/counts`); TCP gates once at
  hello (`magic/version/domain`) then length-prefixed frames.
- This class absorbs bounded jitter only. It does not promise every
  sample to an arbitrarily slow consumer.

### C. Complete recording (offline capture)

Mechanism today: `CsvSink` / `SignalsCsvSink` files
(`emit/csv_sink.hpp`, `emit/signal_sink.hpp`); block recorder is a
separate planned task.

- Policy: the application driver owns completeness. Options are
  pace/backpressure at the driver (slow wall-clock progress while
  keeping numerical `dt` and step order fixed), spool within a
  configured memory/disk budget, or fail loudly with an
  incomplete-artifact status. Silent gaps are never acceptable here.
- I/O errors are sticky (`kInternal`, further snapshots skipped,
  `flush()` reports). Success means the footer/row count proves no
  gaps; disk/error injection must exit nonzero.
- Live display output keeps its own stride/coalescing policy (class A)
  alongside a full-rate recorder (class C).

## 2. Clocks (four rates plus waveform frequency)

These are independent quantities; do not conflate them.

1. Simulation clock: fixed numerical `dt` (`RunnerConfig::dt`),
   `step_index` / `sim_time` in `world::State`. `run(N)` advances
   exactly N steps with no wall-clock pacing; `advance(frame_dt)`
   folds wall time into fixed steps with a `max_substeps` stall guard.
   Determinism lives here only.
2. Emitted sample rate: `1 / (dt * emit_every)`, further decimated per
   sink by `StrideSink` (`step % stride == 0`) and filtered per sink
   by `SignalSubsetSink` (serial allowlist, rows only — every tick is
   still forwarded so `LagStats` stays exact). Decimation changes
   sampled bandwidth and may alias a waveform; it reduces packet
   overhead without preserving skipped samples.
3. Wall-clock packet rate: how fast the publisher actually emits onto
   SHM/TCP/UDP (frames/s, bytes/s). Set by driver pacing, transport
   capacity, and peer readiness — not by `dt`. Flood rates
   (~10^4–10^5 frames/s loopback) are backpressure probes, not network
   promises; see `docs/signals.md` sec. 5 budgets.
4. UI refresh rate: GUI render loop / plot timer (e.g. 30–60 Hz) over
   `FrameHistory` (bounded drop-oldest, capacity 1024). Display
   eviction/coalescing is counted separately from transport gaps.

Waveform frequency (`--sig-freq`, e.g. 1 kHz sine) is none of the
above: it is the physical signal being simulated. Sampling it at
`dt = 0.1 ms` (10 kHz sample rate) vs. streaming it over a 1 kHz
packet budget vs. drawing it at 60 Hz are three separate choices;
undersampling aliases regardless of transport.

## 3. What cannot be guaranteed

Finite memory + a non-blocking producer + an arbitrarily slow receiver
cannot guarantee zero loss. Any claim of lossless delivery to an
unboundedly slow consumer with bounded buffers is rejected by design.
Complete capture must therefore do one of: pace the driver,
spool within an explicit budget, or fail loudly. Faster publishing
alone is not a success metric; goodput, latency, CPU, bytes, and
losses by cause are (see the link benchmark tasks).

## 4. Ownership, startup, shutdown

- Publishers are `emit::ISink`s built and owned at the app edge
  (CLI/GUI), never in `RunConfig` or inside the engine. Whoever owns
  a run loop re-attaches its non-owning observers after every
  rebuild; sinks stay unaware of rebuilds.
- SHM segment lifetime equals publisher lifetime (create fresh on
  construction, unlink on destruction). Readers never unlink; a
  same-name publisher restart needs explicit re-attach (stale POSIX
  mapping, by design).
- TCP actively connects to the listening reader (`host:port`); UDP
  sends connected datagrams with no handshake. Wire versions
  exact-match at attach: loud refusal with found version, never
  silent fallback; version 0 is the mid-construction marker, never
  assigned.
- Publish path performs no syscalls or I/O: adapt, encode, seqlock
  copy (SHM) or non-blocking send (sockets). Failures are sticky
  `Status`, never throws out of `on_snapshot`.
- Shutdown: `--forever` stops at a step boundary, then sinks flush
  and close normally. A stalled TCP peer cannot extend shutdown
  unboundedly: pending bytes are accounted (`has_pending_frame`) and
  abandoned-at-close is reported, not silently kept.

## 5. Counter vocabulary

Counts are per edge object, monotonic within a run, never shared
across threads without external sync.

- Generated: snapshots taken by `Runner` (step-gated by
  `emit_every`); the source of truth every downstream count is cut
  against.
- Decimated (intentional, not loss): `StrideSink::dropped()` (stride
  skips, `0` = muted), `SignalSubsetSink::dropped()` (unknown/
  despawned serials; muted = everything), `matched()` (forwarded
  snapshots), `forwarded_rows()` (signal rows delivered).
- Over-bound (loud refusal): `PublisherStats::dropped_over_bound`
  (every transport agrees: the shared `encode_frame` gate runs first).
- Socket backpressure (transient, counted, non-sticky):
  `PublisherStats::dropped_send` (TCP zero-byte EAGAIN + UDP datagram
  refusal); TCP frames dropped while the depth-1 pending tail is still
  blocked count separately as `dropped_pending_blocked` (previously
  folded into `dropped_send`). `dropped_queue_full` is reserved 0 until
  the bounded owning queue lands.
- Attempted/disabled/bytes: `attempted` counts every `on_snapshot` call;
  `disabled_after_error` counts snapshots ignored once sticky (including
  the frame that triggered the sticky failure); `bytes_accepted` counts
  wire bytes accepted locally for effective bytes/s. At rest
  `attempted == published + over_bound + backpressure + pending_blocked
  + queue_full + disabled (+ 1 while a TCP frame is pending)` — see
  `link/output_report.hpp::output_accounting_ok`. Intentional
  stride/subset skips live in the routing wrappers, never here; pending
  (`has_pending_frame`/`pending_bytes`) and abandoned-at-close are edge
  state reported at finish/close. `sim run` / `sig run` print the
  per-cause line on stderr per chunk (long runs) and on stdout at
  completion only with `--emit-health`; default output stays concise.
  Publish failures are always loud, with or without the flag.
- Published frames:
  `PublisherStats::published` (mirrors SHM header `publish_count`).
- Received/observed: `LagStats::received`, `missed`, `max_gap`
  (from `frame_seq` gaps; pre-attach history counts as `missed`;
  latest-only skips and ring overruns use the same counters with
  different interpretation per class A/B). UDP/sample receivers that
  must tolerate reorder use `link/receiver.hpp` instead
  (`SequenceTracker`/`SessionReceiver`, bounded 64-seq window):
  gaps stay `pending` until they fill (`reordered`) or age out
  (`confirmed` loss); `duplicates`/`stale`, per-session `run_id`
  cursors (one tracker per sender, keyed by source address),
  separated `pre_attach` history vs intentional `skipped` jumps
  (`observe_latest`), and trailing loss only with an authoritative
  expected final sequence (`receiver_trailing`, else unknown).
- Recorded: `CsvSink::rows_written()` /
  `SignalsCsvSink::rows_written()` plus sticky `status()`; success +
  footer count is the completeness proof.
- Rendered: `FrameHistory::size()` / capacity evictions on the display
  side; eviction is display coalescing, never transport loss.

Local socket acceptance (`published_local`, formerly `sent` in benches)
is not remote receipt. Remote receipt
is unknown unless a receiver reports it via its own `LagStats`.

## 6. CSV is output replay, not a restart checkpoint

- Snapshot CSV logs (`step,time,slot,...` /
  `step,time,sig_id,value,quantity`, `max_digits10`, exact round-trip)
  record emitted output for replay (`link_bridge play/record`,
  byte-identical round-trips). Empty snapshots leave no rows, so they
  do not survive a CSV round trip by design.
- Authoritative restart state is the versioned binary checkpoint
  (`world/serialize.hpp`, `save_state` / `load_state` with fallback
  loaders). A CSV replay is never a substitute for a checkpoint, and
  a checkpoint is never a substitute for a complete recording.
