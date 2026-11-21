# Contract: Timing Semantics (014)

The behavioral physics of the delay-honoring mode — what the wheel
computes and why the results are trustworthy. Engine API shapes
live in [engine-seam.md](engine-seam.md); CLI surfaces in
[cli-verbs.md](cli-verbs.md). Decisions D2–D6/D9 of
[research.md](../research.md) pin everything below.

## Time axis

- Whole picoseconds, `u64`, integer arithmetic only, checked adds,
  loud overflow refusal (never wraps, never clamps).
- A gate's transport delay = its stored `propagation_delay_ps`
  exactly as annotated (no rounding, no clamping, no rewrite).
- Un-annotated gate (field 0) = the mode's **unit: 1 ps** (the
  field's smallest storable nonzero value). Zero stays
  "un-annotated" per 013's pinned convention; no gate is faster
  than the unit; the unit is never written back to storage.
- Windows are relative: each window's anchor (a solve assignment or
  a clock edge) is `t=0`; no cross-window absolute time exists.

## Well-foundedness (the no-oscillation guarantee)

Every effect lands strictly after its cause (≥ 1 ps). Within one
picosecond bucket no event can trigger another at the same bucket
time, so buckets never self-interact and time strictly advances.
Combined with the wire-time rejection of combinational cycles,
every window provably drains within the event budget; a budget or
overflow breach is a loud runtime refusal, never a hang and never
silent truncation.

## Transport propagation

- Every input-net transition schedules each consuming gate's
  recomputation at (transition time + gate delay). Per-gate delay
  applies uniformly to all of the gate's input paths.
- **No cancellation, no filtering**: a pulse the physics would
  swallow still appears (the mode observes; it never models
  absorption).
- Duplicate schedules for one (gate, time) collapse into a single
  evaluation reading nets as of that time; evaluations that
  reproduce the current value emit no event.
- Multi-driver (shared) nets: each driver transition re-resolves
  the net through the shipped four-value funnel at that instant;
  transient contention windows appear in the trace through the
  drivers' entries and close by drain; contention counting rides
  the unchanged settled path as today.

## Four-state evaluation during transients (reference: IEEE 1364)

| Gate | Rule |
|---|---|
| AND / NAND | any input 0 → 0; else any unknown → X; else 1 (NAND inverts, X stays X) |
| OR / NOR | any input 1 → 1; else any unknown → X; else 0 (NOR inverts) |
| XOR / XNOR | any unknown input → X (parity has no dominant value) |
| NOT / BUF | pass X through; read Z as X |
| TBUF | released enable → Z (the only Z producer); enabled → data with unknowns read as X |

Z at a gate input reads as X for evaluation (Z is a net state; only
the funnel and TBUF produce it). These tables are the test
reference: goldens derive from them, never from implementation
readback. **Settled contracts are untouched**: a settled X/Z at a
level consumer or a settled S=R=1 net refuses exactly as today —
transient windows only ever *show* X.

## Sequential semantics

- Annotation on an edge-triggered element (DFF/JK/T) = **clock-to-Q**:
  Q (and NQ, same timestamp, never lagging) transition that many
  picoseconds after the edge that caused the commit.
- Annotation on a level element (DLATCH/SRLATCH) = the output's
  response delay: a latch Q transition caused by a mid-window net
  change lands at (arrival + annotation).
- The edge itself is the step call (the module's clock model,
  unchanged): no clock-path delays in this feature; the window
  observes what the edge caused.
- Edges sample the drained pre-edge state; the pre-edge settle
  (async apply + level settle) rides the unchanged zero-delay
  machinery. Therefore **stepping trajectories match zero-delay
  step-for-step** — the trace exposes arrival ordering (a D change
  landing `k` ps after the edge that caused it is the race
  observable), never a different state sequence. Setup/hold
  verdicts are the user's judgment (checking is out of scope).
- A transient S=R=1 window (differential arrival, settles legal)
  renders X at those instants; the settled illegal net refuses as
  today.

## Windows and seeds

| Surface | Anchor (t=0) | Pre-state | Seeds |
|---|---|---|---|
| `solve --with-delays` | the applied assignment | zero-delay solve of the all-zero vector (the default `solve` already echoes) | block-input nets whose applied value differs |
| `step --with-delays` | each clock edge (one window per edge; `edge k:` separators for N edges) | unchanged async/level settle, pre-edge state | per-edge Q diffs of stepped flops (NQ pairs the timestamp) |
| `truth --with-delays` | each row's assignment (same row convention) | all-zero vector | row bits that differ from zero |

Parity invariants (pinned by test, true by construction in user
output): wheel end-state == zero-delay solve result; step
trajectories equal across modes; repeated invocations
byte-identical.

## Budget

Trace capacity is a constant loud cap (`kMaxTimingEvents`);
transport propagation through reconvergent fan-out can multiply
(gate × arrival-time) pairs far beyond gate count, so the cap is a
genuine guard. Breach refuses loudly naming the cap; a test drives
a pathological annotated net over it.
