# Determinism

Which ordering contracts make WEngine runs reproducible, and what breaks
when each one is violated. Determinism is a project priority second only
to physical correctness: never trade it for convenience.

Related guides: [Architecture](architecture.md),
[Extension points](extension-points.md),
[Signal subsystem](../signals.md),
[Delivery guarantees](../delivery.md).

## Fixed-step sequencing

`core::Runner` owns `world::State` and advances it in fixed `dt` steps.
Batch runs call `run(N)` for exactly N steps; realtime loops call
`advance(frame_dt)`, which folds wall-clock time into the same fixed
chunks with an accumulator capped per call (the spiral-of-death guard).
Either way, the unit of progress is one fixed step: solver order is
registration order, solver updates happen before the clock moves, and a
failed step advances neither time nor index and emits nothing.

The failure latch matters here. A failed solver step freezes time and
index but does not undo solver work already done for that step, so the
live state is not a completed tick and must never be read as one.
Retrying the same Runner cannot complete the tick; only rebuilding (or
restoring) the run clears the latch. Clock-range pre-checks (index
overflow, non-finite next time, a `dt` too small to advance the clock)
are different: they run before any mutation, leave state bit-identical,
and are not latched.

What breaks if this is violated: changing operation order, iteration
order, solver update ordering, or timestep sequencing changes bit-level
output and invalidates every golden test downstream.

## Stable signal serials

Every signal source owns a stable serial: a monotonic counter per
`State` that is never reused, even across despawn and respawn. Pool
slots recycle; serials do not. Subscriptions, CSV `sig_id` columns,
`Frame` channels (`sig.<serial>.U`), and control-plane addressing all
use serials, so a resubscribe after respawn can never silently attach
to a different source.

What breaks if this is violated: subscriptions alias the wrong source,
CSV replay misattributes samples, and control commands retune a source
that no longer exists.

## Two-pass signal evaluation

Each signal step evaluates in slot order in two passes: generators
first, then `kSum` combinators, so sums always read fresh operand
values regardless of where the operands sit in the pool. Operands are
validated at configure time to be live, non-sum sources, and a sum over
a later-despawned operand fails the step loudly instead of reading a
stale slot. Noise advances its seeded RNG exactly once per step per
live slot, making the sequence a pure function of seed and step count,
independent of `dt`. Batch-spawned sources derive per-source seeds as
`seed + k`. Stopped sources contribute frozen samples with a frozen
RNG. Table and expression sources evaluate in the generator pass on the
same time base; Lua sources are evaluated by the isolated Lua extra
solver before the signal solver runs, so sums over Lua operands still
read fresh values.

What breaks if this is violated: sums read stale or slot-ordered
values, noise sequences depend on timing instead of step count, and
batch runs stop reproducing across machines.

## Snapshot timing

Snapshots stamped at time `T` carry `value(T)`: end-of-step evaluation.
The solver evaluates each source at `t_end = sim_time + dt`, the same
convention as the integrated particle positions, so a snapshot row is
always the value at its own timestamp, never the value at the start of
the step it just finished.

What breaks if this is violated: plotted waveforms shift by one step
relative to their timestamps, CSV replay disagrees with live display,
and signal/particle channels in the same snapshot describe different
moments.

## Units discipline

Physical quantities crossing subsystem boundaries use
`units::Value<Dim>`, and the engine stores SI base units internally.
Raw `double` values crossing a boundary must carry explicit meaning in
their names (`_V`, `_A`, `_s`). Suffix knowledge lives in exactly one
place (`units/scales.hpp`); the CLI and GUI parse through shared
helpers and never grow their own conversion tables. Signal quantities
follow the same single-table discipline: `signal/types.hpp` owns the
quantity table, and text parses only through the shared quantity-aware
entry point, so cross-dimension text such as `2mA` supplied as volts
fails loudly instead of silently converting. The hot path stays plain
SI `double` plus a tag; dimensional safety lives at the parse, display,
and control edges.

What breaks if this is violated: unit drift between frontends, silent
cross-dimension conversions, and goldens that pass in one unit
spelling and fail in another.

## Serialization versioning

Binary checkpoints (`save_state` / `load_state` in
`world/serialize.hpp`) are versioned with fallback loaders: the current
format version loads older files by synthesizing or zeroing the
columns those versions lacked (serials synthesize as slot indices in
the oldest signal format; files without a signal section load with
empty signals; newer custom-source and sequential columns zero out).
Kind bytes are append-only, so a stored kind always decodes to the
same source kind. Round-trips preserve freelist order, so exact
spawn-reuse behavior survives a save/load cycle. Corrupt input yields
`kInvalidArgument` and leaves the target cleared rather than half
loaded.

Snapshot CSV logs are output replay, never restart checkpoints: only
the binary format carries the full state needed to continue a run.

What breaks if this is violated: old checkpoints refuse to load (or
worse, load with reinterpreted kinds), and long-running experiments
lose their history on upgrade.

## Four clocks

Simulation time, emission, transport, and display each run on their own
clock, and waveform frequency is none of the plumbing clocks:

- Simulation `dt`: the physics step, fixed per run.
- Emitted sample rate: the Runner's global `emit_every` gate composed
  with per-sink `StrideSink` decimation. An output sees steps divisible
  by both gates, and decimation may alias a waveform.
- Wall-clock packet rate: paced runs space fixed steps against
  steady-clock deadlines; sockets send non-blocking with bounded
  buffers and counted drops.
- UI refresh rate: views render evicted, drop-oldest frames on their
  own cadence.

The full policy, including the three delivery classes (latest-state
viewing, ordered bounded sample streaming, complete recording) and the
per-edge counter vocabulary, lives in
[Delivery guarantees](../delivery.md); the signal-side design lives in
[Signal subsystem](../signals.md). The rule for this page is short:
never conflate these clocks. Waveform Hz is the physics; sample rate
is `1 / (dt * emit_every)` further decimated per sink; packet and
refresh rates are transport and display concerns that must never feed
back into the simulation.

What breaks if this is violated: display lag perturbs the physics,
decimation masquerades as signal distortion, and "faster publishing"
gets mistaken for a stronger delivery guarantee.

## Why this matters together

These contracts compose into the project's testing story. Fixed steps
plus pinned evaluation order plus stable serials plus end-of-step
timestamps mean a golden file recorded today replays tomorrow; units
discipline means it replays identically from any frontend spelling;
serialization fallbacks mean old checkpoints still load; and clock
separation means observing a run can never change it. When a golden
test fails, check this list before widening any tolerance: the project
rule is to derive expected behavior independently, compare against
hand-derived closed forms, and never widen a tolerance merely to make
a test pass.
