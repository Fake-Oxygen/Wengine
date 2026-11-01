# Feature Specification: DFF Control Set

**Feature Branch**: `002-dff-control-set`

**Created**: 2026-10-31

**Status**: Draft

**Input**: User description: "Full DFF control set: async SET/RST and clock enable with documented priority. Bare D-only flops cannot reset or stall real pipelines; this is the minimum viable flop for counters and FSMs."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Reset a sequential circuit to a known state (Priority: P1)

A hardware-logic user building a counter or finite-state machine drives a
reset control on one or more DFFs so every flop returns to a known startup
value immediately, without waiting for a clock edge or sequencing D inputs
through several cycles.

**Why this priority**: Without reset, counters and FSMs power on in an
arbitrary state and cannot be brought to a defined idle/start state; every
other control is secondary to a trustworthy reset.

**Independent Test**: Can be fully tested by building a small sequential
circuit, advancing clocks so Q values become non-zero, asserting the reset
control, and reading back Q — delivering a working reset-to-known-state
loop with no other new control.

**Acceptance Scenarios**:

1. **Given** a workspace with a DFF holding Q=1, **When** the user asserts
   the async reset control, **Then** Q becomes 0 immediately (before the
   next clock advance) and stays 0 while reset is held regardless of D or
   clock activity.
2. **Given** a multi-flop counter in an arbitrary state, **When** the user
   asserts reset on all flops and then releases it, **Then** every flop
   reads Q=0 and the next clock advance counts from zero.
3. **Given** identical reset → release → advance sequences run twice from
   the same starting workspace, **When** both runs complete, **Then** both
   report identical flop states (deterministic replay).

---

### User Story 2 - Preset a flop to 1 without clocking (Priority: P2)

A user drives a set/preset control on a DFF so the flop jumps to Q=1
immediately (for example to load a counter seed or force an FSM into a
specific state), without waiting for a clock edge.

**Why this priority**: Preset is the symmetric counterpart of reset and is
needed for seedable counters and one-hot FSM entry; it is P2 only because
reset alone already unblocks the bring-up-to-known-state flow.

**Independent Test**: Can be fully tested by asserting the set control on
an idle flop and reading back Q=1 with no clock advance — delivering
async preset independent of reset and clocking.

**Acceptance Scenarios**:

1. **Given** a DFF holding Q=0 with D=0, **When** the user asserts the set
   control, **Then** Q becomes 1 immediately and stays 1 while set is held
   regardless of D or clock activity.
2. **Given** a flop with set asserted then released, **When** the user
   advances one clock with D=0, **Then** Q follows D (returns to 0),
   proving set does not latch after release.

---

### User Story 3 - Stall a pipeline with clock enable (Priority: P2)

A user deasserts a clock-enable control on selected DFFs so those flops
hold their Q across clock advances while the rest of the circuit keeps
stepping, enabling pipeline stalls and per-domain holds.

**Why this priority**: Real pipelines must freeze individual stages on
backpressure; without enable, every advance steps every flop in the domain
and stalls are inexpressible.

**Independent Test**: Can be fully tested by disabling enable on one flop,
advancing the clock with a toggling D, and confirming Q is unchanged while
an enabled sibling flop follows D — delivering selective hold with no
reset/set involvement.

**Acceptance Scenarios**:

1. **Given** a DFF with enable deasserted and D toggling, **When** the user
   advances the clock by 3 cycles, **Then** Q is unchanged across all 3
   advances.
2. **Given** two flops in the same clock domain where one has enable
   asserted and the other deasserted, **When** the user advances one cycle,
   **Then** the enabled flop captures D and the disabled flop holds Q.

---

### User Story 4 - Rely on one documented priority rule (Priority: P3)

A user reads a single short priority statement (which control wins when
several are active at once) and predicts the flop output for every control
combination without trial and error, in interactive sessions, scripts, and
saved files alike.

**Why this priority**: Priority disputes (reset vs. set, async vs. enable)
are the classic source of counter/FSM bring-up bugs; a documented,
observable rule turns the control set from four pins into one predictable
primitive.

**Independent Test**: Can be fully tested by asserting conflicting controls
simultaneously (reset + set, controls + disabled enable) and reading back
the single documented winner — delivering the rule itself as observable
behavior.

**Acceptance Scenarios**:

1. **Given** a DFF with both set and reset asserted, **When** the user
   observes Q (with no clock advance), **Then** Q equals the documented
   simultaneous-assertion winner and the documentation names that winner.
2. **Given** a DFF with async reset asserted and enable deasserted,
   **When** the user advances the clock, **Then** Q follows the async
   control (async controls override enable), as the documentation states.

---

### Edge Cases

- What happens when set and reset are asserted at the same time? The
  documented winner applies (reset wins → Q=0); the outcome is
  deterministic and identical across REPL, script, and replay.
- What happens when async controls are asserted during a clock advance?
  Async controls take effect immediately at assertion time (not at the
  edge) and dominate the edge capture; the advance itself changes nothing
  while an async control is held.
- What happens when enable is deasserted at the same time D changes?
  Q holds its pre-edge value; D changes are ignored for that edge and
  every subsequent edge until enable is reasserted.
- How does the system handle an unconnected (dangling) control pin on a
  5-input flop? It behaves as the inactive value (no reset, no set,
  enable holding the flop enabled: RST/SET read 0, EN reads 1),
  consistent with FR-008; like any other dangling pin it appears in the
  block interface until explicitly wired or ignored.
- How does the system handle invalid drive values on control pins? Only
  clean logic values are accepted; unknown/floating drives are rejected as
  usage errors that change no state.
- How do NQ, peek, and saved files reflect the new controls? NQ always
  reads NOT Q including immediately after async assertion; peek shows Q/NQ
  unchanged in form; saved files round-trip the new wires and driven
  values.
- What happens on power-on before any control is asserted? Flops power on
  at Q=0 exactly as today; the new controls add no change to the
  power-on state.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: DFF blocks MUST expose async reset (`RST`) and async set
  (`SET`) controls plus a clock-enable (`EN`) control in addition to the
  existing `D` / `CLK` / `Q` / `NQ` pins.
- **FR-002**: Asserting async reset MUST force Q to 0 immediately (with no
  clock advance required) and MUST hold Q at 0 for as long as reset is
  asserted, regardless of `D`, `CLK`, `EN`, or `SET` activity.
- **FR-003**: Asserting async set MUST force Q to 1 immediately (with no
  clock advance required) and MUST hold Q at 1 for as long as set is
  asserted, regardless of `D`, `CLK`, or `EN` activity (subject only to the
  reset-wins rule in FR-005).
- **FR-004**: Deasserting clock enable MUST cause the flop to hold its
  current Q across clock advances (D is ignored for those edges);
  asserting enable MUST restore normal edge capture of D.
- **FR-005**: The system MUST implement and document one total priority
  order: async reset > async set > clock-enable hold > D capture. When
  reset and set are simultaneously asserted, Q MUST be 0 (reset wins).
  Async controls MUST override enable; enable MUST override D.
- **FR-006**: The priority rule MUST be user-visible in help/session
  documentation and MUST be observable: every conflicting-control
  combination MUST produce the documented winner deterministically across
  REPL sessions, scripts, pipes, and replayed files.
- **FR-007**: All new control pins MUST work through the existing session
  verbs with the same conventions as `D`/`CLK`: wireable between blocks,
  drivable/releasable via poke, listed via named pins, readable via peek
  (Q/NQ reflect the controls), and persisted through save/load files with
  round-trip fidelity.
- **FR-008**: Unconnected control pins MUST default to inactive (no reset,
  no set, enable asserted) so existing D-only netlists behave exactly as
  before with zero migration.
- **FR-009**: Repeated identical assert → advance → observe sequences MUST
  produce byte-identical observable results (deterministic replay,
  including async assertion timing relative to advances).
- **FR-010**: Invalid control stimulus (unknown/floating values on control
  pins, unknown pin names) MUST be rejected as usage errors that change no
  simulation state, consistent with existing poke/wire error behavior.
- **FR-011**: Session help and command completion MUST cover the new pins,
  their value forms, and the priority statement, matching existing session
  conventions.

### Key Entities *(include if feature involves data)*

- **DFF Control Pins**: The new per-flop inputs — async reset (`RST`),
  async set (`SET`), clock enable (`EN`) — each drivable, wireable, and
  releasable like existing sequential pins.
- **Flop Q State**: The stored output value, now a function of
  (async controls, enable, D, clock) resolved through the single priority
  order; `NQ` remains its live negation.
- **Priority Rule**: The total order reset > set > enable-hold > D-capture
  plus the simultaneous-assertion outcome (reset wins → Q=0); the one
  normative statement users consult for conflicting controls.
- **Stall Domain**: Any subset of flops with enable deasserted that hold Q
  while sibling flops in the same clock domain keep advancing.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Users can bring a 4-flop counter from any reachable state to
  all-zero with a single reset assertion (no clock advances) on the first
  attempt in at least 9 of 10 trials.
- **SC-002**: Users can freeze one pipeline stage for 5 clock advances
  (Q unchanged) while a sibling stage advances normally, verified by
  readback, on the first attempt.
- **SC-003**: Every conflicting-control combination (reset+set,
  async+disabled-enable, controls+D-change) produces the documented winner
  on all three surfaces (interactive session, script file, piped
  invocation) with identical readback.
- **SC-004**: Existing D-only netlists (no new pins wired or driven)
  produce identical step-for-step outputs before and after the feature
  (zero-migration compatibility).
- **SC-005**: A scripted reset → preset → stall → advance → readback
  sequence of 10 steps, replayed twice from the same starting workspace,
  produces identical outputs on every replay.

## Assumptions

- Controls are active-high and level-sensitive while asserted (assert = 1,
  inactive = 0); this matches the binary poke vocabulary (0/1/release)
  and the existing dangling-ties-to-inactive convention.
- Simultaneous async reset + async set resolves to reset wins (Q=0),
  the industry-standard choice that keeps the power-on/reset value (0)
  the safe default for counters and FSM idle states.
- Clock enable is synchronous with the edge (evaluated at the edge like
  D), while set/reset are asynchronous (effective immediately on
  assertion, independent of edges) — the standard split that lets reset
  work before any clock exists.
- Scope is DFF only; DLATCH keeps its current `D`/`EN`/`Q`/`NQ` behavior
  unchanged in this feature.
- Verb spelling, output formatting, error codes, and file formats follow
  existing session conventions; no new transport or graphical display is
  introduced.
- Pin-name matching stays case-insensitive with numeric aliases preserved,
  consistent with the current named-pin rule.
