# Feature Specification: Opt-In Delay-Honoring Evaluator for Glitch/Race Observation

**Feature Branch**: `014-delay-honoring-evaluator`

**Created**: 2026-11-21

**Status**: Draft

**Input**: User description: "What: Opt-in delay-honoring evaluator for glitch/race observation without touching the zero-delay fast path. Why: Zero-delay hides hazards; an explicit timing mode exposes them while keeping determinism by default."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - See the hidden pulse (Priority: P1)

A user studying a hazard net — one input feeding both a direct path
and an inverter chain into the same gate — selects the module's timing
mode on the single-assignment evaluation. The report becomes an
ordered event trace: every observed net's transitions stamped as
whole-picosecond offsets from the applied assignment, ripples ordered
by arrival, through transient values, ending at the settled state.
The classic pulse appears: three inverters against a direct path show
a pulse whose width is exactly the hand-derived path difference
(3 x unit); the zero-delay evaluation of the same net shows only the
settled value and never hints the pulse existed. Every gate takes its
stored whole-picosecond annotation exactly as annotated; un-annotated
gates take the mode's unit delay. The settled values equal the
zero-delay result for the same assignment — the trace adds
transients, never different endpoints.

**Why this priority**: This is the feature's entire point and the
Why verbatim: zero-delay hides hazards, and this is the observation
that makes them visible. Every other story (default inertness,
races, vocabulary) protects or extends this one observable.

**Independent Test**: Can be fully tested by driving the hazard net
through the timing-mode evaluation and asserting the pulse's width
and placement against a hand-derived path-delay difference (never the
implementation's own readback), plus asserting the settled values
equal the zero-delay evaluation's — delivering glitch observation
with no sequential, truth, or persistence story involved.

**Acceptance Scenarios**:

1. **Given** a hazard net (input → direct path + N-inverter chain
   → one gate), **When** the timing-mode evaluation applies the
   flipping assignment, **Then** the trace shows the transient pulse
   with its hand-derived width (N x unit) and placement, and the
   settled values equal the zero-delay evaluation's.
2. **Given** the same net evaluated without the mode, **When** the
   invocation runs after this feature, **Then** the output is
   byte-identical to before the feature — settled values only, no
   transient, no new text.
3. **Given** gates carrying annotations, **When** the timing-mode
   evaluation runs, **Then** every transition lands exactly its stored
   whole-picosecond annotation after its triggering input transition
   — no rounding, no clamping, no reinterpretation.
4. **Given** un-annotated gates, **When** the timing-mode evaluation
   runs, **Then** every one takes the mode's unit delay — one
   picosecond, the field's smallest storable nonzero value — and the
   mode's output names the unit so the default is never a mystery.
5. **Given** a new input event arriving while an output event is
   still pending, **When** both propagate, **Then** each fires at its
   own offset (transport semantics): no event cancellation, no
   inertial filtering — a pulse the physics would swallow still
   shows, because the mode observes and never filters.
6. **Given** the same timing-mode invocation repeated on the same
   workspace, **Then** the trace is byte-identical — events ordered
   by time first, then the module's standing slot order, never by
   arrival or evaluation whim.

---

### User Story 2 - The default never moves (Priority: P1)

Every flow that does not explicitly select the timing mode is
byte-identical to before the feature: evaluations, stepping, truth
streaming, listings, saves, refusal messages, every golden and every
example replay. The zero-delay fast path takes no timing input: it
reads no delay field, branches on no mode flag, and measures
unchanged on the module's existing benchmarks. This is where 013's
inertness pin earns its keep — the annotation surface shipped with
the promise that stored values never silently change evaluation, and
this feature is the first code that reads the field; that read
happens only inside the explicitly selected discipline, so the
promise keeps its shape: annotations influence evaluation in the
timing mode and nowhere else.

**Why this priority**: The module's standing contract is that the
default is deterministic, fast, and untouched. An evaluator that
perturbed the zero-delay path — in output or in measured cost —
would fork the value system it exists to inspect. Pinning
zero-delta is what makes the opt-in honest.

**Independent Test**: Can be fully tested by running every
pre-feature golden, example replay, refusal class, and the bench
suite, and asserting byte-identical outputs and unchanged
measurements — plus evaluating the same netlist with and without
annotations in default mode and asserting identical results,
proving the default path is annotation-blind.

**Acceptance Scenarios**:

1. **Given** any pre-feature flow (no mode selected), **When** it
   runs after this feature, **Then** its output is byte-identical to
   before the feature.
2. **Given** the same netlist with and without annotations, **When**
   evaluated in default mode, **Then** the outputs are identical —
   the default evaluation path is provably annotation-blind.
3. **Given** the module's benchmark suite, **When** re-run after the
   feature, **Then** measurements are unchanged — the fast path paid
   nothing for the new discipline.
4. **Given** the annotation surface (set, clear, list readback),
   **When** used after this feature, **Then** its behavior is
   byte-identical to 013 — no verb gained timing awareness.

---

### User Story 3 - Races at the sequential boundary (Priority: P2)

A user steps a clocked netlist in timing mode. The step's window
anchors the clock edge at offset zero and reports timestamped events:
each edge flop's Q (and NQ, at the same instant) transitions exactly
its annotation — the clock-to-Q delay — later, and the combinational
downstream ripples on top; a level latch's response lands at its
annotation the same way. A D change arriving close to the edge is
visible at its picosecond offset — the race the zero-delay stepper
cannot show, because there every arrival is simultaneous and
invisible. The event wheel always drains before the edge, and the
edge samples the drained pre-edge state, so the state trajectory
matches zero-delay stepping step-for-step: the mode exposes ordering
without altering outcomes. Whether an arrival violates a flop's
setup budget stays the user's judgment — the mode reports, it does
not verdict.

**Why this priority**: Races are the sequential half of the Why, and
013 explicitly deferred "what a delay means for a clocked element"
to this evaluator. But they ride on story 1's discipline; a user
whose nets are purely combinational gets the feature's full value
from story 1 alone.

**Independent Test**: Can be fully tested by stepping a clocked net
with an annotated clock-to-Q in both modes and asserting identical
state trajectories (readback identical step-for-step) plus trace
arrival offsets hand-derived from clock-to-Q plus downstream path
delays — delivering race observation with no truth or vocabulary
story involved.

**Acceptance Scenarios**:

1. **Given** an edge flop annotated with a delay, **When** stepped
   in timing mode, **Then** Q transitions exactly that many
   picoseconds after the edge, NQ transitions at the same instant
   (never lagging Q), and the trace shows both.
2. **Given** a D path fed by that flop's Q, **When** stepped, **Then**
   the D arrival at the next flop is stamped at its hand-derived
   offset (clock-to-Q + downstream path delays), and the sampled
   state equals zero-delay stepping's for the same step sequence.
3. **Given** level latches in the net, **When** their driving nets
   change mid-window, **Then** their responses land at their
   annotations (or the unit) in the trace, and the standing level
   rules (transparency, hold, re-settle before edging) are
   unchanged.
4. **Given** a transient window where S and R are momentarily both
   high through differential path delays but settle legal, **When**
   the window is evaluated, **Then** those instants render X in the
   trace — visible, never silently coerced — while a settled S=R=1
   net refuses exactly as today.
5. **Given** the same stepping sequence in timing mode repeated,
   **Then** trajectories and traces are byte-identical — parity with
   the zero-delay trajectory and full determinism.

---

### User Story 4 - One vocabulary, deterministic by construction (Priority: P2)

The timing mode is a per-invocation selection on the module's
existing evaluation verbs inside the logic command domain (no new
top-level command), anchored on the already-tasked `--with-delays`
spelling. Completion covers the flag; the interactive terminal
surfaces compose the same invocations with byte-identical output;
the selection never sticks — it is not session state, not serialized,
not part of any workspace — so every invocation starts from the
default and repeated runs are independent. Every registry-size
check, run example, module-table entry, and test-list line updates
in the same change, and every refusal (misspelled flag, malformed
arguments) follows the module's usage/runtime exit conventions with
the workspace untouched.

**Why this priority**: The module's standing contracts (one command
domain, interactive/headless byte parity, registry checks) keep the
mode from forking behavior per surface. They are required for
coherence but add no new observable, hence P2.

**Independent Test**: Can be fully tested by driving the same
timing invocations through the session and one-shot paths and
asserting byte parity, asserting completion offers the flag, and
asserting save/load round-trips and the workspace schema are
untouched (no new persisted state, no version bump).

**Acceptance Scenarios**:

1. **Given** the flag on the evaluation verbs, **When** the same
   invocations run through the session and one-shot paths, **Then**
   outputs are byte-identical, pinned by the standing parity tests.
2. **Given** completion over the evaluation verbs, **When** the flag
   is requested, **Then** it completes with its value forms.
3. **Given** a timing-mode invocation, **When** it ends, **Then**
   nothing persists — the next invocation starts in default mode,
   and save/load schema and serialization versions are untouched.
4. **Given** a misspelled flag or malformed argument, **When** the
   invocation runs, **Then** it fails loudly per the module's
   usage/runtime exit conventions with the workspace untouched.
5. **Given** the registry and documentation duties, **When** the
   feature lands, **Then** every registry-size check, run example,
   module-table entry, and test-list line updates in the same
   change.

---

### User Story 5 - Critical-path visibility on the truth surface (Priority: P3)

A user streams the truth table in timing mode. Every row's values
are identical to the zero-delay table; each row additionally reports
its drain time — the assignment's critical path in whole
picoseconds, hand-derivable as the longest annotated path the
assignment activates. This gives at-a-glance worst-path visibility
across the whole input space without changing a single value, and it
inherits the surface's standing refusals (shared attachments, live
buffers) identically.

**Why this priority**: The task pins the truth surface as part of the
mode's surface set, but its value is analysis convenience atop
phenomena story 1 already proves — the trace demonstrates each path;
the table merely surveys them all at once.

**Independent Test**: Can be fully tested by streaming a small net
with a known maximum path in timing mode and asserting each row's
drain time matches its hand-derived critical path while the row
values match the zero-delay table.

**Acceptance Scenarios**:

1. **Given** timing-mode truth streaming, **When** the table is
   produced, **Then** every row's values are identical to the
   zero-delay table and each row carries its hand-derivable drain
   time in whole picoseconds.
2. **Given** the truth surface's standing refusals (shared
   attachments, live buffers), **When** timing-mode truth is
   requested on such a netlist, **Then** the refusals apply
   identically — timing mode inherits domain constraints, never
   relaxes them.

---

### Edge Cases

- What happens when **a gate carries no annotation in timing mode**?
  → it takes the mode's unit delay: one picosecond, the field's
  smallest storable nonzero value. The stored field stays zero —
  the unit is a mode-level default, never written back.
- What happens when **a gate is annotated with exactly zero**? →
  zero is un-annotated by the module's pinned 013 convention, so
  the gate takes the unit in this mode; no gate can be faster than
  unit, and the mode's usage/output names this so the convention
  never surprises.
- What happens when **two events land on the same picosecond**? →
  deterministic total order: time first, then the module's standing
  slot order. Never arrival order, never evaluation whim.
- What happens when **a new input event arrives while an output
  event is pending**? → transport semantics: the gate recomputes at
  each arrival's own offset — no cancellation, no inertial
  filtering; the mode exists to show the narrow pulses physics
  would swallow.
- What happens on **a shared (multi-driver) net whose drivers
  arrive at different picoseconds**? → transient contention windows
  open; the shipped funnel applies at every instant, transient X/Z
  values appear in the trace and close by drain, and contention
  counting stays live exactly as today.
- What happens when **a transient X or Z sits at a gate input
  mid-window (e.g. a released buffer)**? → the transient propagates
  as X through the window — visible and honest; a settled X/Z at a
  level consumer refuses exactly as today. The settled contract is
  never weakened; the transient is never silently coerced.
- What happens when **S and R are transiently both high through
  differential delays but settle legal**? → those instants render X
  in the trace; only the settled illegal net refuses as today.
- What happens with **huge but legal annotations (hours-scale whole
  picoseconds)**? → timestamps grow accordingly, in whole
  picoseconds; arithmetic overflow of the time axis refuses loudly
  — never wraps, never clamps.
- What happens if **the event wheel could fail to drain**? →
  impossible by construction: combinational cycles are refused at
  wire time, so every window drains within a per-stimulus event
  budget bounded by the netlist; a budget breach is a loud runtime
  failure, never a hang.
- What happens when **annotations change between invocations**? →
  the next timing invocation honors the new values (recompiled like
  the standing topology dirty flag); zero-delay invocations remain
  identical regardless.
- What happens when **the reshaping optimizer level runs first**? →
  it resets annotations exactly per its standing contract, a timing
  run over the optimized net therefore runs at unit delays, and the
  reset note already names it — no new optimizer behavior lands.
- What happens when **a workspace with annotations is saved,
  reloaded, and replayed in timing mode**? → identical replay: the
  annotations round-trip per 013, and the mode itself is never
  serialized — a run-time selection only, so no schema or version
  bump anywhere.
- What happens on **the engine run path (`sim run --solver logic`)**?
  → it stays zero-delay in this feature; timing integration there is
  the named follow-on, and the engine's run configuration, snapshot,
  and checkpoints gain nothing here.
- What happens to **trace size on large nets**? → bounded by
  construction (per-stimulus events are bounded by the netlist
  size); the terminal never receives unbounded output, and a loud
  cap guards pathological cases.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The module MUST provide an opt-in timing evaluation
  mode on its evaluation surfaces (single-assignment solve, clock
  step, truth stream), selected per invocation inside the existing
  logic command domain (no new top-level command); every default
  invocation MUST remain the zero-delay fast path.
- **FR-002**: In timing mode, every gate output transition MUST
  land exactly its stored whole-picosecond annotation after its
  triggering input transition; un-annotated gates MUST take the
  mode's unit delay (one picosecond, the field's smallest storable
  nonzero value); annotations MUST be read as stored — no
  clamping, rounding, or rewriting — and the stored field MUST
  remain untouched (the unit is a mode-level default, never
  written back).
- **FR-003**: Timing evaluation MUST follow transport semantics:
  every input-net transition schedules its own output recomputation
  at the gate's delay; no event cancellation or inertial filtering
  may suppress a transient.
- **FR-004**: Timing-mode reports MUST be ordered event traces:
  every observed net transition with its whole-picosecond offset
  from the window anchor (the applied assignment for solve, the
  clock edge for step), in a pinned total order (time first, then
  the module's standing order), through transient values, ending at
  the drained settled state.
- **FR-005**: Combinational parity: for purely combinational
  netlists, timing-mode settled values MUST equal zero-delay values
  for the same assignment — the trace adds transients, never
  different endpoints.
- **FR-006**: Sequential timing: annotations on edge-triggered
  elements MUST mean clock-to-Q delay (NQ transitions MUST share
  Q's instant, never lag it); on level latches they MUST mean the
  output's response delay; the event wheel MUST fully drain before
  every clock edge, and edges MUST sample the drained pre-edge
  state — so timing-mode stepping trajectories MUST match
  zero-delay stepping step-for-step while the trace exposes
  arrival ordering against the edge. Setup/hold verdicts MUST stay
  the user's judgment; violation checking is explicitly out of
  scope.
- **FR-007**: The zero-delay fast path MUST remain untouched:
  default outputs MUST be byte-identical to pre-feature on every
  surface; the default evaluation path MUST read no delay field
  and branch on no mode; existing benchmarks MUST measure
  unchanged.
- **FR-008**: Determinism: identical timing-mode invocations MUST
  produce byte-identical traces; same-instant events MUST resolve
  in the pinned total order; time arithmetic MUST be whole
  picosecond integers with loud overflow refusal.
- **FR-009**: Settled-state contracts MUST be preserved exactly:
  settled X/Z at level consumers and settled S=R=1 refuse as
  today; transient invalid windows (contention, released buffers,
  momentary S=R=1) MUST propagate X visibly in the trace, never
  silently coerced.
- **FR-010**: Every surface's standing domain constraints MUST
  apply unchanged in timing mode: truth's shared/buffer refusals,
  solve's loud default-echo, and the four-value funnel at every
  instant on shared nets.
- **FR-011**: Truth in timing mode MUST stream rows identical in
  values to zero-delay truth plus a per-row drain time (the
  assignment's critical path in whole picoseconds, independently
  derivable); no other truth output may change.
- **FR-012**: The mode selection MUST be per-invocation: never
  sticky session state, never serialized; no schema or
  serialization-version change anywhere; repeated invocations MUST
  be independent.
- **FR-013**: One vocabulary: the mode's flag MUST anchor on the
  reserved `--with-delays` spelling; completion MUST cover it;
  interactive terminal surfaces MUST compose the same invocations
  with byte-identical output; every registry-size check, run
  example, module-table entry, and test-list line MUST update in
  the same change; every refusal MUST follow the module's
  usage/runtime exit conventions.
- **FR-014**: The feature MUST be verified at the module's testing
  boundaries: independently derived trace goldens (pulse widths,
  drain times, arrival offsets — never the implementation's own
  output), CLI representation, byte-identity of every pre-feature
  flow, and the unchanged bench suite; shipped example workspaces
  MUST pin the inverter-chain glitch demo and a sequential race
  demo with their expected traces.

### Key Entities *(include if feature involves data)*

- **Timing evaluation mode**: the opt-in, per-invocation evaluation
  discipline that reads delay annotations and reports event traces;
  a second evaluator beside the zero-delay fast path, never a
  conditional inside it.
- **Event trace**: the mode's observable — ordered net transitions
  with whole-picosecond offsets from the window anchor, through
  transient values, to the drained settled state.
- **Per-gate delay annotation**: 013's stored whole-picosecond
  field — inert in every default flow, honored here as the gate's
  transport delay; zero means un-annotated (unit in this mode).
- **Unit delay**: the mode's floor for un-annotated gates — one
  picosecond, the field's smallest storable nonzero value; a
  mode-level default, never written back to storage.
- **Window anchor**: the trace's offset-zero — the assignment
  landing (solve) or the clock edge (step); each invocation
  reports its own window, and no cross-invocation absolute
  picosecond clock accumulates.
- **Clock edge boundary**: the step's sampling instant; the wheel
  drains before it and the edge samples the drained pre-edge
  state — the root of trajectory parity.
- **Transport delay**: per-gate input-to-output latency applied
  uniformly to all of the gate's input paths; per-pin and
  rise/fall splits do not exist in storage and are out of scope.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Glitch demonstration in one session — the hazard net
  (direct vs. N-inverter path into one gate) shows its transient
  pulse with the hand-derived width (N x unit) and placement in
  timing mode, the zero-delay evaluation of the same net shows
  only the settled value, and the settled values agree between the
  modes.
- **SC-002**: Zero behavioral delta — every pre-feature golden,
  example replay, refusal message, saved-workspace behavior, and
  bench measurement passes unchanged; the same netlist with and
  without annotations evaluates identically in default mode.
- **SC-003**: Race visibility with pinned trajectories — a clocked
  net with annotated clock-to-Q shows the D-arrival offset relative
  to the edge at its hand-derived picosecond offset; state
  trajectories are identical across modes for the same step
  sequence; transient invalid windows render X while settled
  invalidity refuses as before.
- **SC-004**: One vocabulary, deterministic traces — the flag
  composes identically across the session, one-shot, and
  interactive terminal surfaces (byte parity), completion offers
  it, repeated invocations are byte-identical, nothing persists
  (no schema bump), and every registry-size check, run example,
  module-table entry, and test-list line updates in the same
  change.

## Assumptions

- 013 shipped the storage this feature reads: per-gate annotations
  exist (whole picoseconds, zero = un-annotated), persist in every
  save format, list with readback, and follow documented optimizer
  rules. This feature adds the honoring evaluator only — no struct,
  schema, or serialization-version change (docs/logic.md §12 pins
  exactly this slot).
- Un-annotated gates take the mode's unit (one picosecond),
  grounded in the task's unit-delay semantics and the field's
  whole-picosecond granularity. No gate can be faster than unit in
  this mode; zero-annotation stays "un-annotated" per the pinned
  013 convention.
- Transport semantics (no inertial cancellation) are deliberate:
  the mode's job is observation, and realism filters would hide
  exactly the hazards it exists to expose.
- The per-gate delay applies uniformly to all of the gate's input
  paths; per-pin and rise/fall delays do not exist in storage and
  are out of scope (named future if ever needed).
- Sequential annotations mean output delay: clock-to-Q for edge
  flops, response delay for level latches. Edges sample the drained
  pre-edge state, so trajectories stay identical to zero-delay —
  the feature observes ordering, never alters outcomes.
  Setup/hold checking is explicitly future scope.
- Each timing invocation reports its own relative window (anchor at
  offset zero); no cross-invocation absolute picosecond clock
  accumulates — the step counter remains the module's epoch, and
  picosecond offsets are per-window.
- The flag anchors on the already-tasked `--with-delays` spelling;
  exact argument shapes are planning decisions, required only to be
  explicit, loud, and completion-covered.
- The engine run path (`sim run --solver logic`) stays zero-delay;
  its timing integration is the named follow-on. The engine's run
  configuration, snapshot rows, and checkpoints gain nothing in
  this feature.
- Machine-readable trace export (CSV) is out of scope; the
  terminal trace is the observation surface. A named follow-on
  exists if a tooling consumer appears.
- Shipped example workspaces pin the demos (inverter-chain glitch
  + sequential race) with expected traces in their headers, per
  the module's example convention.
- GUI surfaces are out of scope (separate epic; the standing parity
  rule covers any interactive form that appears).
- A dedicated timing-path benchmark is optional planning scope:
  the feature's measured duty here is the unchanged existing bench
  suite — no regression on the fast path.
