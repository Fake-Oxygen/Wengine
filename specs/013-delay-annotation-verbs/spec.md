# Feature Specification: Propagation-Delay Annotation Verbs on the Reserved Gate Field

**Feature Branch**: `013-delay-annotation-verbs`

**Created**: 2026-11-20

**Status**: Draft

**Input**: User description: "What: User-facing delay annotation verbs on the reserved propagation_delay_ps field. Why: Field exists but is unwritable; annotation must land before any delay simulator can honor it."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Annotate a gate with a propagation delay (Priority: P1)

A user who has placed gates in a logic workspace annotates one of them
— addressed by its display name (AND1, DFF2, TBUF3, the module's
existing per-kind identity) — with a propagation delay: a non-negative
duration the gate's output will one day take to respond. The module
accepts the value, stores it on that gate as a whole number of
picoseconds, and the workspace listing shows the annotation on that
gate. A bare number means picoseconds (the storage unit); a
time-suffixed value (`1.5ns`, `250ps`, `2us`) converts exactly through
the engine's single owned time-suffix table. Anything that is not a
non-negative whole-picosecond duration — negative values, fractional
picoseconds, values beyond the field's range, cross-dimension text
(`1500V`), mistyped or unknown gate names — is refused loudly with an
actionable message and the workspace untouched.

**Why this priority**: This is the entire point of the feature: the
reserved field exists on every gate, is already persisted by every
save format, is already preserved or reset by documented optimizer
rules — and today nothing the user can type will write it. Every
other story (inertness, persistence, vocabulary) protects or
transports this one write.

**Independent Test**: Can be fully tested by placing a gate,
annotating it with both a bare-picosecond and a unit-suffixed value,
and asserting the stored value and the listing readback match an
independently derived conversion — delivering writability with no
persistence or optimizer story involved.

**Acceptance Scenarios**:

1. **Given** a live gate, **When** the user annotates it with a whole
   non-negative picosecond value (bare or exactly-convertible
   suffixed form), **Then** the value is stored on that gate and the
   listing shows it against that gate and only that gate.
2. **Given** a unit-suffixed time value that converts to a whole
   number of picoseconds (`1.5ns`), **When** the user annotates a
   gate, **Then** the stored value is the exact conversion (1500ps)
   — parsed through the single owned time-suffix table, never a
   per-verb conversion table.
3. **Given** an annotation request naming a gate that does not exist,
   or a dead slot, or a malformed name, **When** the verb runs,
   **Then** it fails loudly with the module's runtime/usage error
   conventions and the workspace is untouched.
4. **Given** a value that is negative, fractional in picoseconds
   (`1.5ps`), larger than the field holds, or dimensionally wrong,
   **When** the user attempts the annotation, **Then** it is refused
   loudly with the specific bound or reason stated, and no partial
   write occurs.
5. **Given** an already-annotated gate, **When** the user annotates it
   again, **Then** the last write wins deterministically — the same
   command sequence always produces the same stored state.

---

### User Story 2 - The annotation is inert: stored, never interpreted (Priority: P1)

An annotated netlist behaves exactly like the same netlist without
annotations. Evaluation stays zero-delay: solving, stepping, truth
tables, and exports produce identical values, identical ordering,
identical byte output — the annotation rides along in storage while
no code path reads it. The user's reason to annotate anyway is
explicit: the module's extension map names a future delay evaluator
that will honor the field, and this feature deliberately ships the
annotation surface ahead of it. Flows that annotate nothing — every
existing golden, saved workspace, and refusal message — are
byte-identical to before the feature.

**Why this priority**: The module's standing contract is that stored
values never silently change evaluation. An annotation that perturbed
solve output would fork the value system the same way a second
resolver would; pinning inertness is what makes story 1 safe to ship
before the delay evaluator exists.

**Independent Test**: Can be fully tested by evaluating the same
netlist twice — once un-annotated, once with every gate heavily
annotated — and asserting solve/step/truth/export outputs are
byte-identical, plus asserting every pre-existing test and golden
passes unchanged — delivering the zero-delay guarantee with no
persistence story involved.

**Acceptance Scenarios**:

1. **Given** any netlist carrying any annotations, **When** it is
   evaluated (solve, step, sequential stepping), **Then** every value
   and every output is identical to the same netlist with all
   annotations removed.
2. **Given** any netlist carrying any annotations, **When** truth
   tables or exports are produced, **Then** the outputs are identical
   to the un-annotated netlist's.
3. **Given** any flow that annotates nothing (every pre-feature
   workspace, golden, and command sequence), **When** it runs after
   this feature, **Then** every output is byte-identical to before
   the feature.
4. **Given** annotations on gates feeding shared nets, pulled nets,
   or tied inputs, **When** the netlist evaluates, **Then** resolution
   is exactly what the shipped funnel already gives — the annotation
   has no interplay with any topology attribute.

---

### User Story 3 - Clear the annotation, and know exactly where it persists (Priority: P2)

The user can clear an annotation, restoring the gate to its default
un-annotated state; a cleared gate is indistinguishable from a never
annotated one in every flow. The user can also predict where
annotations survive: saving and reloading a workspace brings every
annotation back identically and replays identically; a workspace saved
before this feature (the field defaulting to zero everywhere) loads
byte-identically with no reinterpretation. Optimizer levels behave
exactly as their standing contract already documents: passes that
rewrite gates in place preserve the annotation in place, and the
level that reshapes the network resets it — no pass changes its
observable resolution behavior because of an annotation, and no pass
gains new annotation-specific rules.

**Why this priority**: A value the user cannot remove, or cannot
predict across save/load and optimization, would poison the
annotation surface's credibility — but these are enablement and
preservation duties over an already-working write, hence P2.

**Independent Test**: Can be fully tested by annotating, clearing, and
asserting the gate is byte-indistinguishable from never-annotated;
saving an annotated workspace, reloading it, and asserting identical
annotations and replay; loading a pre-feature workspace and asserting
byte-identical behavior; and running every optimizer level over
annotated netlists and asserting the documented in-place/reshape
outcome with resolution results unchanged.

**Acceptance Scenarios**:

1. **Given** an annotated gate, **When** the user clears the
   annotation, **Then** the gate returns to the default state and
   every subsequent flow (evaluate, save, list, optimize) treats it
   exactly as a never-annotated gate.
2. **Given** a workspace with annotated gates, **When** it is saved
   and reloaded, **Then** every annotation returns identically and
   replay output is identical.
3. **Given** a workspace saved before this feature, **When** it is
   loaded after the feature, **Then** it loads byte-identically —
   the field's zero default and every save format already carried
   the value, so no file is reinterpreted.
4. **Given** an annotated netlist, **When** an in-place optimizer
   level runs, **Then** the annotation survives on its gate (in
   place), and observable resolution results are unchanged.
5. **Given** an annotated netlist, **When** the reshaping optimizer
   level runs, **Then** the annotation resets exactly as the level's
   standing contract documents (the network is rebuilt), the reset
   is not silent in that contract's reporting, and resolution
   results are unchanged.
6. **Given** a gate carrying an annotation, **When** the gate is
   removed, **Then** its annotation goes with it — no dangling value
   survives, and a slot recycled later starts un-annotated.

---

### User Story 4 - One vocabulary across every driving surface (Priority: P2)

The new capability is first-class module vocabulary inside the logic
command domain (no new top-level command): verbs to set and clear,
addressing by the existing display names, with exactly the engine
seam's validation. Completion covers the new verb names and their
value forms; the interactive terminal surfaces compose the same
invocations with byte-identical output by construction; every
registry-size check, run example, module-table line, and test-list
line updates in the same change. No surface reimplements parsing or
validation — each drives the same acceptance and refusal set.

**Why this priority**: The module's standing contracts (one command
domain, interactive/headless byte parity, registry checks) keep the
feature from forking behavior per front end. They are required for
coherence but add no new expressive capability, hence P2.

**Independent Test**: Can be fully tested by driving the same
set/clear operations through the session verbs and asserting the same
acceptances/refusals as the engine seam, asserting completion offers
the new names and value forms, and asserting interactive output is
byte-identical to headless, pinned by the full-registry parity tests.

**Acceptance Scenarios**:

1. **Given** the module vocabulary, **When** the user sets and clears
   annotations through the session verbs, **Then** validation matches
   the engine seam exactly — same refusals, same acceptance, same
   stored values.
2. **Given** completion over the new verbs, **When** verb names and
   value forms are requested, **Then** the new names and forms
   complete.
3. **Given** the interactive terminal surfaces, **When** they compose
   the same invocations, **Then** output is byte-identical to
   headless by construction, pinned by the parity tests.
4. **Given** the registry and documentation duties, **When** the
   feature lands, **Then** every registry-size check, README run
   example, module-table entry, and test-list line updates in the
   same change.

---

### Edge Cases

- What happens when **the value has sub-picosecond precision**
  (`1.5ps`, `0.0005ns`)? → loud refusal: the field stores whole
  picoseconds and nothing may round or truncate silently.
- What happens when **the value is negative or dimensionally wrong**
  (`-250ps`, `1500V`)? → loud refusal naming the reason; a duration
  is the only accepted dimension.
- What happens when **the value overflows the field**
  (durations of hours)? → loud refusal stating the bound — the
  field's range is the contract, not a silent clamp.
- What happens when **the gate name is mistyped, duplicated in
  intent, or names a dead slot**? → loud refusal; identity stays the
  module's per-kind never-reused counters, and only live gates are
  addressable.
- What happens when **a value of exactly zero is set**? → accepted
  and identical to clearing: zero is the default state, so every
  zero-annotation gate is byte-indistinguishable from a
  never-annotated one.
- What happens when **the gate is sequential (DFF, latch, JK, T,
  SR)**? → annotation is allowed uniformly; what a delay means for a
  clocked element (clock-to-Q, setup honor) is decided by the future
  delay evaluator, not by this feature, and nothing stored today
  constrains it.
- What happens when **the reshaping optimizer level runs on an
  annotated netlist**? → the annotation resets, exactly as the
  level's standing contract documents for the reserved field; no new
  optimizer behavior lands, and the reset stays visible in the
  level's reported contract rather than surprising the user.
- What happens when **an annotated gate is removed**? → the
  annotation is removed with it; recycled slots start at the default
  zero, so no annotation leaks to a future gate.
- What happens when **two annotations target the same gate**? → the
  last write wins, deterministic and immediate; no accumulation, no
  history.
- What happens to **`draw`, truth, and export outputs on annotated
  netlists**? → unchanged: annotation display lives in the listing
  surface only, and rendering takes no delay input.
- What happens when **the user annotates before wiring, after
  wiring, or between evaluations**? → order is irrelevant: the
  annotation is stored topology-independent state and never gates
  any other operation.
- What happens when **a workspace saved by a future delay-aware
  version is loaded by this one**? → the values round-trip through
  the already-shipped persistence unchanged; honoring them is the
  future evaluator's job.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The module MUST provide a user-facing verb that
  annotates a live gate — addressed by its display name — with a
  non-negative propagation delay stored as a whole number of
  picoseconds on the reserved per-gate field, inside the existing
  logic command domain (no new top-level command).
- **FR-002**: Delay values MUST accept a bare-integer form meaning
  picoseconds and time-suffixed forms parsed through the engine's
  single owned time-suffix table; any value that does not convert
  exactly to a whole number of picoseconds MUST be refused loudly
  (no rounding, no truncation, no per-verb suffix tables).
- **FR-003**: Values that are negative, dimensionally wrong, or
  beyond the field's range MUST be refused loudly with the specific
  reason or bound stated; a mistyped, unknown, or dead gate name MUST
  be refused loudly; in every refusal the workspace MUST remain
  untouched and the module's usage/runtime exit conventions MUST
  apply.
- **FR-004**: The module MUST provide an explicit way to clear an
  annotation, and a cleared gate MUST be indistinguishable from a
  never-annotated gate in every observable flow (evaluation, listing,
  save, optimization).
- **FR-005**: The module's listing surface MUST show which gates
  carry which delay annotation; un-annotated netlists MUST list
  byte-identically to before the feature.
- **FR-006**: Annotation MUST be inert: evaluation, stepping, truth
  tables, and export outputs MUST be identical with any annotations
  in place; no code path in this feature MAY read the field to alter
  behavior.
- **FR-007**: Annotations MUST persist through workspace save and
  reload identically and replay byte-identically; workspaces saved
  before the feature MUST load byte-identically with no
  reinterpretation (the field and its zero default already ship in
  every save format).
- **FR-008**: Annotation MUST be available on any live gate
  regardless of kind (combinational and sequential alike); per-kind
  timing semantics MUST remain the future delay evaluator's to
  define.
- **FR-009**: Optimizer behavior MUST remain exactly as documented:
  in-place levels preserve annotations in place, the reshaping level
  resets them per its standing contract, and no level MAY observably
  change resolution results or gain annotation-specific rules.
- **FR-010**: Removing a gate MUST remove its annotation; recycled
  slots MUST start un-annotated; no dangling annotation value may
  survive any removal or reset path.
- **FR-011**: Re-annotation MUST be last-write-wins and
  deterministic: identical command sequences on identical workspaces
  MUST produce identical stored annotations and identical outputs.
- **FR-012**: The new vocabulary MUST live inside the module's existing
  command domain (no new top-level command) and follow its verb
  conventions, with completion covering verb names and value forms;
  interactive terminal surfaces MUST produce byte-identical output to
  headless by composing the same invocations; every registry-size
  check, run example, module-table entry, and test-list line MUST
  update in the same change.
- **FR-013**: Every verb acceptance and refusal MUST match the engine
  seam exactly across all surfaces; no surface MAY reimplement
  parsing or validation.
- **FR-014**: The feature MUST be verified at the module's testing
  boundaries: independently derived value conversions (never the
  implementation's own readback), CLI representation, saved
  artifact, and re-read artifact, including every refusal class and
  the zero-value/cleared equivalence.

### Key Entities *(include if feature involves data)*

- **Propagation-delay annotation**: the per-gate stored duration
  (whole picoseconds, default zero meaning un-annotated) naming how
  long the gate's output takes to respond; stored now, honored later
  — this feature makes it user-writable and visible, and nothing
  more.
- **The reserved per-gate field**: the storage slot the module
  reserved at its extension map's timing hook — already present on
  every gate, already persisted in every save format, already covered
  by documented optimizer rules; the annotation verbs are its first
  user-facing writer.
- **Gate identity**: the module's per-kind monotonic never-reused
  counters rendered as display names (AND1, DFF2, TBUF3) — the
  addressing anchor for every annotation verb; dead slots are not
  addressable.
- **The future delay evaluator**: the named follow-on that reads
  annotations to simulate timing; explicitly out of scope here, but
  the reason annotations must be exact, inspectable, persistent, and
  inert in the meantime.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Writability demonstration in one session — a gate
  annotated with a bare-picosecond value and a unit-suffixed value
  (e.g. `1.5ns`) stores the independently derived exact conversion
  (1500ps), the listing shows it on exactly that gate, and every
  refusal class (negative, fractional picosecond, overflow,
  cross-dimension, unknown gate) fails loudly with the workspace
  untouched.
- **SC-002**: Zero behavioral delta — the same netlist with and
  without annotations produces byte-identical solve, step, truth, and
  export outputs; every pre-existing test, golden, saved-workspace
  behavior, and refusal message passes unchanged.
- **SC-003**: Round-trip and lifecycle — an annotated workspace
  reloads with identical annotations and replays byte-identically; a
  pre-feature workspace loads byte-identically; clearing yields a
  gate byte-indistinguishable from never-annotated; gate removal
  leaves no dangling value; in-place optimization preserves the
  annotation and the reshaping level resets it exactly per its
  documented contract.
- **SC-004**: One vocabulary, every surface — the set/clear
  operations accept and refuse identically across the session verbs
  and the engine seam, completion offers the new names and value
  forms, interactive output is byte-identical to headless, and every
  registry-size check, run example, module-table entry, and
  test-list line updates in the same change.

## Assumptions

- The reserved field, its zero default, its persistence in every save
  format, and the optimizer's documented treatment (in-place levels
  preserve; the reshaping level resets) all ship today; this feature
  only adds the user-facing write, clear, and inspect paths — no
  schema generation bump, no struct change, no evaluator change.
- Delay simulation (an evaluator that honors annotations) is
  explicitly out of scope and remains the named follow-on; this
  feature exists so that evaluator has something to read.
- The set verb anchors on the module's already-reserved `set-delay`
  name; the clear path follows the module's paired-verb convention.
  Exact names and argument shapes are planning decisions; the
  contract requires only that they are explicit, loud, completion-
  covered, and inside the existing logic command domain.
- Bare integers mean picoseconds (the field's storage unit);
  suffixed values parse through the engine's single owned
  time-suffix table (ps/ns/us/ms/s/min/h spellings) and must convert
  exactly — no per-verb conversion tables, no rounding.
- Annotation targets any live gate, combinational or sequential;
  what a delay means per kind is the future evaluator's decision, and
  nothing stored here constrains it.
- A creation-time delay option on the placement verb is out of
  scope: place first, annotate after.
- Annotation display lives in the listing surface only; `draw`,
  truth, and export outputs are unchanged by annotations.
- GUI surfaces are out of scope (the logic GUI panel is a separate
  epic); any interactive form that exposes the verbs composes the
  same invocations with byte-identical output per the standing
  parity rule — no new form is required by this feature.
- The zero-value and cleared states are the same state by design;
  there is no separate "annotated zero" concept anywhere in the
  module.
