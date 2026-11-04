# Feature Specification: SR/D Latch Pair

**Feature Branch**: `004-sr-d-latches`

**Created**: 2026-11-01

**Status**: Draft

**Input**: User description: "Level-sensitive latch pair with documented transparent/frozen behavior and illegal-input refusal. Completes the basic sequential palette (edge + level); needed for bus hold and async handshake examples."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Hold a bus value with a transparent D latch (Priority: P1)

A hardware-logic user builds a bus-hold cell from a D latch: while the
enable is active the output follows the data input transparently, and
when enable is released the output freezes at its last value while data
keeps toggling — so a shared bus can be sampled and then held stable
with no clock involved.

**Why this priority**: Bus hold is the stated motivating use and the
simplest proof that level behavior works; it delivers standalone value
with a single primitive and no edge-triggered flop involvement.

**Independent Test**: Can be fully tested by placing one D latch,
driving data and enable, observing follow-while-enabled then
freeze-while-disabled across data toggles — delivering a working
transparent/frozen loop with no SR-latch involvement.

**Acceptance Scenarios**:

1. **Given** a D latch holding Q=0 with enable active, **When** the user
   drives data to 1, **Then** Q becomes 1 without any clock advance
   (transparent follow).
2. **Given** a D latch with Q=1 and enable then released, **When** the
   user toggles data between 0 and 1 several times, **Then** Q stays 1
   across all toggles (frozen hold).
3. **Given** a D latch with enable released and Q frozen, **When** the
   user re-asserts enable, **Then** Q immediately resumes following the
   current data value.

---

### User Story 2 - Build an async handshake from an SR latch (Priority: P1)

A user builds a minimal asynchronous handshake (e.g. set on request,
reset on acknowledge) from an SR latch with no clock: driving set
asserts the stored bit, driving reset clears it, and driving both at
once is refused loudly instead of silently latching an undefined
value.

**Why this priority**: The async handshake is the second stated
motivating use; together with the D-latch bus hold it covers the
feature's purpose (level-sensitive pair for clock-free holding and
signalling).

**Independent Test**: Can be fully tested by placing one SR latch,
driving set then reset in sequence with readback after each, plus one
attempted illegal set+reset drive that is rejected with state
unchanged — delivering set/reset/hold plus refusal with no D-latch
involvement.

**Acceptance Scenarios**:

1. **Given** an SR latch holding Q=0, **When** the user drives set
   active (reset inactive), **Then** Q becomes 1 with no clock advance.
2. **Given** an SR latch holding Q=1, **When** the user drives reset
   active (set inactive), **Then** Q becomes 0 with no clock advance.
3. **Given** an SR latch holding a known Q, **When** the user attempts
   to drive set and reset active at the same time, **Then** the system
   rejects the attempt as a usage error naming the illegal combination
   and Q is unchanged.
4. **Given** an SR latch with both inputs released (inactive), **When**
   the user observes Q after a set-then-release sequence, **Then** Q
   holds its last set/reset value (memory), identical on every replay.

---

### User Story 3 - Mix level latches with edge flops on one workspace (Priority: P2)

A user combines level-sensitive latches (SR, D) with edge-triggered
flops (DFF, JK, T) in a single workspace, using the same drive /
readback verbs for every kind, and observes each kind following its own
documented rule while shared clock advances step only the edge kinds.

**Why this priority**: The feature's value is completing the palette on
the *same* workspace — mixed designs (e.g. an SR handshake flag feeding
a DFF pipeline register beside a D-latch bus hold) must work without
separate stepping paths or verb dialects.

**Independent Test**: Can be fully tested by placing one D latch, one
SR latch, and one DFF in one workspace, exercising latch transparency
and flop edge capture side by side and confirming each Q follows its
own rule with one consistent readback — delivering kind coexistence
with no new verb.

**Acceptance Scenarios**:

1. **Given** one D latch (enable active), one SR latch, and one DFF in
   the same workspace, **When** the user drives each kind's inputs and
   advances the shared clock by 1, **Then** the latches show their
   level-resolved Q while the DFF shows its edge-captured Q, all
   visible in one readback.
2. **Given** the identical drive → observe → advance sequence run twice
   from the same starting workspace, **When** both runs complete,
   **Then** both report identical latch and flop states (deterministic
   replay).

---

### User Story 4 - Read one documented rule per latch kind (Priority: P3)

A user reads a short documented rule per kind (when the D latch is
transparent vs. frozen; what each SR input combination means including
the illegal one) and predicts the stored value for every input
combination without trial and error, in interactive sessions, scripts,
and saved files alike.

**Why this priority**: Two level primitives are only usable if their
level rule is stated once, normatively, and observed identically on
every surface — the same documentation discipline that made the
edge-triggered tables usable.

**Independent Test**: Can be fully tested by driving every row of each
documented table once and confirming the observed stored value matches
the documented outcome — delivering the tables themselves as observable
behavior.

**Acceptance Scenarios**:

1. **Given** the documented D-latch rule, **When** the user drives each
   enable/data combination, **Then** Q follows data while enabled and
   holds while disabled in all cases.
2. **Given** the documented SR-latch table, **When** the user drives
   each of the 4 S/R combinations, **Then** three produce the documented
   stored value (set / reset / hold) and the fourth is rejected as the
   documented illegal combination.

---

### Edge Cases

- What happens when set and reset are both driven active including
  through wiring (not just direct drive)? The documented illegal
  combination is refused loudly with state unchanged, regardless of
  whether it arises from a direct drive, a wired net, or a loaded file.
- What happens when enable or data on a D latch is driven to an invalid
  (non-binary) value? It is rejected as a usage error that changes no
  state, consistent with existing drive-value validation.
- What happens when a latch input pin is left unconnected (dangling)?
  It reads the inactive value (0 → the hold row), so a bare latch holds
  state; like any other dangling pin it appears in the block interface
  until explicitly wired or ignored.
- How do stored values behave across clock advances? Frozen latches
  (D latch with enable released, SR latch with both inputs released)
  hold Q across any number of advances; transparent D latches resolve
  to the current data value and remain transparent across advances
  while enable stays active.
- How do NQ, readback, and saved files reflect the latches? NQ always
  reads NOT Q including immediately after a level change; readback
  shows Q/NQ in the same form as flops; saved files round-trip the
  latch blocks, their wires, and driven values, and legacy files
  without the new kind load exactly as before.
- What happens on power-on before any input is driven? Latches power on
  at Q=0 exactly like existing sequential elements.
- What happens when a latch pin name is used on the wrong kind (e.g. S
  on a DFF, J on an SR latch, S on a D latch)? It is rejected as a
  usage error naming the bad pin; numeric aliases keep working per
  existing conventions.
- How do the latches interact with multi-clock workspaces? Level
  latches belong to no clock domain — advances never gate their
  level-resolved Q; edge flops in the same workspace step per domain
  exactly as today.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The system MUST provide a D latch kind with data input `D`,
  enable input `EN`, and outputs `Q` / `NQ` with level-sensitive
  behavior: while `EN` is active Q follows `D` transparently with no
  clock advance required; while `EN` is inactive Q holds its last value
  across input changes and clock advances.
- **FR-002**: The system MUST provide an SR latch kind with inputs `S`
  and `R` and outputs `Q` / `NQ` with level-sensitive behavior: set
  (`S` active, `R` inactive) drives Q to 1; reset (`S` inactive, `R`
  active) drives Q to 0; released (`S` and `R` both inactive) holds Q;
  all with no clock advance required.
- **FR-003**: The system MUST refuse the illegal SR combination (`S`
  and `R` both active) loudly as a usage error that changes no
  simulation state, regardless of whether the combination arises from a
  direct drive, wired logic, tied pins, or a loaded file; the error
  MUST name the illegal combination.
- **FR-004**: Both latch kinds MUST resolve level behavior outside clock
  advances (drive/wire/load makes the new Q visible on the next
  readback with no advance) while remaining stable across advances per
  FR-001/FR-002 (frozen latches hold; transparent D latches track `D`).
- **FR-005**: All existing session verbs MUST work on both latch kinds
  with the same conventions as today: inputs wireable between blocks,
  drivable/releasable via poke, listed via named pins, Q/NQ readable
  via peek, and persisted through save/load files with round-trip
  fidelity.
- **FR-006**: Unconnected latch pins MUST default to inactive (0), i.e.
  the hold row — so undriven latches hold state and existing netlists
  behave exactly as before (zero migration).
- **FR-007**: `NQ` on both latch kinds MUST always read NOT Q,
  including power-on state and immediately after every level change,
  consistent with existing sequential elements.
- **FR-008**: Repeated identical drive → observe → advance sequences
  involving latches MUST produce byte-identical observable results
  (deterministic replay across REPL, scripts, pipes, and replayed
  files).
- **FR-009**: Invalid stimulus (non-binary values on `D`/`EN`/`S`/`R`,
  unknown pin names, latch pins on the wrong kind) MUST be rejected as
  usage errors that change no simulation state, consistent with
  existing poke/wire error behavior.
- **FR-010**: Session help and command completion MUST cover the new
  kind name(s), their pin names and value forms, the transparent/frozen
  rule, and the SR table including the illegal combination, matching
  existing session conventions.
- **FR-011**: The transparent/frozen rule and the SR table including
  the illegal combination MUST be user-visible in help/session
  documentation and MUST be observable: every documented input
  combination MUST produce the documented outcome deterministically
  across REPL sessions, scripts, pipes, and replayed files.

### Key Entities *(include if feature involves data)*

- **D Latch**: A level-sensitive storage element with `D`/`EN` inputs;
  its stored value follows `D` while enabled (transparent) and freezes
  while disabled (hold). Used for bus-hold cells.
- **SR Latch**: A level-sensitive storage element with `S`/`R` inputs;
  its stored value is set, reset, or held per the three legal input
  combinations; the fourth combination is illegal and refused. Used for
  async handshake flags.
- **Transparent/Frozen Rule**: The normative per-kind statement users
  consult to predict latch Q: enable-gated follow-vs-hold for the D
  latch; set/reset/hold/illegal for the SR latch.
- **Illegal Combination**: The `S` active + `R` active state of the SR
  latch; a usage error that is always rejected loudly with state
  unchanged, never silently held or resolved.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Users can build a D-latch bus-hold cell and demonstrate
  follow-while-enabled then freeze-across-5-data-toggles readback on
  the first attempt in at least 9 of 10 trials.
- **SC-002**: Users can drive all four SR input combinations on one
  latch and observe the three documented stored values plus the loud
  illegal-combination refusal (state unchanged) on the first attempt.
- **SC-003**: A mixed workspace (at least one level latch and one
  edge-triggered flop) exercised through drive → readback → advance
  produces per-kind-correct Q on all three surfaces (interactive
  session, script file, piped invocation) with identical readback.
- **SC-004**: Existing netlists containing no SR latch blocks produce
  identical step-for-step outputs before and after the feature
  (zero-migration compatibility, modulo the documented DLATCH
  edge-to-level refinement stated in the help).
- **SC-005**: A scripted level-drive → readback → advance sequence of
  10 steps exercising both latch kinds, replayed twice from the same
  starting workspace, produces identical outputs on every replay.

## Assumptions

- Controls are active-high and level-sensitive (driven = 1, inactive =
  0); this matches the binary poke vocabulary (0/1/release) and the
  existing dangling-ties-to-inactive convention.
- The existing `DLATCH` vocabulary (`D`/`EN`, exactly 2 inputs) is the
  D latch of this pair: this feature refines its semantics from the
  documented edge-capture simplification to true transparent/frozen
  level behavior, keeping its kind name, pin names, and fan-in.
- The SR latch is asynchronous with no enable or clock pin in this
  feature (hold is expressed as `S=R=0`); gated variants are out of
  scope.
- Simultaneous `S=R=1` is always illegal and refused (no
  reset-dominant or set-dominant alternative); the safe default is loud
  refusal with state unchanged.
- Pin-name matching stays case-insensitive with numeric aliases
  preserved, consistent with the current named-pin rule.
- Verb spelling, output formatting, error codes, and file formats follow
  existing session conventions; no new transport or graphical display
  is introduced.
- Scope is the level-sensitive pair only: DFF/JK/T edge behavior,
  existing verb spellings, and existing file-schema compatibility
  (legacy files load) are unchanged.
