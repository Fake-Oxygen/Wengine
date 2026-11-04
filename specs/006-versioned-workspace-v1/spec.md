# Feature Specification: Versioned Workspace v1 (Flop Initial States + Clock-Domain Metadata)

**Feature Branch**: `006-versioned-workspace-v1`

**Created**: 2026-11-04

**Status**: Draft

**Input**: User description: "Versioned workspace v1 carrying flop initial states and clock-domain metadata. Sequential workspaces must persist and stay openable. Legacy versions v1/v2 should be dropped completely."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Save and reopen a sequential workspace identically (Priority: P1)

A hardware-logic user builds a sequential workspace (combinational gates
plus edge-triggered flops and/or level-sensitive latches, wired and tied,
with named clock domains and per-flop initial states), saves it to a file,
and later reopens that file. The reopened workspace is identical in every
observable respect: gate topology, wire and tie structure, flop initial
states, and clock-domain metadata. Saving the reopened workspace produces a
byte-identical file.

**Why this priority**: This is the core promise — sequential work must
persist and stay openable. Without a round-trip that preserves inits and
domain metadata, every save/load loses design intent (power-on values,
domain names) and forces users to reconstruct it by hand.

**Independent Test**: Can be fully tested by building a small sequential
net (e.g. a DFF counter with mixed initial values across two clock
domains), saving it, reloading it into a fresh session, and confirming
identical readback plus a byte-identical re-save — delivering lossless
persistence with no stepping or legacy-file involvement.

**Acceptance Scenarios**:

1. **Given** a sequential workspace with at least one edge-triggered flop
   carrying a non-default initial state and at least one named clock
   domain, **When** the user saves it and reloads the file into a fresh
   session, **Then** topology, initial states, and domain metadata all
   read back identical to the saved workspace.
2. **Given** a just-reloaded workspace, **When** the user saves it again
   without modification, **Then** the second file is byte-identical to
   the first.
3. **Given** a workspace file with comments, quoted strings, or block vs
   flow style differences that carry the same content, **When** the user
   loads it, **Then** it opens successfully and re-saving yields the
   canonical byte layout.

---

### User Story 2 - Reopened initial states take effect on power-on (Priority: P1)

A user reopens a workspace whose flops and latches carry stored initial
states and observes those values immediately — before any clock advance —
in the normal state readback. Subsequent stepping evolves deterministically
from those starting values, and repeating the load-plus-run sequence always
produces the same trajectory.

**Why this priority**: Persisted inits are worthless if reopening ignores
them and everything still powers on zeroed. Power-on fidelity is what turns
"saved bits" into "restored design" (counters resume mid-sequence, FSMs
restart in their saved state).

**Independent Test**: Can be fully tested by saving a counter workspace
with a nonzero start pattern, reloading it, reading back state before any
step, advancing a fixed number of steps, and confirming the observed
sequence matches the same run performed before saving — delivering
power-on fidelity with no domain-selection or legacy handling involved.

**Acceptance Scenarios**:

1. **Given** a reloaded workspace whose flops carry stored initial values,
   **When** the user reads back flop/latch state before any advance,
   **Then** every stored value matches its persisted initial state
   (including non-default High values, not all-zero).
2. **Given** the reloaded workspace run for N fixed steps, **When**
   compared against the identical pre-save workspace run for N steps
   from the same starting point, **Then** both runs report identical
   state sequences step for step.
3. **Given** a workspace where a sequential element carries no explicit
   initial value, **When** it is loaded, **Then** that element powers on
   at the documented default (Low), matching today's fresh-attach
   behavior.

---

### User Story 3 - Named clock domains survive the round-trip (Priority: P1)

A user whose net has one or more clock domains (including the implicit
domain formed by dangling clock pins) saves the workspace and reopens it.
Domain names are intact, advancing by domain name behaves exactly as before
the save, and advancing all domains behaves exactly as before the save.

**Why this priority**: Multi-clock designs are addressed by domain name
(step one domain, step all). If names or domain bindings are lost or
scrambled by persistence, selective stepping after reload silently drives
the wrong flops — a correctness hazard, not just an inconvenience.

**Independent Test**: Can be fully tested by building a two-clock net,
saving it, reloading it, advancing N steps selecting one domain by name,
and confirming the selected domain's flops advanced N times while the
other domain's flops held — delivering domain fidelity with no
initial-state or legacy-file involvement.

**Acceptance Scenarios**:

1. **Given** a reloaded two-clock workspace, **When** the user advances N
   steps selecting one domain by name, **Then** that domain's flops
   reflect N captures while the other domain's flops hold their values —
   identical to the pre-save behavior.
2. **Given** a reloaded workspace, **When** the user advances N steps
   across all domains, **Then** every domain's flops reflect N captures
   each, identical to the pre-save behavior.
3. **Given** a workspace file whose domain metadata names a clock source
   that does not resolve in the stored topology (or binds two names to
   the same source inconsistently), **When** the user attempts to load
   it, **Then** the load is refused loudly naming the offending domain
   and the current workspace is left unchanged.

---

### User Story 4 - Legacy v1/v2 files are refused loudly, never half-loaded (Priority: P2)

A user (or script) presents a workspace file written in either legacy
format generation and receives a clear refusal that names the found
version. Nothing is partially loaded, the current workspace is untouched,
and no silent migration or fallback interpretation is attempted on any
entry path (interactive session, one-shot run, or scripted load).

**Why this priority**: Dropping legacy support is an explicit breaking
change. A loud, version-naming refusal is what makes the break safe and
debuggable; a silent reinterpretation (or a crash, or a half-loaded net)
would corrupt designs while looking successful.

**Independent Test**: Can be fully tested by attempting to load one
legacy-generation sample file of each old kind and confirming each attempt
fails with a version-naming diagnostic and the session workspace reads
back exactly as before the attempt — delivering clean refusal with no
persistence or stepping involvement.

**Acceptance Scenarios**:

1. **Given** a legacy-generation workspace file, **When** the user
   attempts to load it, **Then** the attempt fails loudly with a
   diagnostic naming the found version, and the current workspace reads
   back exactly as before the attempt.
2. **Given** a file claiming the new version marker but missing the new
   required sections (indistinguishable-bytes legacy content), **When**
   the user attempts to load it, **Then** it is refused as a legacy file
   rather than misloaded under the new schema.
3. **Given** a failed load of any kind (legacy, unknown version, corrupt
   content), **When** the failure is reported, **Then** no gate, wire,
   tie, init, or domain metadata from the file is present in the session.

---

### User Story 5 - Example library fully converted, plus three advanced references (Priority: P2)

A new user or test author opens the shipped example library and finds
every file — the five existing combinational examples and three new
advanced sequential ones — loadable in the new format with zero legacy
relics. Each of the three new examples is an advanced sequential design
that exercises the new persistence surface, and each carries header
documentation stating its run command and expected output, following the
established examples convention. The three advanced designs are: a
two-clock-domain counter with mixed initial states (inits plus selective
stepping by domain name); an enable-gated counter built from
full-control-set flops with asynchronous reset/set (control persistence
and async forces after reload); and a mixed-kind sequential net combining
JK/T divider stages with level-sensitive latch logic (all sequential
kinds, latch settle, and level behavior after reload).

**Why this priority**: Examples are the executable documentation and the
future test/reference corpus. If any shipped file stays legacy it becomes
an unloadable trap; without advanced sequential references, the new
inits-plus-domains surface has no canonical demonstration and future
testing must reinvent fixtures from scratch.

**Independent Test**: Can be fully tested by loading each of the eight
shipped files (five converted plus three new) into a fresh session,
running its documented command, and confirming the documented expected
output — delivering a verified reference corpus with no stepping-semantics
or refusal-logic involvement.

**Acceptance Scenarios**:

1. **Given** each shipped example file (five converted, three new),
   **When** the user loads it into a fresh session and runs its
   documented command, **Then** it loads without error and produces the
   documented expected output.
2. **Given** any of the three advanced examples after loading, **When**
   the user saves the workspace again, **Then** the re-save is in the
   new schema and a further load/save cycle is byte-identical.
3. **Given** the shipped example set, **When** inspected as files,
   **Then** no file carries a legacy generation marker and every
   sequential file carries initial states and clock-domain metadata.

---

### Edge Cases

- What happens when a file claims the new version but a gate row carries
  an initial value on a combinational gate? It is a loud load error naming
  the gate — inits belong to sequential elements only.
- What happens when an initial value is non-binary? It is a loud load
  error naming the gate — same strictness as stimulus validation.
- What happens when two clock-domain entries share a name, or a domain
  entry has no resolvable clock source? Loud load error naming the
  domain; nothing loads.
- What happens when a domain entry's source resolves but covers no live
  edge flop? Loud load error — empty domains are rejected rather than
  silently meaning "step nothing".
- What happens when a file carries an unknown top-level section or an
  unknown key inside a known section? Loud error naming the key — the
  existing strictness (never silently tolerate) extends to the new
  sections unchanged.
- What happens when a future (higher-than-current) version is presented?
  Loud refusal naming the found version, same as today — dropping legacy
  versions does not open the door to forward versions.
- What happens to an empty workspace (no gates)? It saves and reloads
  with the new version marker and empty sections, consistent with today.
- What happens to interactive stepping, stimulus drives, and run
  determinism? Unchanged — this feature moves state and names into the
  file; it redefines no evaluation, priority, or stepping semantics.
- What happens to the shipped advanced examples if a future change breaks
  one? They are pinned by load/run/expected-output checks like the
  existing examples, so breakage fails loudly instead of rotting silently.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The system MUST save sequential workspaces in the new
  versioned schema carrying, in canonical deterministic bytes: gate
  topology, wire structure, tie structure, one binary initial state per
  sequential element, and named clock-domain metadata.
- **FR-002**: The system MUST load only files conforming to the new
  schema (new version marker plus the new required sections) and MUST
  restore topology, initial states, and domain metadata together, or fail
  with the workspace untouched.
- **FR-003**: The system MUST refuse legacy-generation files (both legacy
  kinds) loudly on every load path, with a diagnostic naming the found
  version; there MUST be no fallback loader and no silent migration.
- **FR-004**: A file carrying the new version marker but lacking the new
  required sections MUST be refused as a legacy file, never misloaded
  under the new schema.
- **FR-005**: Every sequential element (edge-triggered flop or
  level-sensitive latch) MUST be able to carry a persisted binary initial
  state; an element with no explicit initial value MUST power on at the
  documented default (Low).
- **FR-006**: Initial values on combinational gates, and non-binary
  initial values anywhere, MUST be loud load errors naming the offending
  entry.
- **FR-007**: Clock-domain metadata MUST give every derived clock domain
  (including the implicit domain of dangling clock pins) a stable name
  bound to its clock source; domain membership MUST remain derived from
  the topology (single source of truth), and any metadata that does not
  resolve against the stored topology MUST be a loud load error.
- **FR-008**: After loading, fresh power-on state MUST reflect the
  persisted initial values; stepping, domain selection, and state
  readback MUST behave identically to the pre-save workspace.
- **FR-009**: Save/load/save MUST be byte-identical; loading MUST accept
  the YAML content superset (comments, quoting, block vs flow style)
  while saving always emits the canonical byte layout.
- **FR-010**: Existing strictness MUST be preserved and extended to the
  new sections: unknown keys/sections are loud errors, and all existing
  topology validation (known gates/pins, single-driver, flop-aware cycle
  rules) applies unchanged.
- **FR-011**: Loading MUST be atomic at the workspace level: any failure
  (legacy version, unknown version, corrupt content, topology error,
  init/domain error) leaves the current workspace exactly as before the
  attempt.
- **FR-012**: All existing user-visible behavior except version acceptance
  MUST be unchanged: session verbs and their spelling, stepping and
  stimulus semantics, determinism and repeatability, and recorded-output
  formats.
- **FR-013**: Three advanced sequential reference workspaces MUST ship as
  YAML files in the new schema: (a) a two-clock-domain counter with mixed
  initial states, (b) an enable-gated counter from full-control-set flops
  with asynchronous reset/set, (c) a mixed-kind net combining JK/T divider
  stages with level-sensitive latch logic. Each MUST load, MUST carry
  initial states and clock-domain metadata, and MUST document its run
  command and expected output in header comments per the established
  examples convention.
- **FR-014**: Every pre-existing shipped example file MUST be reworked to
  the new schema; no shipped file may remain in a legacy generation.
- **FR-015**: No trace of legacy version support may remain: no fallback
  loader on any load path, no legacy test fixtures or expectations, no
  user-documentation references to loading legacy files. The only
  legacy-related behavior is the loud version-naming refusal (FR-003).

### Key Entities *(include if feature involves data)*

- **Versioned Workspace File**: The persisted artifact carrying gate
  topology, wires, ties, per-element initial states, and clock-domain
  metadata under a single version marker; the unit saved and opened.
- **Flop Initial State**: The binary power-on value stored per sequential
  element (edge flop or level latch); applied at fresh attach after load,
  defaulting to Low when not explicitly stored.
- **Named Clock Domain**: A clock domain (the set of edge-triggered flops
  sharing one clock source, with dangling clocks forming the implicit
  domain) together with its persisted stable name and source binding; the
  unit of selective stepping.
- **Legacy File**: Any workspace file of either retired generation; never
  loadable, always refused with a version-naming diagnostic.
- **Reference Example**: A shipped YAML workspace file with documented run
  command and expected output, serving as executable documentation and
  future test/reference corpus.
- **Canonical Byte Layout**: The single deterministic file encoding
  emitted by save, such that save/load/save is byte-identical.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: A user can save a two-clock counter workspace with mixed
  initial values, reopen it in a fresh session, and observe the stored
  initial values in the first state readback on the first attempt.
- **SC-002**: Ten consecutive save/load/save cycles on a sequential
  workspace with inits and named domains produce byte-identical files on
  all ten cycles.
- **SC-003**: A user can reload a two-clock workspace, advance 3 steps
  selecting one domain by name, and observe the selected domain advanced
  3 times while the unselected domain held, on the first attempt.
- **SC-004**: Attempting to open one sample file of each legacy kind
  fails with a diagnostic naming the found version, with the session
  workspace verified unchanged afterwards, on the first attempt for each.
- **SC-005**: Two identical fixed-step runs started from the same
  reloaded workspace produce identical state readbacks on every
  repetition.
- **SC-006**: Each of the eight shipped example files (five converted,
  three new) loads in a fresh session and produces its documented
  expected output from its documented run command on the first attempt.
- **SC-007**: A repository-wide inspection finds no fallback loader, no
  legacy test fixture or expectation, no shipped legacy file, and no
  user-documentation reference to loading legacy files — only the
  version-naming refusal.

## Assumptions

- "v1" in the request is read literally: the new schema generation is
  version 1 of the redefined workspace format, and both legacy
  generations ("v1/v2" of the old line) are retired. Because the retired
  first generation shares the numeral `1`, acceptance is defined by
  marker **plus** the new required sections (a `version: 1` file without
  them is refused as legacy), so legacy bytes can never misload as new.
- No migration path or conversion tooling is in scope: legacy files are
  refused, not upgraded. Users re-author or re-save from a session that
  still holds the design. The requester attests legacy versions will never
  be loaded again, so zero fallback scope is required and no
  back-compatibility accommodation may be retained.
- The five currently shipped example files were observed to carry the
  legacy `version: 1` marker; their rework to the new schema (FR-014) is
  therefore required, not optional — otherwise they become unloadable.
- Initial-state persistence covers every element with stored state (edge
  flops and level latches alike); omitted inits default to Low, matching
  today's fresh-attach power-on.
- Clock-domain membership stays derived from the topology (one domain per
  clock source over edge flops; latches belong to no domain); only names
  and source bindings are persisted, so the netlist remains the single
  source of truth.
- File-format scope only: no new simulation semantics, no timing/delay,
  no GUI, no import/export format changes beyond the workspace schema.
- The YAML content superset tolerance (comments, quoting, styles) and the
  strict unknown-key refusal both carry over unchanged to the new schema.
