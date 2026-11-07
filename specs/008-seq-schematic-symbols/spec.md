# Feature Specification: Sequential Schematic Symbols

**Feature Branch**: `008-seq-schematic-symbols`

**Created**: 2026-11-07

**Status**: Draft

**Input**: User description: "Distinct flop/latch schematic symbols and clock-net routing in shared layout math. Sequential schematics are unreadable with comb AND-boxes; layout stays the single source of truth for both raster paths."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Tell state elements apart from combinational gates at a glance (Priority: P1)

A hardware-logic user draws a sequential net (combinational gates plus
edge-triggered flops and/or level-sensitive latches, e.g. a counter with
surrounding glue). Every flop and latch renders as a rectangular state
symbol that is visually unmistakable from the curved combinational
shapes: edge-triggered flops carry a clock-wedge mark on the clock pin,
level-sensitive latches carry a distinct transparency mark instead, and
the symbol face names the kind (data, jack-knife, toggle, set-reset
variants each identifiable). No sequential gate renders as a curved
combinational shape or as an empty box.

**Why this priority**: Today sequential gates draw nothing inside their
boxes, so any sequential schematic is a field of empty rectangles mixed
with curved gates — unreadable. Distinct symbols are the whole point of
the feature; without them nothing else matters.

**Independent Test**: Can be fully tested by drawing a net with at least
one flop kind, one latch kind, and one combinational gate, and checking
the picture: each sequential box shows its rectangle + kind-specific
mark while the combinational gate keeps its existing curved shape —
delivering symbol distinction with no wire-routing or export
involvement.

**Acceptance Scenarios**:

1. **Given** a net with a data flop, a transparent latch, and an AND
   gate, **When** the user draws the schematic, **Then** the flop shows
   a rectangular body with a clock-wedge mark, the latch shows a
   rectangular body with a transparency mark different from the wedge,
   and the AND gate keeps its existing curved shape.
2. **Given** a net with jack-knife, toggle, and set-reset elements,
   **When** the user draws the schematic, **Then** each kind is
   identifiable from its symbol face (kind marking or pin arrangement),
   never collapsing to a single generic box.
3. **Given** a combinational-only net drawn before and after this
   feature, **When** the user compares the two pictures, **Then** every
   combinational body, wire, dot, grid, and label is pixel-identical to
   before (no restyling of existing shapes).

---

### User Story 2 - Trace the clock net from driver to every flop (Priority: P1)

A user studying timing traces each clock net from its driver pin to
every clock pin it drives. Clock pins sit at a fixed, distinct anchor
on the symbol (separate from data and control pins), clock wires route
through the same shared layout calculation as all other wires, and the
clock destination is obvious (wedge mark at the entry point). Data,
async-control, and inverted-output pins each land at their own stable
anchor so a reader can tell D from CLK from reset/set/enable from Q/NQ
without reading a label.

**Why this priority**: Symbols alone do not make a schematic readable —
if the clock wire lands on an arbitrary evenly-spaced pin slot, the
reader cannot tell which pin is the clock and cannot follow the net.
Pin-anchored, layout-computed clock routing is what turns distinct boxes
into a readable timing picture.

**Independent Test**: Can be fully tested by drawing a two-flop net
sharing one clock driver plus one async-driven flop, and checking pin
positions and wire endpoints: both clock wires land on the clock anchor
of their flops, the async wire lands on a different anchor, and all
endpoints match the shared layout's computed routes — delivering
clock-traceability with no symbol-face or export involvement.

**Acceptance Scenarios**:

1. **Given** two flops sharing one clock driver, **When** the user draws
   the schematic, **Then** both clock wires terminate at the clock anchor
   (wedge position) of their flop, and the two clock anchors sit at the
   same relative position on both symbols.
2. **Given** a full-control flop (data + clock + async reset/set +
   enable), **When** the user draws the schematic, **Then** all five
   inputs land at five distinct, stable pin anchors (clock on the wedge,
   data/control/async each on their own anchor), with no two wires
   sharing one anchor point.
3. **Given** a flop whose inverted output drives another gate,
   **When** the user draws the schematic, **Then** the wire leaves from
   a distinct inverted-output anchor (visually marked as inverted) that
   is different from the normal-output anchor.

---

### User Story 3 - See the identical schematic in the window and in the exported file (Priority: P2)

A user draws a sequential net interactively on screen, then exports the
same net to a picture file (and vice versa). Both pictures show the
same symbols, the same pin anchors, and the same wire routes, because
both display paths consume the one shared layout calculation without
recomputing geometry. A layout golden (numeric box/wire coordinates)
matches what both pictures paint.

**Why this priority**: The feature statement pins the architecture:
layout stays the single source of truth for both display paths. If each
path invented its own symbol geometry or wire math, the two pictures
would drift apart and every golden would be meaningless. Parity is the
guardrail.

**Independent Test**: Can be fully tested by computing the shared layout
for a sequential net once, rendering it through both display paths, and
comparing: the same symbol appears at the same box in both, every wire
in both follows the layout's computed route, with no path-specific
geometry — delivering parity with no new symbol design involved.

**Acceptance Scenarios**:

1. **Given** a sequential net and its computed layout, **When** the user
   views it on screen and exports it to a file, **Then** both pictures
   place every symbol body at the layout's box and route every wire
   through the layout's points (same anchors, same corners, same label
   positions).
2. **Given** the same net laid out twice, **When** the user compares the
   two layouts, **Then** box positions, pin anchors, and wire routes are
   identical (deterministic; slot-order scans, no per-run variation).
3. **Given** a sequential net where one display path is unavailable
   (no display, or no file backend), **When** the user attempts that
   path, **Then** it fails loudly with a rebuild hint exactly as today —
   the available path still draws the full sequential picture.

---

### User Story 4 - Read dense sequential schematics without overlap confusion (Priority: P3)

A user draws a larger sequential net (counter chain with fanout, shared
clock, inverted feedback). Symbols never overlap each other, wires
never pass through a symbol body band reserved for labels, fanout
junction dots still mark true branches, and the per-column level
headers still read left-to-right. The picture stays legible at the
default zoom/fit without manual rearrangement.

**Why this priority**: Distinct symbols that overlap or collide with
wires are barely better than empty boxes. Layout legibility rules
(spacing, bands, dots, headers) must extend to the new symbols and
clock wires unchanged.

**Independent Test**: Can be fully tested by drawing a 4-flop counter
with shared clock plus fanout and checking spacing invariants: no two
symbol boxes overlap, no wire crosses a label band, every multi-fanout
driver shows a junction dot — delivering legibility with no new
interaction.

**Acceptance Scenarios**:

1. **Given** a multi-flop net with a shared clock and fanout,
   **When** the user draws the schematic, **Then** no two symbol boxes
   overlap and no wire passes through another symbol's body.
2. **Given** the same net, **When** the user looks at fanout points,
   **Then** every driver with more than one consumer shows a junction
   dot at its output anchor (normal and inverted outputs counted
   separately), and unrelated wire crossings show no dot.

---

### Edge Cases

- What happens when a flop has full controls (extra async reset/set and
  enable pins)? All pins render at distinct anchors; no pin is dropped
  even when statically inactive, and the symbol grows no wider than the
  standard box (pins stack within the shared box metrics).
- What happens when a clock pin dangles (no driver)? The clock anchor
  and wedge still render; no wire is drawn to it and no driver is
  invented. The gate still belongs to the implicit default clock domain
  semantically, but the picture shows an unconnected clock pin.
- What happens when a latch enable dangles? Same rule: anchor rendered,
  no wire, latch holds per existing semantics — the picture never hides
  an undriven pin.
- What happens when a flop output is completely unobserved (no fanout)?
  The symbol still renders fully with both output anchors visible; lack
  of fanout never collapses or hides the symbol.
- What happens when a net is latch-only (no edge flops)? The picture
  renders with transparency marks and no clock wedges; no clock-specific
  failure or empty-column error appears.
- What happens when the topology is erroneous (combinational cycle,
  missing driver table)? Layout returns the existing empty schematic and
  the draw verb reports the error exactly as today — symbol work never
  masks a topology failure.
- What happens with an empty net (no live gates)? The existing empty
  placeholder renders unchanged.
- What happens to determinism? Layout is a pure function of the net
  (slot-order scans, fixed box metrics, fixed anchor fractions); drawing
  the same net twice yields identical coordinates and identical pictures
  in both paths.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: Every sequential gate kind the netlist supports
  (edge-triggered flops including jack-knife and toggle, transparent
  latches including set-reset) MUST render as a rectangular symbol that
  is visually distinct from all combinational curved shapes; no
  sequential gate may render as a combinational shape or as an empty
  box.
- **FR-002**: Edge-triggered flop symbols MUST show a clock-wedge mark
  at the clock pin; level-sensitive latch symbols MUST show a
  transparency mark that is visually different from the clock wedge.
- **FR-003**: Each sequential kind MUST be identifiable from its symbol
  face (kind marking and/or pin arrangement); two different sequential
  kinds MUST NOT collapse to the same picture when their pin counts
  differ or their kinds differ.
- **FR-004**: The shared layout calculation MUST assign every sequential
  pin a stable anchor derived from the pin's role (data, clock/enable,
  async control, normal output, inverted output) rather than the generic
  evenly-spaced positions used for combinational inputs; the same pin
  role MUST sit at the same relative position on every symbol of that
  kind.
- **FR-005**: Clock wires MUST route through the shared layout as
  ordinary computed wire routes (same corner math family as other
  wires), terminating exactly at the clock anchor; the clock destination
  MUST be identifiable by the wedge at the entry point.
- **FR-006**: Async-control pins (reset/set) and enable pins MUST each
  have their own anchor distinct from data and clock; inverted-output
  pins MUST have their own anchor distinct from normal outputs with a
  visible inversion mark (bubble) at the source.
- **FR-007**: Both display paths (interactive window and file export)
  MUST consume the shared layout's boxes, anchors, and wire routes
  without recomputing geometry; neither path may invent symbol metrics
  or wire corners of its own.
- **FR-008**: Combinational rendering MUST be unchanged: bodies, wires,
  junction dots, grid, column headers, and label placement for nets
  without sequential gates MUST be identical to before this feature.
- **FR-009**: Layout legibility invariants MUST extend to sequential
  nets: no two symbol boxes overlap, no wire passes through a symbol
  body or its label band, multi-consumer drivers show junction dots
  (normal and inverted fanout counted), and column headers still mark
  each level.
- **FR-010**: Layout MUST remain a deterministic pure function of the
  net (slot-order scans, fixed box metrics); laying out the same net
  twice MUST yield identical boxes, anchors, and routes, and error cases
  (topology failure, empty net) MUST behave exactly as today.
- **FR-011**: Undriven sequential pins (dangling clock/enable/data)
  MUST still render their anchor and mark with no wire attached; the
  renderer MUST NOT invent a driver, tie the pin, or hide the symbol.

### Key Entities

- **Sequential Symbol**: The rectangular picture of one flop or latch:
  body, kind marking, clock wedge (flops) or transparency mark
  (latches), per-role pin anchors, output/inverted-output anchors. The
  unit that replaces today's empty box.
- **Clock Net**: A driver net feeding clock pins; traced in the picture
  from one source anchor to one wedge anchor per flop through
  layout-computed wire routes.
- **Pin Anchor**: A fixed point on a symbol where one pin's wire
  attaches, derived from the pin's role; shared-layout data consumed
  identically by both display paths.
- **Shared Layout**: The single deterministic calculation mapping a net
  to symbol boxes, pin anchors, and wire routes; the only geometry
  source either display path may use.
- **Display Path**: One of the two picture producers (interactive
  window, exported picture file); a consumer of the shared layout,
  never a geometry owner.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Users drawing a net with at least one flop, one latch,
  and one combinational gate can name which box is which kind (flop vs
  latch vs combinational) correctly on the first attempt in at least 9
  of 10 trials, without reading text labels.
- **SC-002**: Users tracing a shared-clock net across 4 flops follow
  the wire from driver to each clock wedge without error on the first
  attempt, and correctly distinguish the clock pin from data and async
  pins on a full-control flop in the same picture.
- **SC-003**: The on-screen picture and the exported file picture of the
  same sequential net place every symbol and route every wire
  identically (same boxes, same anchors, same corners) on every
  repetition; laying out the same net twice yields identical
  coordinates.
- **SC-004**: Users drawing a purely combinational net observe zero
  visual change versus previous behavior (same bodies, wires, dots,
  labels) in all trials.
- **SC-005**: Users can draw and export a 4-flop counter with shared
  clock, fanout, and inverted feedback with no overlapping symbols, no
  wire-through-body, and correct junction dots, on the first attempt.

## Assumptions

- Symbol vocabulary follows the established rectangular convention
  (box + clock wedge for edge flops, distinct transparency mark for
  latches, kind marking on the face, inversion bubble on inverted
  outputs). Strict standards compliance (e.g. IEEE shapes) is out of
  scope; readability and kind-distinction are the bar.
- Box metrics stay shared and fixed (the existing column/box/gap/margin
  constants); sequential symbols fit the standard box and pin stacks
  never widen it. No new zoom, pan, fit, or label behavior is
  introduced.
- Clock routing reuses the existing corner math family (mid-column
  doglegs) with role-based endpoints; wires sharing one column gap take
  deterministic staggered lanes so unrelated nets never coincide, while
  same-driver branches still meet at the source anchor (marked by the
  fanout dot). No dedicated clock-spine layer, no orthogonal routing
  pass, and no new wire style (color/width) is introduced in this
  feature.
- Scope is drawing only: netlist vocabulary, stepping semantics, clock
  domains, optimizer behavior, persistence formats, and command
  spellings are unchanged. Existing draw/export failure contracts
  (missing display, missing backend, topology error) are unchanged.
- Performance impact is negligible: anchor math rides on the existing
  per-gate layout pass with no asymptotic change; no benchmark proof
  beyond the existing layout golden suite passing is required.
