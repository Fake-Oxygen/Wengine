# Feature Specification: JK/T Flop Kinds

**Feature Branch**: `003-jk-t-flops`

**Created**: 2026-11-01

**Status**: Draft

**Input**: User description: "JK (hold/set/reset/toggle) and T (toggle/hold) flop kinds on the same stepper. Covers classic counter/divider designs without emulating them from DFFs; small additive kind extension."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Divide a clock with chained T flops (Priority: P1)

A hardware-logic user builds a frequency divider (ripple or synchronous)
by chaining T flops with T held active, advancing the clock N cycles, and
observing each stage toggle at half the rate of the previous one — the
classic divider design expressed directly instead of by emulating toggle
behavior from D flops plus external feedback wiring.

**Why this priority**: The divider is the canonical T-flop circuit and the
simplest proof that the new kind works on the existing clock stepper; it
delivers standalone value with a single new primitive.

**Independent Test**: Can be fully tested by building a 2-stage T divider,
asserting toggle on both flops, advancing 4 clock cycles, and reading back
the divide-by-2 / divide-by-4 pattern — delivering a working divider with
no JK involvement.

**Acceptance Scenarios**:

1. **Given** a T flop holding Q=0 with toggle input active, **When** the
   user advances the clock by 1 cycle, **Then** Q becomes 1; after one
   more cycle Q returns to 0.
2. **Given** a T flop holding Q=1 with toggle input inactive,
   **When** the user advances the clock by 3 cycles, **Then** Q is still
   1 across all 3 advances.
3. **Given** two chained T flops forming a divide-by-4 chain,
   **When** the user advances 4 cycles, **Then** the first stage has
   toggled 4 times and the second stage twice, as shown by readback.

---

### User Story 2 - Build a counter from JK flops (Priority: P1)

A user builds a small synchronous counter from JK flops, driving the
J/K input pairs per stage through all four behaviors (hold, reset,
set, toggle) and advancing the clock to step the count — the classic
counter design expressed with one primitive per bit instead of D flops
plus per-bit combinational excitation logic.

**Why this priority**: The counter is the canonical JK circuit; together
with the T divider it covers the feature's stated purpose (classic
counter/divider designs without DFF emulation).

**Independent Test**: Can be fully tested by wiring a 2-bit JK counter,
exercising each J/K combination on one flop, advancing clocks, and
confirming the four behaviors — delivering the full JK truth table with
no T-flop involvement.

**Acceptance Scenarios**:

1. **Given** a JK flop holding Q=0, **When** the user drives set inputs
   (J active, K inactive) and advances 1 cycle, **Then** Q becomes 1.
2. **Given** a JK flop holding Q=1, **When** the user drives reset
   inputs (J inactive, K active) and advances 1 cycle, **Then** Q
   becomes 0.
3. **Given** a JK flop with both J and K active, **When** the user
   advances 1 cycle, **Then** Q inverts (toggle); with both J and K
   inactive, **When** the user advances 3 cycles, **Then** Q is
   unchanged (hold).
4. **Given** a 2-bit JK counter starting at zero, **When** the user
   advances 4 cycles with toggle wiring, **Then** readback shows the
   count sequence 00 → 01 → 10 → 11 → 00.

---

### User Story 3 - Mix DFF, JK, and T flops on one clock stepper (Priority: P2)

A user combines DFF, JK, and T flops in a single workspace driven by the
same clock, using the same advance / drive / readback verbs for every
kind, and observes all three kinds stepping together deterministically.

**Why this priority**: The feature's value is additive kinds on the
*same* stepper — mixed designs (e.g. a JK counter feeding a DFF pipeline
register beside a T divider) must work without separate stepping paths.

**Independent Test**: Can be fully tested by placing one flop of each
kind in one clock domain, advancing N cycles, and confirming each flop
followed its own input table while sharing the reported cycle count —
delivering kind coexistence with no new verb.

**Acceptance Scenarios**:

1. **Given** one DFF, one JK flop, and one T flop in the same clock
   domain, **When** the user advances 2 cycles, **Then** each flop's Q
   reflects its own input table and all three report the same cycle
   count.
2. **Given** the identical mixed poke → advance → readback sequence run
   twice from the same starting workspace, **When** both runs complete,
   **Then** both report identical flop states (deterministic replay).

---

### User Story 4 - Read one excitation table per kind (Priority: P3)

A user reads a short documented input table for each new kind (which
J/K/T combination means hold, set, reset, toggle) and predicts the next
state after one clock advance without trial and error, in interactive
sessions, scripts, and saved files alike.

**Why this priority**: Two new primitives are only usable if their
input-to-next-state mapping is stated once, normatively, and observed
identically on every surface.

**Independent Test**: Can be fully tested by driving every row of each
table once, advancing one cycle per row, and confirming the documented
next state — delivering the tables themselves as observable behavior.

**Acceptance Scenarios**:

1. **Given** the documented JK table, **When** the user drives each of
   the 4 J/K combinations and advances one cycle, **Then** Q follows
   the table (hold / reset / set / toggle) in all 4 cases.
2. **Given** the documented T table, **When** the user drives each of
   the 2 T values and advances one cycle, **Then** Q holds or toggles
   exactly as documented.

---

### Edge Cases

- What happens when J, K, or T is driven to an invalid (non-binary)
  value? It is rejected as a usage error that changes no state,
  consistent with existing drive-value validation.
- What happens when a J/K/T input pin is left unconnected (dangling)?
  It reads the inactive value (0 → the hold row of the table), so a
  bare flop with nothing driven holds Q across advances; like any other
  dangling pin it appears in the block interface until explicitly wired
  or ignored.
- How do NQ, readback, and saved files reflect the new kinds? NQ always
  reads NOT Q; readback shows Q/NQ unchanged in form with the new kind
  names; saved files round-trip the new blocks, their wires, and driven
  values, and legacy files without the new kinds load exactly as before.
- What happens on power-on before any input is driven? New flops power
  on at Q=0 exactly like existing flops; the new kinds add no change to
  the power-on state.
- What happens when a new-kind pin name is used on the wrong kind
  (e.g. J on a DFF, D on a JK flop)? It is rejected as a usage error
  naming the bad pin; numeric aliases keep working per existing
  conventions.
- How do the new kinds interact with multi-clock workspaces? Each new
  flop belongs to exactly one clock domain like existing flops;
  advancing one clock steps only the flops it drives.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The system MUST provide a JK flop kind with synchronous
  inputs `J`, `K`, a clock input `CLK`, and outputs `Q` / `NQ`.
- **FR-002**: The system MUST provide a T flop kind with synchronous
  input `T`, a clock input `CLK`, and outputs `Q` / `NQ`.
- **FR-003**: On each clock advance, a JK flop MUST resolve its next Q
  from the single excitation table: J=0,K=0 → hold; J=0,K=1 → Q=0
  (reset); J=1,K=0 → Q=1 (set); J=1,K=1 → invert Q (toggle).
- **FR-004**: On each clock advance, a T flop MUST resolve its next Q
  from the single excitation table: T=0 → hold; T=1 → invert Q
  (toggle).
- **FR-005**: Both new kinds MUST step on the existing clock stepper
  (same advance mechanics, same per-domain selectivity, same cycle
  reporting) alongside existing flop kinds; no separate stepping path.
- **FR-006**: All existing session verbs MUST work on the new kinds with
  the same conventions as today: inputs wireable between blocks,
  drivable/releasable via poke, listed via named pins, Q/NQ readable
  via peek, and persisted through save/load files with round-trip
  fidelity.
- **FR-007**: Unconnected J/K/T pins MUST default to inactive (0), i.e.
  the hold row — so undriven new flops hold state and existing netlists
  behave exactly as before (zero migration).
- **FR-008**: `NQ` on the new kinds MUST always read NOT Q, including
  power-on state and immediately after every step, consistent with
  existing flops.
- **FR-009**: Repeated identical drive → advance → observe sequences
  involving the new kinds MUST produce byte-identical observable
  results (deterministic replay across REPL, scripts, pipes, and
  replayed files).
- **FR-010**: Invalid stimulus (non-binary values on J/K/T/CLK, unknown
  pin names, new-kind pins on the wrong kind) MUST be rejected as usage
  errors that change no simulation state, consistent with existing
  poke/wire error behavior.
- **FR-011**: Session help and command completion MUST cover the new
  kind names, their pin names and value forms, and both excitation
  tables, matching existing session conventions.

### Key Entities *(include if feature involves data)*

- **JK Flop**: A clocked storage element with J/K data inputs; its next
  state is given by the four-row excitation table (hold / reset / set /
  toggle). Synchronous-only in this feature.
- **T Flop**: A clocked storage element with a single T control input;
  its next state is hold (T=0) or invert (T=1). Synchronous-only in this
  feature.
- **Excitation Table**: The normative per-kind mapping from driven
  inputs to next Q, evaluated at the clock advance; the one statement
  users consult to predict flop behavior.
- **Shared Stepper**: The existing clock-advance mechanism the new kinds
  ride on — one domain advance steps every driven flop of every kind in
  that domain and reports a single cycle count.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Users can build a 2-stage T divider and observe the
  divide-by-2 / divide-by-4 readback pattern over 4 advances on the
  first attempt in at least 9 of 10 trials.
- **SC-002**: Users can drive all four JK input combinations on one
  flop and observe the documented next state (hold / reset / set /
  toggle) after one advance per combination, on the first attempt.
- **SC-003**: A mixed workspace (at least one DFF, one JK, one T flop
  in one clock domain) advanced twice produces per-kind-correct Q on
  all three surfaces (interactive session, script file, piped
  invocation) with identical readback.
- **SC-004**: Existing netlists containing no JK/T blocks produce
  identical step-for-step outputs before and after the feature
  (zero-migration compatibility).
- **SC-005**: A scripted drive → advance → readback sequence of 10
  steps exercising both new kinds, replayed twice from the same
  starting workspace, produces identical outputs on every replay.

## Assumptions

- The J/K/T "set/reset" rows are synchronous input combinations
  evaluated at the clock advance — not asynchronous preset/clear pins.
  Async controls remain DFF-only; JK/T expose no RST/SET/EN pins in
  this feature (hold is expressed as J=K=0 / T=0).
- Controls are active-high and level-sensitive at the edge (driven = 1,
  inactive = 0); this matches the binary poke vocabulary (0/1/release)
  and the existing dangling-ties-to-inactive convention.
- Pin-name matching stays case-insensitive with numeric aliases
  preserved, consistent with the current named-pin rule.
- Verb spelling, output formatting, error codes, and file formats follow
  existing session conventions; no new transport or graphical display
  is introduced.
- Scope is additive kinds only: DFF and DLATCH behavior, existing file
  formats, and existing verb spellings are unchanged.
