# Feature Specification: Sequential Session Verbs

**Feature Branch**: `001-seq-session-verbs`

**Created**: 2026-10-31

**Status**: Draft

**Input**: User description: "Headless + session verbs to advance clocks N cycles, poke block inputs, and peek flop Q state. Makes sequential core usable/testable from REPL, pipes, and scripts with zero GUI dependency."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Advance clocks N cycles (Priority: P1)

A hardware-logic user working in the headless REPL, a pipe, or a script
advances one or more clocks by an exact number of cycles and observes the
resulting circuit state, without opening any graphical interface.

**Why this priority**: Clock advancement is the core action that makes the
sequential core usable at all; without it, flop state can never evolve and
nothing else in this feature has value.

**Independent Test**: Can be fully tested by entering the logic workspace,
building a small sequential circuit, advancing a clock by N cycles, and
confirming the reported cycle count and resulting outputs — delivering a
working advance-and-observe loop on its own.

**Acceptance Scenarios**:

1. **Given** a workspace with a sequential circuit and clock `clk` at cycle
   0, **When** the user advances `clk` by 5 cycles, **Then** the session
   reports cycle 5 and flop states reflect exactly 5 sequential updates.
2. **Given** a workspace with two independent clocks, **When** the user
   advances only `clk_a` by 3 cycles, **Then** blocks driven by `clk_b`
   remain unchanged and blocks driven by `clk_a` reflect 3 updates.
3. **Given** the same advance command issued twice on identical starting
   state, **When** both runs complete, **Then** both runs report identical
   flop states and outputs (deterministic replay).

---

### User Story 2 - Poke block inputs (Priority: P1)

A user drives a named block input pin to a chosen logic value (0, 1, or
release/unknown) from the session or a one-shot command, so stimulus can
be applied between clock advances in interactive or scripted flows.

**Why this priority**: Without stimulus control, clock advancement can only
replay whatever inputs already exist; poking is what makes multi-step
test sequences expressible.

**Independent Test**: Can be fully tested by poking an input pin, reading
back the driven value, advancing zero cycles, and confirming the value
holds — delivering controllable stimulus independent of clocking.

**Acceptance Scenarios**:

1. **Given** a workspace with block `u1` exposing input pin `d`,
   **When** the user pokes `u1.d` to 1, **Then** a subsequent read of that
   pin shows the driven value 1.
2. **Given** a poked input pin, **When** the user releases it,
   **Then** the pin returns to its prior undriven/wired behavior and the
   release is reported.

---

### User Story 3 - Peek flop Q state (Priority: P2)

A user inspects the current Q value of one flop, all flops of a block, or
all flops in the workspace, in both interactive sessions and piped
one-shot invocations, so test scripts can assert on sequential state.

**Why this priority**: Observation completes the loop (poke → advance →
peek); it is P2 only because advancement and stimulus already deliver
standalone value, while peeking makes results assertable.

**Independent Test**: Can be fully tested by building a sequential circuit
with a known reset state, peeking without advancing, and confirming the
reported Q values match the reset state — delivering readback with no
other new action.

**Acceptance Scenarios**:

1. **Given** a workspace with flop block `f1` holding Q=1 after an advance,
   **When** the user peeks at `f1`, **Then** the session reports Q=1 for
   that block.
2. **Given** a piped one-shot invocation, **When** the user peeks at all
   flops, **Then** the output lists every flop with its current Q value in
   a stable, script-parseable order.

---

### Edge Cases

- What happens when the advance count is 0 or negative? Zero is a no-op
  that reports current state; negative counts are rejected as usage
  errors without changing state.
- How does the system handle advancing a clock name that does not exist
  or drives nothing? It reports a clear error naming the unknown clock
  and changes no state.
- How does the system handle poking a block/pin that does not exist, is
  not an input, or carries an invalid logic value? It reports a usage
  error identifying the bad target or value and changes no state.
- How does the system handle peeking at a block with no flops or an
  unknown block name? It reports empty-but-successful results for
  flop-less blocks and a clear error for unknown names.
- How does the system behave when combinational and sequential blocks are
  mixed? Combinational outputs settle around each sequential update so
  observed outputs always match the reported cycle.
- How does the system behave across REPL session, one-shot, and piped
  invocations? All three spellings of the same verb produce identical
  state transitions and identical human-readable output for the same
  starting workspace.
- What happens when a named pin is used on the wrong kind? Named pins
  on combinational gates (`AND1.D`) and `NQ` on combinational outputs
  are usage errors naming the bad pin; numeric aliases (`DFF1.1`,
  `DFF1.Y`) always keep working.
- How does a sink wired from `NQ` behave? It reads NOT Q at every
  evaluation, including mid-sequence steps; unwiring clears the
  inversion together with the wire and changes nothing else.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: Users MUST be able to advance a named clock by an exact
  non-negative integer cycle count from both the interactive session and
  one-shot/piped invocations, with identical results.
- **FR-002**: The system MUST report the clock name and resulting cycle
  count after every advance, including a zero-cycle advance.
- **FR-003**: Users MUST be able to drive (poke) a named block input pin
  to a logic value (0, 1, plus a defined release/unknown) and to release
  a previously poked pin back to wired behavior.
- **FR-004**: Users MUST be able to inspect (peek) flop Q state at three
  scopes: a single named flop/block, all flops of a block, and all flops
  in the workspace.
- **FR-005**: Peek output MUST list each flop identity with its current Q
  value in a stable, deterministic order suitable for scripted assertions.
- **FR-006**: All three verbs MUST work headless (REPL, one-shot, pipe,
  script file) with zero graphical dependency.
- **FR-007**: The system MUST reject unknown clock names, unknown
  block/pin targets, non-input poke targets, invalid logic values, and
  negative cycle counts as usage errors that change no simulation state.
- **FR-008**: Repeated identical poke → advance → peek sequences MUST
  produce byte-identical observable results (deterministic replay).
- **FR-009**: Session help and command completion MUST cover the new verbs,
  their argument names, and their value forms, matching existing session
  conventions.
- **FR-010**: Sequential blocks MUST expose named pins: DFF inputs `D`
  / `CLK` and output `Q`; DLATCH inputs `D` / `EN` and output `Q`.
  Numeric spellings (`.1`, `.2`, `.Y`) MUST keep working as aliases;
  help, listings, and saved files MUST show the named forms.
- **FR-011**: Every flop MUST also expose `NQ` (negated Q) as a second
  wireable output; `NQ` MUST always read NOT Q, including power-on
  state and immediately after every step. Dangling `NQ` is ignored by
  default (hidden from listings and block outputs until explicitly
  `use`d or wired); `peek` always shows both Q and NQ regardless.

### Key Entities *(include if feature involves data)*

- **Session Workspace**: The named in-memory logic circuit shared by all
  verbs within one session or one-shot invocation; owns blocks, wires,
  and driven values.
- **Clock Domain**: A named clock signal with a monotonically increasing
  cycle count; advancing it steps exactly the blocks it drives.
- **Block Input Pin**: A named input of a named block that can be
  externally driven to a logic value or released to wired behavior.
- **Flop Q State**: The current stored output value of a sequential
  element, addressable per flop, per block, or workspace-wide.
- **Advance Command**: A requested (clock, cycle-count) pair plus its
  resulting reported cycle count and settled circuit outputs.
- **SeqPinName**: The canonical human-readable spelling of a sequential
  pin (`D`, `CLK`/`EN`, `Q`, `NQ` per kind); numeric forms are accepted
  aliases, canonical display is named, matching is case-insensitive.
- **NQ Output**: The second output of a flop, always the negation of Q,
  wireable to any input (sinks read inverted Q). Dangling `NQ` is
  ignored by default and reported alongside Q wherever flop state is
  shown (`peek`, `list` ignored section); `use` re-exposes it.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Users can build a sequential circuit and observe 1-cycle and
  100-cycle advances producing the correct flop states on the first
  attempt in at least 9 of 10 trials.
- **SC-002**: A scripted poke → advance → peek sequence of 10 steps,
  replayed twice from the same starting workspace, produces identical
  outputs on every replay.
- **SC-003**: All three verbs work identically across interactive session,
  one-shot, and piped invocation for the same workspace (same inputs →
  same reported outputs).
- **SC-004**: Every invalid input (unknown clock, unknown block/pin,
  non-input poke, invalid value, negative count) yields a clear error
  message naming the problem within one command round-trip, with no state
  change.
- **SC-005**: New-verb help text is discoverable from the session help
  listing, and tab-completion suggests the new verbs and their argument
  names.

## Assumptions

- Users interact through the existing headless CLI surfaces (interactive
  REPL prompt, one-shot command line, stdin pipes, script files); no new
  transport is introduced.
- Verb spelling and output formatting follow the existing session verb
  conventions (lowercase verbs, `--key value` options, human-readable
  stdout, errors to stderr, exit code 2 for usage errors).
- The sequential evaluation core already exists and is correct; this
  feature exposes control and observation verbs over it rather than
  changing evaluation semantics.
- Logic value vocabulary is binary plus release/unknown (0, 1, release);
  wider or multi-bit poke encodings are out of scope for this feature.
- Graphical display of sequential state is out of scope; observation is
  textual only.
- Named-pin vocabulary is fixed: DFF (`D`, `CLK`, `Q`, `NQ`), DLATCH
  (`D`, `EN`, `Q`, `NQ`); case-insensitive; numeric aliases preserved;
  no other pin names exist.
