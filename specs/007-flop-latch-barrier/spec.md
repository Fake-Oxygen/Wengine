# Feature Specification: Flop-Latch Opaque Barrier

**Feature Branch**: `007-flop-latch-barrier`

**Created**: 2026-11-04

**Status**: Draft

**Input**: User description: "Teach O0/O1/O2 to treat flop/latch gates as opaque barriers with DCE protection. Running today's optimizer over a sequential net must not delete or rewire state elements."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Optimize a sequential net at O0 without losing state (Priority: P1)

A hardware-logic user has a working sequential net (combinational gates plus
edge-triggered flops and/or level-sensitive latches, e.g. a DFF counter or a
T divider with surrounding glue logic). They run the logic optimizer at level
O0 over the net. Every flop and latch in the net survives the pass with its
kind, pin count, and wiring unchanged, and re-running the optimized net for N
fixed steps produces step-for-step identical stored values and settled
outputs as the unoptimized net.

**Why this priority**: O0 is the default, always-on simplification level. If
O0 deletes or rewires a single state element, every downstream flow (stepping,
snapshots, saved workspaces) silently corrupts. Preservation at O0 is the
non-negotiable safety floor; nothing else matters if this fails.

**Independent Test**: Can be fully tested by building a small sequential net
(counter with at least one DFF and one latch kind), optimizing at O0, and
comparing gate-by-gate: every sequential gate present before is present after
with the same logical wiring, and a fixed-step run before/after yields identical
readback — delivering state safety with no O1/O2 involvement.

**Acceptance Scenarios**:

1. **Given** a live sequential net with at least one flop and one latch,
   **When** the user optimizes at O0, **Then** the optimized net contains
   every original sequential gate with the same kind and the same logical
   per-pin drivers (same source gates by name, same polarity; a constant
   source may be re-pointed at most to its surviving canonical constant),
   and no sequential gate is removed, merged, or re-typed.
2. **Given** the same net optimized at O0, **When** the user runs the
   original and the optimized net for N fixed steps from the same starting
   state, **Then** stored values and settled outputs match at every step.
3. **Given** a flop whose output drives no block output and no other gate,
   **When** the user optimizes at O0, **Then** the flop is still present in
   the optimized net (state readback keeps it observable; lack of fanout
   never counts as dead).

---

### User Story 2 - Optimize around barriers at O1/O2 without touching state (Priority: P1)

A user optimizes the same sequential net at O1 (bubble-push canonical form)
or O2 (universal-vocabulary conversion). Combinational glue around the flops
may still be simplified or converted per the level's normal rules, but no
rewrite reaches through a flop or latch pin: no bubble-push fuses a NOT
through a flop boundary, no constant is folded into or through a state
element, no flop input is dropped as "dead", and no consumer of a flop output
is rewired to bypass the flop.

**Why this priority**: O1/O2 are the aggressive levels where rewrites are
most likely to reach through state today (fusion, conversion, resynthesis).
If barriers hold only at O0, promoting a sequential design to O1/O2 still
destroys it. Barrier semantics must be uniform across all three levels.

**Independent Test**: Can be fully tested by optimizing a sequential net at
O1 and at O2 and asserting the sequential-gate set is identical (kind,
pins, drivers) while spot-checking that a known combinational redundancy
elsewhere in the same net was still simplified — delivering barrier
uniformity with no run/step involvement.

**Acceptance Scenarios**:

1. **Given** a sequential net containing a NOT driven by combinational logic
   feeding a flop input, **When** the user optimizes at O1, **Then** the
   flop's input driver is unchanged (the NOT is not fused through the flop
   boundary into a re-typed driver).
2. **Given** a sequential net where a flop output feeds combinational logic,
   **When** the user optimizes at O1 or O2, **Then** every consumer of that
   flop output still reads from the same flop output pin (no bypass
   rewiring, no inversion pushed onto the flop).
3. **Given** a sequential net with a constant-driven flop input (e.g. tied
   control), **When** the user optimizes at any level, **Then** the flop and
   its input wiring survive unchanged (the constant is not propagated
   through the state element and the input is never dropped as dead).

---

### User Story 3 - Optimize a sequential net end-to-end with no loud refusal (Priority: P2)

A user runs the standard optimize command over a sequential net at any level
and gets back a usable optimized net (plus the normal before/after report),
not a loud failure. Sequential nets are ordinary optimizer input at every
level; the optimizer never rejects a net merely for containing flops or
latches.

**Why this priority**: Protection by refusal (erroring out on any sequential
gate) would technically "not delete state" but breaks the stated goal: the
optimizer must run over sequential nets. Loud refusal remains correct only
for genuinely unoptimizable situations (budget overflow, topology errors),
never for the mere presence of state elements.

**Independent Test**: Can be fully tested by running the optimize command at
O0, O1, and O2 over a sequential net and asserting each returns success with
a well-formed net — delivering refusal-free optimization with no simulation
involvement.

**Acceptance Scenarios**:

1. **Given** a valid sequential net, **When** the user optimizes at O0, O1,
   and O2, **Then** all three levels succeed and each result contains the
   full original sequential-gate set.
2. **Given** a combinational-only net, **When** the user optimizes at any
   level, **Then** behavior is unchanged from before this feature
   (no new failures, no new rewrites, identical gate counts as before).

---

### User Story 4 - Trust the before/after report on sequential nets (Priority: P3)

A user optimizing a sequential net reads the before/after gate counts and can
trust that any reduction came exclusively from combinational glue: the
sequential-gate count before and after is identical, and the reported
reduction never includes a deleted or merged flop/latch.

**Why this priority**: The report is the user's only window into what the
optimizer did. If counts move without explanation, users cannot tell safety
from corruption. Pinning the state-element count makes the barrier directly
visible in existing output with no new surface.

**Independent Test**: Can be fully tested by optimizing a sequential net with
known redundant combinational glue and checking the report: total gates drop
while the sequential-gate count is unchanged — delivering visible safety
with no new command or flag.

**Acceptance Scenarios**:

1. **Given** a sequential net with redundant combinational glue,
   **When** the user optimizes and reads the report, **Then** the
   sequential-gate count after equals the count before, and any total-gate
   reduction is fully accounted for by removed combinational gates.
2. **Given** a sequential net with no redundant combinational logic,
   **When** the user optimizes at O0, **Then** the optimized net has the
   same total gate count as the original (nothing to simplify, nothing
   removed).

---

### Edge Cases

- What happens when a flop/latch output is completely unobserved (no fanout,
  output ignored)? The gate is still kept: state elements are always
  optimization roots because state readback observes them outside the block
  interface. Lack of fanout never counts as dead for sequential gates.
- What happens when a flop input dangles (no driver)? The flop is kept with
  the dangling pin intact; the optimizer never invents a driver, ties the
  pin, or deletes the flop for having an undriven input.
- What happens when a constant drives a flop input? The constant source in
  the combinational cone may still be simplified per normal rules, but the
  flop input pin itself keeps its driver slot (re-pointed at most to the
  surviving canonical constant source, never dropped or folded into the
  flop). The flop output is always treated as varying, never constant.
- What happens with full-control flops (extra control pins such as async
  reset/set and enable)? All pins of the flop — data, clock, and every
  control pin — are barrier pins with identical protection; no control pin
  is dropped as dead even when it appears statically inactive.
- What happens with inverted flop outputs (NQ consumers)? NQ consumers keep
  reading the same NQ pin of the same flop; the optimizer never pushes an
  inversion through the flop or re-points the consumer at the Q pin (or vice
  versa).
- What happens when two flops have identical wiring? They are never merged:
  each state element is a distinct observable store, so structural
  deduplication skips sequential gates entirely.
- What happens at O2's small-block resynthesis over a net containing flops?
  Resynthesis never absorbs, replicates, or re-encodes a flop; at most the
  combinational regions between barriers are resynthesized, with flop pins
  treated as fixed block boundary values.
- What happens to gate identity and display names? Preserved sequential gates
  keep their slots/counters where the level's existing interface rules keep
  other preserved gates in place; at minimum, name-to-gate resolution before
  and after must find the same sequential gate under the same name.
- What happens to determinism? Optimization remains deterministic: the same
  net optimized twice at the same level yields identical results, and slot
  processing order for the combinational regions is unchanged.
- What happens on repeated O2 nand<->nor toggles over a sequential net?
  Toggles converge instead of multiplying: same-source dangling pins are
  re-tied to one variable per pin (the minimized path's discipline, applied
  structurally), tied duplicates merge in cleanup, and inverter pairs
  cancel — so block-input counts stay pinned and gate counts settle into a
  bounded oscillation (like the combinational 2<->3), never exponential
  growth. Exact round-trip equality is not promised (that is minimized
  resynthesis territory, impossible across state).

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The optimizer MUST preserve every live flop/latch gate
  (all edge-triggered and level-sensitive state-element kinds the netlist
  supports) at O0, O1, and O2: no sequential gate may be deleted, merged
  with another gate, re-typed to a different kind, or converted into
  combinational logic.
- **FR-002**: The optimizer MUST NOT rewire any pin of a sequential gate:
  each preserved gate keeps its kind, pin count, and logical per-pin driver
  (same source gate by identity, same polarity; a constant source may be
  re-pointed at most to its surviving canonical constant), and no input pin
  may be dropped, folded, bypassed, or absorbed into the gate by constant
  propagation, dead-input elimination, bubble-push, or universal conversion.
- **FR-003**: The optimizer MUST NOT rewire any consumer of a sequential
  gate output: every gate input or block output reading a flop/latch output
  (normal or inverted pin) MUST read the same pin of the same gate after
  optimization, with polarity unchanged.
- **FR-004**: Dead-gate elimination MUST treat every live sequential gate as
  a protected root: a sequential gate MUST survive elimination regardless of
  output fanout, ignored-output flags, or whether its outputs reach a block
  output. State readback observes flops outside the block interface, so lack
  of combinational fanout never makes a state element dead.
- **FR-005**: Constant reasoning MUST stop at state-element boundaries: a
  sequential gate output MUST always be classified as varying (never
  constant), and no constant value may propagate through a flop/latch in
  either direction.
- **FR-006**: Structural deduplication MUST skip sequential gates: two
  identically wired flops/latches MUST NOT be merged into one, even when
  all their input drivers match.
- **FR-007**: The optimizer MUST accept nets containing flop/latch gates at
  all three levels (O0, O1, O2) and return a well-formed optimized net; it
  MUST NOT fail merely because sequential gates are present. Loud failure
  remains reserved for genuine error conditions (gate-budget overflow,
  topology errors), reported exactly as today.
- **FR-008**: Combinational optimization around barriers MUST remain
  effective: redundant combinational glue that does not touch a barrier pin
  MUST still be simplified per the level's existing rules (folding, bypass,
  dead-gate removal of purely combinational gates, fusion at O1, conversion
  at O2), so optimizing a sequential net can still reduce total gate count.
- **FR-009**: Optimization of combinational-only nets MUST be
  behavior-identical to before this feature: no new rewrites, no new
  failures, no changed gate counts on nets without sequential gates.
- **FR-010**: Optimization MUST remain deterministic: optimizing the same
  net twice at the same level MUST produce identical results, and repeated
  optimization MUST converge (optimizing an already-optimized sequential
  net changes nothing further).
- **FR-011**: Optimized sequential nets MUST remain simulatable with
  identical semantics: a fixed-step run of the optimized net MUST produce
  step-for-step identical stored values and settled outputs as the original
  net from the same starting state, including async-control priority,
  transparent/frozen latch behavior, and illegal-combination refusal.

### Key Entities

- **State Element**: A live flop or latch gate (edge-triggered or
  level-sensitive, including full-control variants with extra control pins);
  the unit of preservation. Observable via state readback outside the
  combinational block interface, hence never dead by fanout reasoning.
- **Opaque Barrier**: The contract that no optimization pass looks through,
  folds through, or rewires across a state-element pin in either direction;
  combinational reasoning treats each barrier pin as a fixed boundary value.
- **DCE Protection**: Dead-gate elimination roots always include every live
  state element in addition to the existing block-output roots, so
  unobserved flops survive the pass.
- **Combinational Glue**: The non-state gates and wiring between barriers
  and block boundaries; the only region the optimizer may still simplify,
  convert, or resynthesize.
- **Optimization Level**: The user-selected intensity (O0 simplify, O1
  canonical bubbles, O2 universal vocabulary); barrier semantics are
  identical at all three, only the permitted combinational rewrites differ.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Users optimizing a sequential net (at least one flop and one
  latch plus combinational glue) at O0, O1, and O2 get back a net where all
  of the original state elements are present with unchanged wiring, on the
  first attempt in at least 9 of 10 trials.
- **SC-002**: A fixed-step run of 10 steps over an optimized sequential net
  produces stored values and settled outputs identical to the unoptimized
  net at every step, on every repetition.
- **SC-003**: Users optimizing a sequential net containing a completely
  unobserved flop (no fanout) still find that flop present afterward, on
  the first attempt.
- **SC-004**: Users optimizing a purely combinational net observe zero
  change versus previous behavior (same success/failure, same resulting
  gate count) in all trials.
- **SC-005**: Optimizing the same sequential net twice in a row at the same
  level yields an identical net both times, and a second optimization pass
  over an optimized net reports no further change.

## Assumptions

- The set of state-element kinds is fixed by the netlist vocabulary already
  supported (edge-triggered flops and level-sensitive latches, including
  full-control flop variants and inverted-output consumers); this feature
  introduces no new gate kind and no new pin convention.
- "Opaque barrier" pins include every pin of a state element: data, clock /
  enable, and any async-control pins. There is no "safe to fold" subset.
- Block-interface and gate-identity conventions (slots, display names,
  ignored-output handling) are unchanged; preserved state elements follow
  the same in-place stability rules the optimizer already gives preserved
  block outputs.
- No new user-facing command, flag, or report field is required: existing
  optimize levels and the existing before/after report cover this feature.
  If the report already breaks down gate counts by kind, the state-element
  count line is expected to stay flat; no format change is mandated.
- Performance impact is negligible: barrier checks ride on the existing
  per-gate kind dispatch, adding no asymptotic cost and requiring no
  benchmark proof beyond the existing optimizer test suite passing.
- Scope is the optimizer only: stepping, snapshots, persistence formats,
  and interactive verbs are unchanged. Simulation-parity verification
  reuses the existing fixed-step stepper semantics without redefining them.
