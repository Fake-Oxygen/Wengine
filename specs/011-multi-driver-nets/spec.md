# Feature Specification: Allowlisted Multi-Driver Nets with Loud Contention Counting

**Feature Branch**: `011-multi-driver-nets`

**Created**: 2026-11-19

**Status**: Draft

**Input**: User description: "What: Allowlisted multi-driver nets with loud contention counting on top of the VAL1 resolver. Why: Buses and wired-OR require N:1 nets; single-driver rule must become opt-in per net, never a silent overwrite."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Opt a net into multi-driver use (Priority: P1)

A user explicitly marks one destination input as a **shared (multi-driver)
net**. Only then may a second wired driver land on it. Before any mark,
today's rule is untouched: wiring a second driver onto an occupied input
is refused loudly with the netlist left exactly as it was — never a
silent overwrite, never a global or implied multi-driver mode. The mark
is per input, explicit, and inspectable; removing it while more than one
driver is still attached is refused loudly (unwire down first), and
extra drivers beyond a small fixed cap are refused loudly, never
truncated.

**Why this priority**: The opt-in gate is the safety property that makes
N:1 nets safe at all. The default must remain the loud single-driver
refusal so that no existing, imported, or accidental flow can ever grow a
second driver without explicit user intent. Every other story in this
feature stands on this gate; a gate that can be bypassed silently is no
gate.

**Independent Test**: Can be fully tested by wiring a second driver onto
an unmarked occupied input and asserting the loud refusal leaves the
netlist byte-identical to today; marking the input and wiring the same
second driver then succeeds; unmarking with two drivers attached fails
loudly; unmarking after unwiring down to one succeeds — delivering the
authoring gate with no resolution or counting story involved.

**Acceptance Scenarios**:

1. **Given** an occupied input without the mark, **When** a second
   driver is wired onto it, **Then** the wiring fails loudly and the
   netlist is unchanged — the same refusal, with the same message shape,
   as today.
2. **Given** a marked input, **When** additional drivers are wired onto
   it, **Then** each attachment is accepted and recorded, up to the
   fixed cap.
3. **Given** a marked input with two or more drivers attached, **When**
   the user removes the mark, **Then** removal fails loudly; after
   unwiring down to at most one driver, removal succeeds.
4. **Given** a marked input already holding the capped number of
   drivers, **When** another driver is wired, **Then** the wiring fails
   loudly — the driver set is never silently truncated or grown past
   the cap.

---

### User Story 2 - Resolve N:1 nets through the shipped funnel (Priority: P1)

With several drivers attached, a shared net evaluates through the same
single four-value resolution funnel that tri-state resolution shipped
(the VAL1 contract): every attached driver contributes its present value
(an inverted/NQ attachment contributes the inverted value), and the
funnel folds the span — agreement resolves to the agreed level (the
wired-OR/wired-AND case), low-vs-high disagreement resolves to unknown
(X), and an empty span resolves to high-Z. Downstream consumers behave
exactly as the funnel contract already defines: definite levels flow;
X/Z inputs fail loudly with simulation state and step timing untouched.
No second resolver, no new value semantics, no strength model.

**Why this priority**: This is the entire expressive point of the
feature — buses and wired-OR require N:1 nets. Without resolution over
the full attached-driver span, the allowlist would only admit
configurations that then misbehave; with it, every multi-driver
construct resolves by the already-published, already-tested table.

**Independent Test**: Can be fully tested by evaluating spans of
attached drivers and asserting the shipped reference table: `{low, low}`
→ low, `{low, low, low}` → low, `{low, high}` → X, one driver → its
level, zero drivers → high-Z, and a Q+NQ attachment pair of the same
source → X — delivering wired consensus and conflict detection with no
counting story involved.

**Acceptance Scenarios**:

1. **Given** a shared net whose attached drivers all present the same
   level, **When** the net evaluates, **Then** it resolves to that level
   (consensus; high-Z transparency and all other combinations follow the
   shipped table unchanged).
2. **Given** a shared net whose attached drivers present both low and
   high, **When** the net evaluates, **Then** it resolves to kUnknown,
   and a downstream consumer that requires a definite level rejects it
   loudly with state, step count, and timing untouched.
3. **Given** a shared net with an inverted (NQ-sourced) attachment,
   **When** the net evaluates, **Then** that driver contributes its
   inverted value to the span — a Q and an NQ attachment of the same
   source contend and resolve to X.
4. **Given** a shared net whose drivers have all been unwired, **When**
   the net evaluates, **Then** it resolves to kHighZ, identical to the
   empty-span contract.
5. **Given** any input with exactly one attached driver (marked or
   not), **When** the netlist evaluates, **Then** the resolved value and
   every output are bit-identical to the pre-feature single-driver
   behavior.

---

### User Story 3 - Contend loudly: counted, visible, diagnostic-only (Priority: P1)

Every resolution of a shared net whose active drivers present both low
and high (a contended resolution) increments a **per-net contention
count**. The count is visible in the module's evaluation/solve status
and readback surface — a user never has to watch individual net values
to learn that a bus fought. Counts are diagnostic session state in the
engine's established loud-counter convention: they never alter
simulation state, timing, or output values; they are cleared by session
reset; they are never serialized into workspaces or checkpoints.
Identical runs produce identical value streams and identical count
sequences.

**Why this priority**: Loudness is half the feature contract, co-named
in the feature description. A contended net that feeds no level
consumer would otherwise be silently X; the counter makes every
contention event visible in health/readback exactly the way the engine
already treats drops and lag — counted, never silent. Detection without
visibility would reduce a fault class to a coincidence the user has to
catch in the act.

**Independent Test**: Can be fully tested by driving a shared bus into
disagreement a known number of times and asserting the readback count
is exactly that number after exactly that many contended resolutions,
zero after agreeing ones, cleared by session reset, with all serialized
output byte-identical to a contention-free session — delivering loud
contention accounting with no authoring-gate story involved.

**Acceptance Scenarios**:

1. **Given** a shared net that contends on a given evaluation, **When**
   the evaluation completes, **Then** that net's contention count has
   incremented by exactly one and the resolved value is kUnknown.
2. **Given** a shared net whose drivers agree (or whose span contains no
   low-vs-high conflict), **When** the net evaluates, **Then** the
   contention count is unchanged.
3. **Given** a session with nonzero contention counts, **When** the user
   inspects the evaluation status/readback, **Then** the per-net counts
   are visible without watching individual net values.
4. **Given** two identical runs (same netlist, inputs, and order),
   **When** their outputs are compared, **Then** value streams and count
   sequences are identical.
5. **Given** a session with contention history, **When** the workspace
   is saved (or simulation state serialized), **Then** the artifact is
   byte-identical to the save from the same session without contention
   — counts are never serialized, and session reset clears them.

---

### User Story 4 - Save, reload, and keep every v1 flow bit-identical (Priority: P2)

A user saves a workspace containing marked nets and multiple attached
drivers, and reloads it: the marks and every attachment come back
identically and replay identically. The workspace schema evolves under
the module's established loud convention — no file is ever silently
reinterpreted, and the prior-generation policy (refuse without loader,
or carry forward) is made explicitly and loudly at the bump, exactly as
the module has done at every past bump. Every flow that never marks a
net — every golden, every saved file, every refusal message — behaves
exactly as before the feature.

**Why this priority**: Persistence is what makes the new constructs real
designs rather than session curiosities, and v1 preservation is the
standing constraint that keeps every existing golden valid. It ranks
below the P1 stories only because it is enablement and constraint
rather than new expressive capability.

**Independent Test**: Can be fully tested by building a marked
two-driver netlist, saving, reloading, and asserting identical marks,
attachments, and replay outputs; plus running the pre-existing logic
suite and asserting every test, golden, and saved-workspace refusal
passes unchanged — delivering persistence and regression safety with no
counting story involved.

**Acceptance Scenarios**:

1. **Given** a saved workspace with marked nets and multiple drivers per
   marked net, **When** it is loaded, **Then** marks, attachments, and
   all evaluation outputs are identical to the pre-save session.
2. **Given** the workspace schema bump, **When** a file from a prior
   generation is loaded, **Then** the module's loud convention applies
   — carried or refused by explicit recorded decision, never silently
   reinterpreted.
3. **Given** any netlist, workspace, golden, or session flow that never
   marks an input, **When** it is loaded and run after this feature,
   **Then** every output is byte-identical to before the feature.

---

### User Story 5 - One vocabulary across every driving surface (Priority: P2)

The new capability is first-class module vocabulary: the CLI module's
verbs cover marking, unmarking, and shared wiring with exactly the
engine seam's validation; completion covers the new names and option
forms; the interactive terminal forms compose the same invocations with
byte-identical output by construction. The optimizer stays sound in the
presence of shared nets: no pass observably changes a shared net's
resolution results, passes that cannot prove safety leave shared nets
untouched, and passes remain pure, idempotent, and loud on overflow.

**Why this priority**: The module's standing contracts — one command per
solver domain, interactive/headless byte parity, optimizer soundness —
are what keep a feature from forking behavior per front end. They are
required for coherence but add no new expressive capability, hence P2.

**Independent Test**: Can be fully tested by driving the same
mark/wire/unmark operations through the session verbs and asserting the
same acceptance/refusals as the engine seam, through the interactive
forms and asserting byte-identical output to headless, and by running
every optimizer pass over a shared-net netlist and asserting
resolution results never observably change and passes stay idempotent —
delivering front-end and tooling coherence with no resolution story
involved.

**Acceptance Scenarios**:

1. **Given** the module vocabulary, **When** the user marks, unmarks,
   and wires shared nets through the session verbs, **Then** validation
   matches the engine seam exactly — same refusals, same acceptance,
   including the cap and unmark-lifecycle refusals.
2. **Given** the interactive forms, **When** they compose the same
   invocations, **Then** output is byte-identical to headless by
   construction, pinned by the full-registry parity tests.
3. **Given** completion over the new vocabulary, **When** option names
   and `--opt=` value forms are requested, **Then** the new names and
   forms complete.
4. **Given** any optimizer pass, **When** run on a netlist containing
   shared nets, **Then** observable resolution results never change; a
   pass that cannot prove safety for a shared net leaves it untouched;
   every pass stays idempotent and loud on overflow.
5. **Given** the registry and documentation duties, **When** the feature
   lands, **Then** every registry-size check, README run example,
   module table entry, and test list line updates in the same change.

---

### Edge Cases

- What happens when **a second driver lands on an unmarked occupied
  input**? → loud refusal, netlist untouched — today's behavior
  byte-identical.
- What happens when **the mark is requested on a tied input**? → ties
  never join a shared net's driver span: the tie rule (untie first)
  is unchanged; the mark governs wired drivers only.
- What happens when **the mark is set with zero drivers attached**? →
  legal: a shared net holds 0..cap drivers and resolves kHighZ while
  empty.
- What happens when **the same source drives the net through both Q and
  NQ**? → the span presents `{low, high}`: the net fights itself,
  resolves X, and the contention count increments.
- What happens when **the same source pin is attached twice through the
  same polarity**? → no special case: it resolves as ordinary agreeing
  drivers; an authoring-level duplicate refusal is a planning choice,
  not a contract change.
- What happens when **a driver gate is removed (or its wire unwired)
  while other drivers remain**? → exactly that attachment is released,
  no dangling entry survives, and the remaining drivers resolve
  unchanged.
- What happens when **a contended shared net feeds no level consumer**?
  → still loud: the resolution is X, the count increments, and the
  event is visible in readback — loudness never depends on a consumer
  being present.
- What happens when **a contended shared net feeds a level consumer**?
  → the count increments first (detection happens at the net), then the
  consumer rejects X loudly with state/step/timing untouched.
- What happens when **drivers are attached/unwired in different orders**?
  → identical evaluation and identical counts: resolution and counting
  depend only on the driver-value multiset, never enumeration order.
- What happens with **an unknown- or high-Z-presenting driver**? →
  today's gate kinds present only low/high; when a future tri-state
  kind lands (the module's named follow-on slot), its contributions
  flow through the same funnel and the counting semantics ride along
  with that feature.
- What happens to **the counts on save/load/reset**? → counts are
  session diagnostics: never serialized, cleared by session reset,
  identical across identical replays.
- What happens to **cycle and validity checks for wires onto shared
  nets**? → unchanged: every added wire passes the same dead-slot,
  input-range, tie-conflict, and flop-aware cycle checks; marking never
  bypasses validation.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: A destination input MUST accept at most one wired driver
  unless it has been explicitly allowlisted as a shared net; the
  occupied-input refusal MUST remain loud with the netlist untouched on
  failure — never a silent overwrite, never a global or implied
  multi-driver mode.
- **FR-002**: Users MUST be able to explicitly mark a destination input
  as a shared net and to remove that mark; the mark MUST be per input
  and inspectable, and removing the mark while more than one driver is
  attached MUST fail loudly.
- **FR-003**: The number of drivers on one shared net MUST be bounded
  by a small fixed cap; attachment past the cap MUST be refused loudly,
  never silently truncated.
- **FR-004**: A shared net MUST legally hold anywhere from zero to the
  cap drivers across its lifetime (attach and unwire freely within the
  cap).
- **FR-005**: Evaluation of a shared net MUST present every attached
  driver's present value — with per-attachment inversion applied for
  inverted (NQ) attachments before resolution — to the same single
  four-value resolution funnel shipped for tri-state resolution: active
  agreement resolves to the level, low-vs-high disagreement resolves to
  kUnknown, and no active drivers resolve to kHighZ. No second resolver
  and no new value semantics may be introduced.
- **FR-006**: An input with exactly one attached driver (marked or not)
  MUST resolve bit-identically to the pre-feature single-driver
  behavior, and unmarked inputs MUST keep today's evaluation path with
  no measurable added cost (benchmark-verified, not assumed).
- **FR-007**: Each resolution of a shared net whose active drivers
  present both low and high MUST increment that net's contention count
  by exactly one; non-contending resolutions MUST NOT increment it.
- **FR-008**: Per-net contention counts MUST be visible in the
  evaluation/solve status and readback surface, without requiring the
  user to watch individual net values.
- **FR-009**: Contention counts MUST be diagnostic session state only:
  they MUST NOT alter simulation state, timing, or output values; they
  MUST be cleared by session reset; they MUST NOT be serialized into
  workspaces or checkpoints.
- **FR-010**: Identical inputs MUST produce identical value streams and
  identical count sequences; counts MUST depend only on resolution
  outcomes, never on driver enumeration order.
- **FR-011**: Downstream consumers MUST keep the shipped contract:
  definite levels flow; kHighZ/kUnknown inputs fail loudly with
  simulation state and timing untouched; the contention count MUST
  increment before any such consumer refusal.
- **FR-012**: Every wire added to a shared net MUST pass the same
  validation as any other wire (dead slots, input range, tie conflicts,
  flop-aware cycle check); marking MUST NOT bypass any check, and ties
  MUST NOT join a shared net's driver span.
- **FR-013**: Removing a gate or wire MUST release exactly that
  attachment from the shared net — no dangling driver entries may
  survive, and remaining drivers MUST resolve unchanged.
- **FR-014**: Marks and attachments MUST round-trip through the
  versioned workspace identically (save → load → replay byte-identical);
  the schema bump MUST follow the module's loud explicit policy for
  prior generations, and no file may ever be silently reinterpreted.
- **FR-015**: Every flow not using shared nets MUST behave
  byte-identically to before the feature — goldens, saved files,
  refusal messages, and session outputs.
- **FR-016**: The new vocabulary MUST live inside the module's existing
  command domain (no new top-level command) with completion coverage
  for names and option forms, and interactive output MUST be
  byte-identical to headless by construction.
- **FR-017**: Optimizer passes MUST remain sound, idempotent, and loud
  on netlists containing shared nets: observable resolution results MUST
  NOT change, and a pass that cannot prove safety for a shared net MUST
  leave it untouched.
- **FR-018**: Multi-driver resolution MUST be verified against the
  independently derived four-value reference table (the tri-state
  resolution contract), never against the implementation's own output.

### Key Entities *(include if feature involves data)*

- **Shared-net mark (allowlist entry)**: the per-input opt-in that lifts
  the single-driver rule for exactly that input; persistent topology
  state, visible in readback, removable only when at most one driver
  remains attached.
- **Shared net**: a destination input's convergence point holding zero to
  cap wired attachments.
- **Driver attachment**: one edge onto a shared net, carrying its source
  and polarity (Q or NQ); the unit of the resolution span.
- **Contention count**: the per-net diagnostic counter of contended
  resolutions; session state, never serialized, visible in
  evaluation/solve status.
- **The resolution funnel**: the shipped single four-value fold
  (high-Z transparent, unknown dominant, conflict → X); unchanged and
  sole — every shared net resolves through it.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Exhaustive multiset verification — every driver-value
  multiset up to the cap, including inverted attachments, resolves
  exactly per the independent four-value reference table (all-agree →
  level, any low+high mix → X, empty → Z), and all distinct attachment
  orders of the same span produce identical results and identical
  counts.
- **SC-002**: Zero regressions — every pre-existing test, golden, and
  saved-workspace behavior passes unchanged; the unmarked occupied-input
  refusal is byte-identical; and single-driver evaluation keeps its
  existing path with no measurable cost added (benchmark before/after).
- **SC-003**: Loudness demonstration in one workspace — an unmarked
  second wire is refused loudly; a marked bus with agreeing drivers
  resolves the level; the same bus with a conflicting driver resolves
  X and the visible per-net count increments; a level consumer fed that
  X fails loudly with timing untouched: four distinguishable
  user-visible outcomes from one construct.
- **SC-004**: Diagnostic proof — identical runs produce identical count
  sequences; counts reset with session reset and never appear in saved
  artifacts (saves from contention-free and contention-laden otherwise
  identical sessions are byte-identical).
- **SC-005**: Round-trip — a saved shared-net workspace reloads with
  identical marks and attachments and replays byte-identical outputs;
  the prior-generation policy at the schema bump is exercised loudly
  (carried or refused) exactly as recorded.

## Assumptions

- "The VAL1 resolver" is the shipped four-value resolution funnel from
  the tri-state resolution feature (`specs/010-tristate-net-resolution`,
  task LOGIC-V2-VAL1/WENGI-15's sibling): high-Z transparent, unknown
  dominant, low-vs-high → X, order-independent. This feature adds
  netlist authoring and contention observability **on top of** it; no
  resolution semantics change and no strength model lands (wired-OR /
  wired-AND intent is consensus; conflict is X).
- The driver cap is a small fixed bound chosen at planning (in the
  spirit of the module's existing caps), applied loudly; it is not
  silently growable and not per-net configurable.
- The contention counter counts low-vs-high contended resolutions.
  Unknown- or high-Z-presenting drivers cannot arise from today's gate
  kinds; the tri-state buffer kind is the named follow-on (VAL3) and
  brings its own counting refinements with it.
- The exact authoring shape (a wiring option vs. a separate mark verb)
  is a planning decision; the contract requires only that the opt-in is
  explicit, per input, loud on refusal, covered by completion, and
  byte-identical across the interactive and headless front ends.
- Counts are session diagnostics in the module's established
  convention (like cycle counters and poke maps: cleared by reset, never
  serialized); the evaluation/solve status and readback surfaces show
  them.
- Duplicate attachment of the same source pin carries no special
  semantics; refusing duplicates would be an authoring nicety chosen at
  planning, not a contract.
- Ties and pokes never join a shared net's driver span: the untie-first
  rule and dangling-input-only poke rule are preserved unchanged.
- The workspace schema bump follows the module's loud no-silent-
  reinterpretation convention; whether prior generations gain a loader
  or are refused is decided explicitly and loudly at the bump (the
  module's standing decision is refusal without fallback loaders).
- Bus primitives, importers, tri-state/pull gate kinds, strength and
  delay models stay out of scope — each is a named follow-on slot in
  the module's extension map that this feature unblocks.
- Zero-delay evaluation remains in force; stored propagation-delay
  fields stay uninterpreted.
- The four-value vocabulary and every gate kind byte are append-only
  and unchanged by this feature; no new kind lands here.
