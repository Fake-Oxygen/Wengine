# Feature Specification: Tri-state Net Resolution (kHighZ/kUnknown Semantics)

**Feature Branch**: `010-tristate-net-resolution`

**Created**: 2026-11-19

**Status**: Draft

**Input**: User description: "What: Give the reserved kHighZ/kUnknown values real resolution semantics in the single funnel function. Why: Reserved slot from v1; without it no bus, enable-gated driver, or contention detection is expressible."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Detect contention on a shared net (Priority: P1)

A logic-module user drives one net from two active drivers presenting
opposite levels (one low, one high). The net resolves to **unknown
(kUnknown, X)** — the conflict is detected and represented, never
silently resolved to one of the contenders. Readback shows the net as
unknown, distinct from every legal level; consumers that require a real
level (gate inputs, sequential control nets) keep failing loudly with
simulation state and step timing untouched, exactly as they do today
when X reaches them.

**Why this priority**: Contention between drivers is a real hardware
fault class; resolving it to a silently chosen winner corrupts
downstream state invisibly. The unknown marker was reserved in v1
precisely for this; giving it semantics is the correctness floor for
every multi-driver use case. Nothing else in this feature matters if a
0-vs-1 fight can produce a plausible-looking level.

**Independent Test**: Can be fully tested by resolving a span of two
drivers `{low, high}` and asserting the result is exactly kUnknown
(and never kLow/kHigh/kHighZ), plus asserting an existing consumer
given that result still rejects it loudly — delivering contention
detection with no tri-state/agreement story involved.

**Acceptance Scenarios**:

1. **Given** a net with two active drivers presenting opposite levels,
   **When** the net resolves, **Then** the resolved value is exactly
   kUnknown — a third, conflict-marked outcome that is neither
   contender's level.
2. **Given** a resolved value of kUnknown fed to a consumer that
   requires a definite level (a gate input or a sequential control
   net), **When** the consumer evaluates, **Then** the consumer rejects
   it loudly, and the simulation state, step count, and timing are
   untouched (same convention as v1 X/Z rejection).
3. **Given** a net whose drivers present `{low, high, unknown}` or
   `{high, unknown}`, **When** the net resolves, **Then** the result is
   kUnknown regardless of how many level-presenting drivers exist — an
   unknown driver never gets outvoted by a real level.

---

### User Story 2 - Model an enable-gated tri-state bus (Priority: P1)

A user models a bus where drivers can be off: a disabled (enable-gated)
driver presents **high-Z (kHighZ, Z)** and contributes nothing. With one
enabled driver, the bus shows that driver's level; disable it and the
bus becomes Z (undriven); enable two conflicting drivers and the bus
becomes X (story 1). The user can therefore express "driver present but
off" as a first-class state, distinct from both "driving low/high" and
"fighting".

**Why this priority**: The bus and the enable-gated driver are the two
expressive powers the reserved Z value exists for; without transparent
Z semantics no tri-state structure can be written down at all. Shares
P1 with contention because the two together are the minimum coherent
four-value bus model.

**Independent Test**: Can be fully tested by resolving spans
`{high, high-Z}` → kHigh, `{high-Z}` → kHighZ, and `{high-Z, high-Z}`
→ kHighZ, and asserting that adding or removing any number of high-Z
drivers never changes a span's resolved value — delivering
enable-gated buses with no contention or agreement story involved.

**Acceptance Scenarios**:

1. **Given** a net whose drivers are one active level plus any number
   of high-Z drivers, **When** the net resolves, **Then** the result is
   the active driver's level, identical to the same span without the
   high-Z drivers.
2. **Given** a net whose drivers are all high-Z (or a net with no
   drivers at all), **When** the net resolves, **Then** the result is
   exactly kHighZ — undriven, distinct from kUnknown contention.
3. **Given** an enable-gated driver that toggles between off (Z) and
   on (level), **When** the user resolves the bus across that toggle,
   **Then** the bus value tracks the driver's enabled level and
   falls back to kHighZ when it is the only driver and turns off.

---

### User Story 3 - Let agreeing drivers coexist (wired consensus) (Priority: P2)

A user ties several drivers to the same net and all active ones agree
(the classic wired-OR/wired-AND intent). The net resolves to the agreed
level — not to X, and with no technology-specific reinterpretation
(no pull-up/pull-down strength model): agreement means the level,
disagreement means unknown (story 1).

**Why this priority**: Consensus is required for the common redundant-
drive and tie-off patterns to be usable at all, but it adds no new
value vocabulary — it only removes a false conflict between drivers
that happen to agree. It ranks below the P1 stories because a wrong
conflict answer is an inconvenience; a wrong contention answer is
silent corruption.

**Independent Test**: Can be fully tested by resolving `{low, low}`,
`{low, low, low}`, and `{high, low, high-Z}`-style spans and asserting
the agreed level wins whenever only one distinct level is active —
delivering consensus with no enable-gating semantics involved.

**Acceptance Scenarios**:

1. **Given** a net with two or more active drivers all presenting the
   same level, **When** the net resolves, **Then** the result is that
   level.
2. **Given** a net with active drivers agreeing and any number of
   high-Z drivers mixed in, **When** the net resolves, **Then** the
   result is still the agreed level (high-Z drivers stay transparent).

---

### User Story 4 - Keep every v1 single-driver flow bit-identical (Priority: P2)

A user with existing single-driver nets (all v1 content: one wire per
input, levels 0/1 only) continues to get exactly today's results: a
single low/high driver resolves to its own level, existing consumers
behave identically, saved workspaces load identically, and no golden
output anywhere changes. The new semantics are strictly additive
behind the same seam the v1 funnel already occupies.

**Why this priority**: The funnel is on every input's path; any drift
in single-driver behavior would invalidate every existing golden and
every saved design in one move. It ranks below P1 only because v1
content never exercises the new values, so preservation is a constraint
rather than new capability.

**Independent Test**: Can be fully tested by running the pre-existing
logic test suite and asserting that every test not pinning the v1
funnel's Z/X-refusal placeholder passes unchanged, plus resolving
`{low}` and `{high}` spans and asserting the level passes through —
delivering regression safety with no multi-driver story involved.

**Acceptance Scenarios**:

1. **Given** a span containing exactly one driver presenting low or
   high, **When** the net resolves, **Then** the result is that value,
   the same as before the change.
2. **Given** any existing netlist, workspace, or recorded golden from
   v1, **When** loaded and run after this feature, **Then** all
   outputs are identical to before (the resolution vocabulary may not
   renumber, reorder, or reinterpret its existing members).
3. **Given** the v1 placeholder behavior — the funnel refusing Z/X
   spans and non-single spans, which no live v1 content can reach
   (every live call path presents one low/high driver), **When** the
   new semantics land, **Then** those previously-refused spans resolve
   per FR-002..FR-008 and the tests that pinned the old refusal are
   re-pinned to the new resolution table in the same change — the
   superseded placeholder is retired loudly in the tests, never
   silently kept beside the new contract.

---

### Edge Cases

- What happens with **no drivers at all** (empty span)? → resolves to
  kHighZ (undriven), not an error and not kUnknown.
- What happens with **a single kUnknown driver**? → kUnknown (an
  unknown driver alone already poisons the net; there is nothing to
  outvote it).
- What happens with **kUnknown mixed into a conflict**
  (`{low, high, unknown}`)? → kUnknown either way (X dominance and
  0-vs-1 conflict agree on the outcome; no ordering can rescue a
  level).
- What happens with **many high-Z drivers** (large spans)? →
  resolution stays exact and cost stays proportionate to span length;
  high-Z drivers never accumulate error.
- What happens when the **same span is presented in a different
  order**? → identical result: resolution depends only on the driver
  values as a multiset, never on slot order, so determinism cannot
  depend on driver enumeration order.
- What happens with **v1-shaped calls** (span of one low/high value,
  including every existing internal call site)? → today's result,
  bit-identically.
- What happens to the **value vocabulary itself**? → nothing: the
  four members keep their existing names, meanings, and ordering
  (append-only convention); only the resolution behavior gains
  semantics.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The resolution seam MUST accept spans of any driver
  count (zero or more), not only the single-driver spans of v1.
- **FR-002**: A driver presenting kHighZ MUST be transparent: adding
  or removing any number of high-Z drivers MUST NOT change a span's
  resolved value.
- **FR-003**: With exactly one active (non-high-Z) driver, the net
  MUST resolve to that driver's level.
- **FR-004**: With multiple active drivers all presenting the same
  level, the net MUST resolve to that level.
- **FR-005**: With active drivers presenting both low and high, the
  net MUST resolve to kUnknown; the resolution MUST NOT pick either
  contender's level.
- **FR-006**: With any driver presenting kUnknown, the net MUST
  resolve to kUnknown regardless of the other drivers (unknown
  dominates low/high; high-Z never masks it).
- **FR-007**: With no active drivers (empty span or all high-Z), the
  net MUST resolve to kHighZ — an outcome distinct from kUnknown so
  undriven and contended are never conflated.
- **FR-008**: Resolution MUST be total over driver-value combinations:
  every well-formed span resolves to exactly one of the four defined
  values, and value combination itself MUST NOT be an error at the
  resolution seam.
- **FR-009**: Resolution MUST be order-independent and deterministic:
  any permutation of the same driver values MUST yield the same
  result, and identical spans MUST always resolve identically.
- **FR-010**: Single-driver v1 behavior MUST be preserved bit-exactly:
  a span of one low/high driver resolves to that value, and existing
  consumers MUST keep rejecting kUnknown/kHighZ inputs loudly with
  simulation state and timing untouched.
- **FR-011**: The resolution table MUST be verified against an
  independently derived reference (a hand-derived truth table anchored
  to the four-value wired-resolution convention used by standard
  hardware-description practice), never against the implementation's
  own output.

### Key Entities *(include if feature involves data)*

- **Driver contribution**: what one driver presents to a net — a level
  (low/high), high-Z (present but not driving), or unknown (driving an
  unresolved value).
- **Resolved net value**: the single four-value result, with origin
  semantics: low/high = driven level; kHighZ = undriven net;
  kUnknown = contention or propagated unknown.
- **Contention**: at least two active drivers presenting opposite
  levels; the condition kUnknown exists to represent.
- **Resolution (the single funnel)**: the one seam every input
  resolves through; total, commutative, order-independent combination
  of driver contributions into one net value.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: 100% of the exhaustive resolution table matches the
  independent reference — every unordered combination (multiset) of
  driver values up to length 4 (69 combinations: 4 singles, 10 pairs,
  20 triples, 35 quadruples) resolves to the referenced outcome, plus
  all distinct orderings of representative spans.
- **SC-002**: Zero regressions outside the intentionally changed
  contract — every pre-existing test that does not pin the v1
  funnel's Z/X-refusal placeholder passes unchanged; the placeholder
  assertions are superseded by the new resolution table in the same
  change, and every live v1 call path (single low/high driver)
  behaves identically.
- **SC-003**: Order-invariance verified mechanically — for every
  tested span, all distinct permutations of the same driver values
  produce identical results, so no caller can observe driver
  enumeration order.
- **SC-004**: All three motivating scenarios are expressible and
  distinguishable in a single demonstration: an enable-gated bus
  resolving to the enabled level, the same bus with a second
  conflicting driver resolving to kUnknown, and the same bus fully
  disabled resolving to kHighZ — three different user-visible answers
  from one construct.

## Assumptions

- The four-value resolution follows the standard wired-resolution
  convention (as used in IEEE 1164-style four-value modeling):
  high-Z is neutral/transparent, unknown dominates low/high, and
  low-vs-high conflict resolves to unknown. No technology-specific
  strength model (pull-ups/pull-downs, wired-AND/wired-OR device
  semantics) is in scope: agreement means the level, conflict means
  unknown.
- An empty driver span means an undriven net and resolves to kHighZ.
- Scope is the resolution semantics of the single funnel function
  itself. Netlist authoring is unchanged: the single-driver wiring
  rule, the wire/unwire verbs, the workspace schema, and serialization
  behave exactly as today; multi-driver netlist constructs (bus
  primitives, importers, a tri-state gate kind) are the follow-on
  work this feature unblocks and are out of scope here.
- Existing consumer behavior is intentionally preserved: gate inputs,
  sequential control nets, and block sinks keep rejecting X/Z loudly
  — contention is *detected* (readback shows X/Z distinctly; consumers
  fail loudly with state/timing untouched), not silently propagated
  through levels.
- Zero-delay evaluation remains in force; the stored
  propagation-delay fields stay uninterpreted (separate V2 slot).
- The four value vocabulary is append-only: existing member names,
  meanings, and ordering never change; only resolution behavior is
  added.
