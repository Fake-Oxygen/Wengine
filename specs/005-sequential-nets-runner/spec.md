# Feature Specification: Sequential Nets Runner

**Feature Branch**: `005-sequential-nets-runner`

**Created**: 2026-11-04

**Status**: Draft

**Input**: User description: "Run sequential nets through core::Runner with a nested RunConfig section and multi-clock domain field. Moves clocks from CLI-only stepping into the deterministic fixed-step engine + snapshots path."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Run a sequential net for N fixed steps and replay it identically (Priority: P1)

A hardware-logic user defines a sequential net (combinational gates plus
edge-triggered flops and level-sensitive latches, with driven inputs) and
runs it for an exact number of fixed steps through the shared deterministic
engine, then reads back flop/latch states and settled outputs. Running the
identical net with the identical configuration a second time produces
byte-identical observable results.

**Why this priority**: This is the core promise of the feature — clocks
move from the interactive-only stepper into the reproducible fixed-step
engine path shared by every other simulation domain. Without it nothing
else (domains, snapshots, sinks) has a foundation.

**Independent Test**: Can be fully tested by building a small sequential
net (e.g. a DFF counter or T divider), running it for N steps headlessly,
recording the state readback, then re-running the identical configuration
and confirming identical readback — delivering deterministic batch stepping
with no domain selection or snapshot plumbing involved.

**Acceptance Scenarios**:

1. **Given** a sequential net with at least one edge-triggered flop and a
   valid run configuration, **When** the user runs it for N fixed steps,
   **Then** each flop's stored value reflects exactly N edge resolutions
   per its excitation table and the reported step count equals N.
2. **Given** the identical net and configuration run twice from the same
   starting state, **When** both runs complete, **Then** both report
   identical flop/latch states and identical settled outputs on every
   repetition.
3. **Given** a net containing only level-sensitive latches (no edge flops),
   **When** the user runs it for N fixed steps, **Then** the run succeeds
   and latch values follow the transparent/frozen rule across all steps
   (no clock edge required, no spurious cycle failure).

---

### User Story 2 - Select which clock(s) advance on a multi-clock net (Priority: P1)

A user whose net has more than one clock driver selects which clock domain
advances: one named domain, all domains, or a stated subset. Only the flops
driven by the selected clock(s) capture new values on a step; flops in
unselected domains hold their stored values while combinational outputs
still settle around them.

**Why this priority**: Multi-clock nets are already constructible (each
flop belongs to exactly one derived clock domain); without a selection
field the engine path could only step everything at once and would mis-step
real multi-clock designs. Selective stepping is what makes the engine path
a faithful move of the current clock semantics rather than a regression.

**Independent Test**: Can be fully tested by building a net with two
independent clocks, running N steps with only the first domain selected,
and confirming the first domain's flops advanced N times while the second
domain's flops held — delivering per-domain selectivity with no snapshot
or sink involvement.

**Acceptance Scenarios**:

1. **Given** a net with two independent clock domains, **When** the user
   runs N steps selecting only the first domain, **Then** flops in the
   first domain reflect N edge resolutions and flops in the second domain
   hold their starting values.
2. **Given** the same two-domain net, **When** the user runs N steps
   selecting all domains, **Then** both domains' flops reflect N edge
   resolutions each.
3. **Given** a selection naming a clock domain that does not exist in the
   net, **When** the user attempts the run, **Then** the run is refused
   loudly as a usage error naming the unknown domain and no simulation
   state changes.

---

### User Story 3 - Observe sequential state through snapshots and sinks (Priority: P2)

A user records or streams a sequential run through the standard
observation path (state readback, saved snapshots, file/CSV-style sinks)
and sees flop/latch stored values plus settled outputs at each recorded
step, with the same values in the live readback and in the recorded
artifact.

**Why this priority**: The stated purpose is joining the "snapshots path":
engine runs must be observable through the same recording surfaces as
other domains. Without snapshot/sink parity the engine run is a black box.

**Independent Test**: Can be fully tested by running a short sequential
run with recording enabled, then comparing the recorded per-step stored
values against the live readback sequence — delivering observable engine
runs with no new recording format.

**Acceptance Scenarios**:

1. **Given** a sequential run of N steps with per-step recording enabled,
   **When** the run completes, **Then** the recording contains N ordered
   entries whose stored values match the live readback at each step.
2. **Given** the same recorded run replayed from its starting state,
   **When** the replay completes, **Then** the second recording is
   identical entry-for-entry to the first.

---

### User Story 4 - Carry stimulus, controls, and legality rules into the engine path (Priority: P2)

A user drives inputs (including asynchronous controls and latch enables)
for an engine run and observes the same documented resolution rules as in
interactive stepping: async forces apply with their documented priority,
transparent latches follow immediately, frozen latches hold, and the
illegal combination is refused loudly with the step rejected and state
unchanged.

**Why this priority**: Moving clocks into the engine must not silently
change stimulus semantics. Controls and level behavior that work
interactively must behave identically in batch, including loud failure on
illegal input, or existing designs will misbehave when moved to the engine.

**Independent Test**: Can be fully tested by running a net with an async
force asserted and a net attempting the illegal combination, confirming
the force takes effect per the priority rule in the first case and the
second run fails loudly with state unchanged — delivering semantic parity
with no domain-selection involvement.

**Acceptance Scenarios**:

1. **Given** a flop with its async reset driven active, **When** the user
   runs one fixed step, **Then** the flop reads the reset value regardless
   of clock selection or data input, matching the documented priority.
2. **Given** a net whose driven or wired inputs resolve to the documented
   illegal combination, **When** the user attempts a step, **Then** the
   step fails loudly naming the illegal combination, advances neither time
   nor step count, and leaves stored state unchanged.
3. **Given** a net driven with a non-binary stimulus value, **When** the
   user attempts a step, **Then** the step fails loudly as a usage error
   with state unchanged, consistent with interactive drive validation.

---

### Edge Cases

- What happens when the selected domain list is empty? It is rejected as
  a usage error before any step; an explicitly empty selection never means
  "step nothing silently".
- What happens when a flop's clock pin dangles (no driver)? It belongs to
  the implicit default domain exactly as in interactive stepping, and
  selecting that default domain steps it; a selection naming only other
  domains leaves it held.
- What happens when a latch input is unconnected (dangling)? It reads the
  inactive value (the hold row), so undriven latches hold state — the same
  rule as interactive stepping.
- What happens when `dt` is zero, negative, non-finite, or below clock
  resolution? The run is rejected before any solver mutation with state
  bit-identical, consistent with the shared fixed-step validation.
- What happens when the step count is zero? The run succeeds as a no-op
  with no state change and no recorded entries, consistent with other
  engine runs.
- What happens when the sequential section is populated but a different
  solver is selected (or vice versa)? The configuration is rejected loudly
  before running, matching the one-section-per-solver rule.
- What happens when a mid-run step fails (illegal combination arising from
  wired logic at step k)? The run aborts at step k with a loud error
  naming the cause; completed steps keep their recorded entries and the
  failure is reported with the step index frozen (no silent continuation).
- What happens to existing files and interactive sessions? Legacy saved
  workspaces load unchanged; interactive stepping behavior is unchanged —
  the engine path is additive.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The system MUST run a sequential net through the shared
  deterministic fixed-step engine for an exact configured number of steps,
  resolving edge-triggered flops per their documented excitation tables
  and level-sensitive latches per the transparent/frozen rule on every
  step.
- **FR-002**: The system MUST provide one nested sequential configuration
  section (owned beside the sequential engine code, aggregated into the
  shared run configuration) carrying the sequential run inputs; the driver
  MUST honor that section only when the sequential solver is selected and
  MUST reject it loudly when set with any other solver.
- **FR-003**: The nested section MUST carry a multi-clock domain selection
  field expressing: one named domain, all domains, or an explicit subset;
  the engine MUST step exactly the selected domains' flops per step while
  unselected flops hold.
- **FR-004**: Clock-domain membership MUST match interactive stepping
  exactly: one domain per clock-driver net over edge-triggered flops only;
  dangling clock pins form the implicit default domain; level-sensitive
  latches belong to no domain and are never gated by the selection.
- **FR-005**: Repeated identical runs (same net, same stimulus, same
  configuration) MUST produce byte-identical observable results across
  all surfaces (headless run, scripted run, replayed run).
- **FR-006**: Sequential engine runs MUST be observable through the
  standard snapshot path: per-step stored values and settled outputs
  visible in readback and in recorded artifacts with identical values in
  both.
- **FR-007**: Asynchronous controls MUST resolve in the engine path with
  the documented total priority before edging, and level latches MUST
  settle before edging on every step, identical to interactive stepping.
- **FR-008**: The documented illegal combination MUST fail the step loudly
  (naming the combination), advancing neither time nor step count and
  committing no state change; non-binary stimulus MUST fail the same way
  as a usage error.
- **FR-009**: Invalid configurations MUST be rejected before any step with
  state unchanged: unknown domain names, empty domain selection,
  non-positive/non-finite/step-resolution `dt`, zero-or-invalid step
  counts, and section/solver mismatches.
- **FR-010**: A mid-run failure MUST abort the run loudly at the failing
  step with the step index reported; already-recorded entries MUST remain
  intact and no further steps may execute silently.
- **FR-011**: Existing workspaces, file formats, and interactive stepping
  behavior MUST be unchanged (legacy files load exactly as before; the
  engine path is purely additive).

### Key Entities *(include if feature involves data)*

- **Sequential Net**: A gate netlist containing combinational gates plus
  edge-triggered flops and/or level-sensitive latches with wired and
  driven inputs; the unit stepped by the engine.
- **Clock Domain**: The set of edge-triggered flops sharing one clock
  driver net (dangling clocks form the implicit default domain); the unit
  of selective stepping. Level latches belong to no domain.
- **Domain Selection**: The multi-clock field of the nested section —
  one named domain, all domains, or an explicit subset — deciding which
  flops capture on each fixed step.
- **Nested Section**: The sequential solver's configuration block inside
  the shared run configuration (stimulus + domain selection + run inputs),
  honored only when the sequential solver is selected.
- **Engine Snapshot**: The per-step recorded entry carrying stored flop/
  latch values and settled outputs; the observation surface shared with
  other simulation domains.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Users can run a 2-bit counter net for 4 fixed steps through
  the engine and observe the count sequence 00 → 01 → 10 → 11 → 00 in the
  readback on the first attempt in at least 9 of 10 trials.
- **SC-002**: Users can run a two-clock net for 3 steps selecting one
  domain and observe the selected domain advanced 3 times while the
  unselected domain held, on the first attempt.
- **SC-003**: Two identical 10-step sequential engine runs from the same
  starting net produce identical recorded outputs on every repetition.
- **SC-004**: Existing sequential workspaces run interactively produce
  step-for-step identical outputs before and after the feature (no
  migration, no behavior change on the interactive path).
- **SC-005**: An attempted run with an unknown domain name, and an
  attempted step resolving to the illegal combination, are each refused
  loudly with state unchanged, on the first attempt.

## Assumptions

- The fixed-step engine owns time (`dt`, step index); clock domains select
  *which flops capture* on a step and never alter the step cadence or
  create per-domain clocks.
- The excitation tables, control priority, transparent/frozen rule, and
  illegal-combination refusal are unchanged from the documented
  interactive semantics; this feature moves them onto the engine path
  without redefining them.
- Stimulus for batch runs is carried in the nested section as parsed
  values (no free-text parsing inside the engine), following the
  one-section-per-solver convention.
- Recording uses the existing snapshot/sink surfaces; no new file format
  or transport is introduced.
- Scope is the engine + configuration + observation wiring: interactive
  verbs keep their spelling and behavior, and no graphical display is
  introduced.
