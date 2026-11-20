# Feature Specification: Enable-Gated Buffer plus Per-Net Pull-Up/Down for Float Resolution

**Feature Branch**: `012-gated-buffer-pulls`

**Created**: 2026-11-20

**Status**: Draft

**Input**: User description: "What: Enable-gated buffer kind plus per-net pull-up/down for float resolution. Why: Usable buses need a driver that can disconnect; pulls give the defined idle level."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Disconnect a driver with an enable-gated buffer (Priority: P1)

A user places a buffer with a data input and an enable input on a bus.
While the enable is asserted, the buffer drives its data level onto the
net like any ordinary driver; while the enable is released, the buffer
presents high-Z (disconnected) and contributes nothing to the net's
resolution — the bus behaves exactly as if that driver were unwired.
Several such buffers (plus ordinary drivers on a shared net) therefore
form a usable bus: at most one enabled at a time drives the level, all
disabled leaves the net floating, two enabled in disagreement contends.
The enable is active-high; an active-low intent is expressed by driving
the enable through an inverter, exactly as any other polarity need is
expressed in the module.

**Why this priority**: A driver that cannot disconnect cannot share a
net — this is the expressive primitive every bus story stands on. The
tri-state resolver (010) defined what Z means and the shared-net
allowlist (011) defined who may co-drive; neither lets one driver step
aside while the others talk. Without the gated buffer, buses remain
unwritable.

**Independent Test**: Can be fully tested by toggling one buffer's
enable and asserting the net tracks the data level while asserted and
resolves as if the driver were absent while released (level from the
remaining drivers, or high-Z when none remain) — delivering
connect/disconnect with no pull story involved.

**Acceptance Scenarios**:

1. **Given** a buffer whose enable is asserted and whose data input
   presents a definite level, **When** the net evaluates, **Then** the
   buffer contributes that level to the resolution span, identical to an
   ordinary driver presenting the same level.
2. **Given** a buffer whose enable is released, **When** the net
   evaluates, **Then** the buffer contributes high-Z — the resolved
   value is identical to the same net with that buffer's attachment
   removed.
3. **Given** a bus with two buffers enabled in disagreement (one
   driving low, one driving high), **When** the net evaluates, **Then**
   it resolves to kUnknown and the per-net contention count increments
   — contention between gated drivers is detected exactly like
   contention between ordinary drivers.
4. **Given** a buffer whose enable input presents kUnknown or kHighZ,
   **When** the buffer evaluates, **Then** evaluation fails loudly with
   simulation state, step count, and timing untouched (control inputs
   require definite levels, the module's standing convention).
5. **Given** a buffer whose data input presents kUnknown or kHighZ
   while the enable is asserted, **When** the buffer evaluates, **Then**
   evaluation fails loudly with state and timing untouched (buffer
   inputs are ordinary gate inputs — Z/X is rejected at the seam, never
   silently driven).

---

### User Story 2 - Define the idle level of a floating net (Priority: P1)

A user attaches a pull — up, down, or none — to a destination input's
net. A net that resolves to all-floating (empty span, or every attached
driver presenting high-Z, including all buffers disabled) then resolves
to the defined idle level: high for pull-up, low for pull-down, and
stays high-Z when the pull is none (today's behavior, the default).
A pull never outvotes anything: a net with any active level driver, or
a net that resolves to unknown through contention, keeps exactly the
value the shipped funnel already gives it — the pull only names what
"nobody is driving" means on that net.

**Why this priority**: Co-named in the feature description and the
other half of a usable bus. A floating CMOS input is not a value; real
designs terminate it with a resistor, and without the pull the user's
only idle state is Z — which every level consumer rejects loudly. The
pull turns "bus parked" from an error into a designed level.

**Independent Test**: Can be fully tested by parking a bus (all drivers
high-Z) under each pull setting and asserting pull-up reads high,
pull-down reads low, and none reads high-Z — while a driven or
contended net under any pull setting resolves exactly as without the
pull — delivering idle-level definition with no buffer story involved.

**Acceptance Scenarios**:

1. **Given** a net with a pull-up whose drivers are all high-Z (or with
   no drivers at all), **When** the net evaluates, **Then** it resolves
   to kHigh.
2. **Given** a net with a pull-down whose drivers are all high-Z (or
   with no drivers at all), **When** the net evaluates, **Then** it
   resolves to kLow.
3. **Given** a net with no pull (the default) whose drivers are all
   high-Z, **When** the net evaluates, **Then** it resolves to kHighZ —
   byte-identical to today's behavior.
4. **Given** a net with any pull setting whose active drivers agree on
   a level, **When** the net evaluates, **Then** it resolves to that
   level — the pull changes nothing on a driven net.
5. **Given** a net with any pull setting whose active drivers contend
   (low vs. high), **When** the net evaluates, **Then** it resolves to
   kUnknown and the contention count increments — a pull never papers
   over a fight.

---

### User Story 3 - One funnel, no second resolver (Priority: P1)

Both mechanisms resolve through the single shipped four-value funnel:
the buffer only decides *what value it contributes* (level or high-Z)
and the pull only decides *what an all-Z span means on that net*
(idle level or Z). No strength model, no priority order, no wired-OR /
wired-AND reinterpretation lands — agreement still means the level,
disagreement still means X, unknown still dominates, order still does
not matter. A single-driver net with no pull evaluates bit-identically
to today on the existing path.

**Why this priority**: The module's hardest-won invariant is that one
funnel resolves every net. A buffer or pull that smuggled in its own
semantics (strengths, priorities, enable-dominance over X) would fork
the value system and invalidate the 010/011 reference tables. This
story pins the composition so the two new mechanisms are pure
authoring on top of settled semantics.

**Independent Test**: Can be fully tested by exhaustively resolving
driver spans mixing gated (enabled/disabled) and ordinary attachments
under every pull setting against the 010/011 reference table plus the
two float-mapping rows (all-Z + up → high, all-Z + down → low) —
delivering funnel unity with no persistence or vocabulary story
involved.

**Acceptance Scenarios**:

1. **Given** any driver span, **When** the span is permuted (attachments
   in any order), **Then** the resolved value is identical — order
   independence survives buffers and pulls.
2. **Given** a span containing a kUnknown driver (however produced),
   **When** the net evaluates under any pull setting, **Then** the
   result is kUnknown — unknown still dominates levels and pulls alike.
3. **Given** an input with exactly one ordinary driver and no pull,
   **When** the netlist evaluates, **Then** every value and output is
   bit-identical to the pre-feature behavior, on the pre-feature path
   with no measurable added cost.

---

### User Story 4 - Save, reload, and keep every existing flow bit-identical (Priority: P2)

A user saves a workspace containing gated buffers and pull settings
and reloads it: kinds, enables, data wirings, marks, attachments, and
pulls come back identically and replay identically. The workspace
schema evolves under the module's established loud convention — no
file is ever silently reinterpreted, and the prior-generation policy
(refuse without loader, or carry forward) is made explicitly and loudly
at the bump. Every flow that uses no buffer and no pull — every
golden, every saved file, every refusal message — behaves exactly as
before the feature.

**Why this priority**: Persistence is what makes the new constructs
real designs, and preservation is the standing constraint that keeps
every existing golden valid. Ranks below the P1 stories only because
it is enablement and constraint rather than new expressive capability.

**Independent Test**: Can be fully tested by building a pulled bus with
two gated buffers, saving, reloading, and asserting identical topology,
pulls, and replay outputs; plus running the pre-existing logic suite
and asserting every test, golden, and saved-workspace refusal passes
unchanged.

**Acceptance Scenarios**:

1. **Given** a saved workspace with gated buffers and pull settings,
   **When** it is loaded, **Then** kinds, wirings, marks, attachments,
   and pulls are identical to the pre-save session and replay
   byte-identically.
2. **Given** the workspace schema bump, **When** a file from a prior
   generation is loaded, **Then** the module's loud convention applies
   — carried or refused by explicit recorded decision, never silently
   reinterpreted.
3. **Given** any netlist, workspace, golden, or session flow using no
   buffer and no pull, **When** it is loaded and run after this
   feature, **Then** every output is byte-identical to before.

---

### User Story 5 - One vocabulary across every driving surface (Priority: P2)

The new capability is first-class module vocabulary: the CLI module's
verbs cover placing gated buffers, wiring their data/enable inputs,
and setting/clearing pulls with exactly the engine seam's validation;
completion covers the new names and option forms; the interactive
terminal forms compose the same invocations with byte-identical output
by construction. The optimizer stays sound: no pass observably changes
a buffer's enable-gating or a net's pulled resolution, passes that
cannot prove safety leave them untouched, and passes remain pure,
idempotent, and loud on overflow. Sequential gates stay opaque barriers
as before.

**Why this priority**: The module's standing contracts — one command
per solver domain, interactive/headless byte parity, optimizer
soundness — keep a feature from forking behavior per front end. They
are required for coherence but add no new expressive capability, hence
P2.

**Independent Test**: Can be fully tested by driving the same
place/wire/pull operations through the session verbs and asserting the
same acceptance/refusals as the engine seam, through the interactive
forms and asserting byte-identical output to headless, and by running
every optimizer pass over a buffer-and-pull netlist and asserting
resolution results never observably change and passes stay idempotent.

**Acceptance Scenarios**:

1. **Given** the module vocabulary, **When** the user places buffers and
   sets/clears pulls through the session verbs, **Then** validation
   matches the engine seam exactly — same refusals, same acceptance.
2. **Given** the interactive forms, **When** they compose the same
   invocations, **Then** output is byte-identical to headless by
   construction, pinned by the full-registry parity tests.
3. **Given** completion over the new vocabulary, **When** option names
   and `--opt=` value forms are requested, **Then** the new names and
   forms complete.
4. **Given** any optimizer pass, **When** run on a netlist containing
   gated buffers or pulls, **Then** observable resolution results never
   change; a pass that cannot prove safety leaves them untouched; every
   pass stays idempotent and loud on overflow.
5. **Given** the registry and documentation duties, **When** the feature
   lands, **Then** every registry-size check, README run example,
   module table entry, and test list line updates in the same change.

---

### Edge Cases

- What happens when **the enable is asserted but the data input is
  unwired (dangling)**? → loud failure at the buffer's data seam with
  state/timing untouched (dangling-input rule unchanged; a buffer never
  invents data).
- What happens when **the enable input is unwired (dangling)**? →
  loud failure at the enable seam — an undriven enable never defaults
  to on or off.
- What happens when **the data source is itself a floating (pulled)
  net**? → the buffer's data seam sees the *resolved* value: a pulled
  idle level flows as data (loud rejection only for unresolvable Z/X at
  the seam).
- What happens when **a buffer output is pulled while disabled**? →
  the buffer contributes Z; an all-Z span with a pull resolves to the
  idle level (story 2, acceptance 1/2).
- What happens when **a pull is set on a single-driver ordinary net**?
  → legal: it only names the idle level for the (today impossible, but
  future-Z-source) all-Z case; driven evaluation is untouched.
- What happens when **a pull is set on a tied input**? → ties and pulls
  are disjoint per input like ties and shared marks: setting a pull on
  a tied input (or tying a pulled input) is a loud refusal — untie
  first. The tie-class resolution itself is unchanged.
- What happens when **a buffer drives a shared net as an extra
  attachment**? → allowed wherever an ordinary driver is allowed
  (within the cap, mark rules unchanged); its contribution to the span
  is its gated value (level when enabled, Z when released), with
  per-attachment polarity applied as for any attachment.
- What happens when **a buffer output is shorted by a tie**? → ties
  never join driver spans: the untie-first rule is unchanged; wiring a
  buffer output into a tie member is refused loudly like any driver.
- What happens when **enable toggles mid-session between evaluations**?
  → deterministic per evaluation: each evaluation resolves from present
  input values; identical input sequences replay identically.
- What happens when **a pulled net feeds a level consumer while
  floating**? → the consumer sees the resolved idle level (a definite
  level flows); only a genuinely unresolvable Z/X reaches a consumer
  seam and fails loudly there.
- What happens to **pulls on save/load/reset**? → pulls are persistent
  topology state (saved and reloaded like marks); contention counts
  stay session diagnostics (never serialized, cleared by reset).
- What happens when **a gate is removed that a buffer's data/enable
  input is wired from**? → exactly that attachment is released per the
  module's existing removal semantics; no dangling driver entries
  survive.
- What happens to **cycle checks for buffer wirings**? → unchanged:
  every buffer wire passes the same dead-slot, input-range,
  tie-conflict, and flop-aware cycle checks; buffers never bypass
  validation.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The module MUST provide an enable-gated buffer gate kind
  with exactly two inputs — data plus an active-high enable — appended
  to the gate-kind vocabulary under the module's append-only rule (no
  renumbering, no reuse of persisted byte values).
- **FR-002**: An asserted enable MUST make the buffer contribute its
  data input's resolved definite level to the driven net's resolution
  span, identically to an ordinary driver presenting that level.
- **FR-003**: A released enable MUST make the buffer contribute exactly
  kHighZ to the resolution span, regardless of the data input's value —
  identical to the attachment being absent.
- **FR-004**: The buffer's data and enable inputs MUST require definite
  levels at evaluation: kUnknown or kHighZ (including dangling) on
  either input MUST fail loudly with simulation state, step count, and
  timing untouched. The buffer MUST NOT silently default an undriven
  enable, invent data, or drive X/Z downstream.
- **FR-005**: Users MUST be able to attach a pull setting (none, up,
  down) to a destination input's net; the setting MUST be per input,
  explicit, inspectable, settable, and clearable, with none as the
  default preserving today's behavior.
- **FR-006**: A net whose resolution span is all-floating (empty or
  every attachment high-Z) MUST resolve to kHigh under pull-up, kLow
  under pull-down, and kHighZ when the pull is none.
- **FR-007**: A pull MUST NOT alter any non-floating resolution:
  driven agreement, low-vs-high contention (still kUnknown, still
  counted), and unknown-dominance MUST resolve exactly as without the
  pull, under every pull setting.
- **FR-008**: Pulls MUST be persistent topology state: saved and
  reloaded identically with the workspace; contention counts MUST remain
  session diagnostics (never serialized, cleared by reset).
- **FR-009**: Pulls and ties MUST be disjoint per input: setting a pull
  on a tied input, or tying a pulled input, MUST fail loudly with the
  netlist untouched — the untie-first rule is unchanged and tie-class
  resolution is unchanged.
- **FR-010**: The buffer kind MUST compose with shared nets: a buffer
  output MAY attach wherever an ordinary driver may (within the cap,
  under the same mark rules and the same wire validation); its span
  contribution is its gated value with per-attachment polarity applied
  as for any attachment.
- **FR-011**: Removing a gate or wire MUST release exactly that buffer
  attachment or pull-bearing wiring — no dangling entries may survive,
  and remaining drivers MUST resolve unchanged.
- **FR-012**: Every buffer wire MUST pass the same validation as any
  other wire (dead slots, input range, tie conflicts, flop-aware cycle
  check); the buffer kind MUST NOT bypass any check, and ties MUST NOT
  join a buffer's driver span.
- **FR-013**: Buffer resolution MUST be verified against an
  independently derived truth table (enable × data → contribution and
  span outcomes), never against the implementation's own output; pulled
  float resolution MUST be verified against the two added reference
  rows on top of the shipped 010/011 table.
- **FR-014**: Single-driver nets with no pull MUST keep today's
  evaluation path with no measurable added cost (benchmark-verified,
  not assumed), and every flow using no buffer and no pull MUST behave
  byte-identically to before the feature.
- **FR-015**: The new vocabulary MUST live inside the module's existing
  command domain (no new top-level command) with completion coverage
  for names and option forms, and interactive output MUST be
  byte-identical to headless by construction.
- **FR-016**: Optimizer passes MUST remain sound, idempotent, and loud
  on netlists containing buffers or pulls: observable resolution
  results MUST NOT change, and a pass that cannot prove safety MUST
  leave them untouched; sequential gates stay opaque barriers.
- **FR-017**: Identity MUST follow the module's stable convention:
  buffer gates take per-kind monotonic never-reused counters in the
  existing scheme; slots recycle, counters do not.

### Key Entities *(include if feature involves data)*

- **Gated buffer (new gate kind)**: a two-input (data + active-high
  enable) combinational gate whose output contribution is the data
  level while enabled and high-Z while released; per-kind counters in
  the stable identity scheme; append-only kind byte.
- **Enable**: the buffer's control input; requires a definite level,
  Z/X fails loudly; active-high only (active-low via an inverter).
- **Pull setting**: the per-destination-input attribute (none/up/down,
  default none) naming what an all-floating span resolves to on that
  net; persistent topology state, inspectable, disjoint from ties.
- **Floating vs. contended net**: all-Z (idle — the pull applies) vs.
  low+high or unknown-bearing (conflict — the pull never applies);
  the shipped funnel distinguishes them before the pull maps the
  float to its idle level.
- **The resolution funnel**: the shipped single four-value fold
  (high-Z transparent, unknown dominant, conflict → X), unchanged and
  sole — buffers choose their contribution, pulls map the all-Z
  outcome, nothing else changes.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Exhaustive gated-span verification — every combination
  of enabled/disabled buffers and ordinary drivers up to the shared
  cap, under every pull setting, resolves exactly per the shipped
  reference table plus the two float-mapping rows, and all attachment
  orders of the same span produce identical results and identical
  contention counts.
- **SC-002**: Idle-level demonstration in one workspace — a bus parked
  with all buffers disabled reads high under pull-up, low under
  pull-down, and high-Z with no pull; enabling one buffer drives its
  level under every pull setting; parking it again restores the idle
  level: distinguishable user-visible outcomes from one construct.
- **SC-003**: Loudness demonstration — a buffer with an X/Z/dangling
  enable or data input fails loudly with timing untouched; a contended
  gated bus resolves X, increments the visible contention count, and a
  level consumer fed that X fails loudly: no silent value is ever
  produced or driven.
- **SC-004**: Zero regressions — every pre-existing test, golden, and
  saved-workspace behavior passes unchanged; single-driver evaluation
  keeps its existing path with no measurable cost added (benchmark
  before/after).
- **SC-005**: Round-trip — a saved buffer-and-pull workspace reloads
  with identical kinds, wirings, marks, attachments, and pulls and
  replays byte-identical outputs; the prior-generation policy at the
  schema bump is exercised loudly (carried or refused) exactly as
  recorded.

## Assumptions

- The shipped single four-value funnel from the tri-state resolution
  feature (`specs/010-tristate-net-resolution`) and the shared-net
  allowlist with loud contention counting
  (`specs/011-multi-driver-nets`) are the foundation: this feature
  adds authoring (a disconnecting driver, an idle level) on top of
  them; no resolution semantics change and no strength, priority, or
  wired-OR/wired-AND model lands.
- The buffer has exactly two inputs (data + active-high enable) and
  one kind byte under the append-only rule; active-low enable is
  expressed with an inverter, and no complementary (active-low or
  inverting-data) variant lands in this feature.
- The buffer is combinational and stateless: it owns no clocked state,
  breaks no combinational cycles by itself, and passes the same
  flop-aware cycle validation as every other gate.
- The pull has exactly three settings (none/up/down), one per
  destination input, default none; it is not per-gate, not per-tie-
  class, and not a multi-bit or programmable-resistance value.
- Pulls apply at the net-resolution seam (per destination input),
  before level consumers: a pulled idle level is a definite level to
  downstream seams; only genuinely unresolvable Z/X reaches a consumer
  and fails loudly there.
- Buffer data/enable inputs consume resolved net values, so a pulled
  idle level upstream flows as ordinary data; the loud Z/X rejection
  at the buffer's own input seams is the module's standing gate-input
  convention, not a new rule.
- Pulls are persistent topology state (like shared marks); contention
  counts stay session diagnostics (cleared by reset, never serialized).
- The exact authoring shape (verbs vs. options for pulls and buffer
  wiring) is a planning decision; the contract requires only that it
  is explicit, per input, loud on refusal, covered by completion, and
  byte-identical across the interactive and headless front ends.
- Bus primitives, importers, strength and delay models stay out of
  scope — each is a named follow-on slot in the module's extension map
  that this feature unblocks.
- Zero-delay evaluation remains in force; stored propagation-delay
  fields stay uninterpreted.
- The four-value vocabulary and every existing gate-kind byte are
  append-only and unchanged except for the one appended buffer kind.
