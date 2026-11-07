# Contract: Shared Layout Anchors for Sequential Symbols

**Feature**: `008-seq-schematic-symbols` | **Date**: 2026-11-07
**Parties**: `include/wengine/logic/layout.hpp` (geometry owner) +
`include/wengine/logic/seq_symbol.hpp` (symbol model owner) ↔
`src/cli/logic_draw_sdl.cpp` and `src/cli/logic_draw_imgui.cpp`
(geometry consumers) ↔ `tests/test_logic_layout.cpp` +
`tests/test_logic_seq_symbol.cpp` (verifiers).

This contract is normative: any geometry change lands here first, then
in `layout.hpp`/`seq_symbol.hpp`, then in both TUs together. Neither raster TU may
hard-code a sequential fraction, row, corner, or mark.

## C1. Box metrics (unchanged, restated for completeness)

`kLayoutColW=120`, `kLayoutBoxW=76`, `kLayoutBoxH=56`,
`kLayoutGapY=30`, `kLayoutMargin=30`. Columns = topo level, stacking =
slot order, canvas extents as today. Sequential gates use the standard
box; pins stack inside it, never widen it.

## C2. Sequential input-anchor rows (fractions of box height, from top)

| Role (from `pins.hpp` KindPin names) | Row |
|--------------------------------------|-----|
| D, J, T, S | 1/6 |
| CLK (DFF/JK/T), EN (DLATCH) | 2/6 |
| RST (DFF) | 3/6 |
| K (JK), R (SRLATCH), SET (DFF) | 4/6 |
| EN (DFF full-control) | 5/6 |

Anchor point: `(box.x, box.y + row_num * box_h / 6)` with integer
division evaluated exactly as written (row_num * h first, then /6 —
callers use the layout query, never re-derive). Combinational input
anchors keep the legacy spread `(i+1)*h/(n+1)`.

Invariant I-CLK: every edge-flop CLK anchor sits at row 2/6 regardless
of kind or fan-in (bare and full-control DFF identical).

## C3. Sequential output anchors (right edge)

- Q: `(box.x + w, box.y + h/2)` — the pre-existing output convention.
- NQ: `(box.x + w, box.y + 5*h/6)` with an inversion bubble painted at
  the anchor (same bubble vocabulary as NOT/NAND/NOR/XNOR).

## C4. Wire-route endpoints

- Sink end (`p[3]`): sequential sink → its C2 anchor; combinational
  sink → legacy spread point. `x1 = dst box.x` (left edge).
- Source end (`p[0]`): sink reads NQ (`driver_inv` set) → source NQ
  anchor (C3); otherwise → source Q anchor. `x0 = src box.x + w`.
- Corners (`p[1]`, `p[2]`): mid-column dogleg with a deterministic
  per-wire lane stagger: `xm = (x0 + x1) / 2 + wire_lane_offset(lane)`
  (integer division, then integer offset), where `lane` is the wire's
  ordinal among same-gap (`src_level`, `dst_level`) wires in slot-order
  enumeration and offsets run 0, +8, −8, +16, −16, … (`kWireLaneStep`).
  Lane 0 keeps the legacy midpoint, so single-wire channels render
  exactly as before; unrelated nets sharing a gap never coincide on one
  vertical trunk, while same-driver branches still meet exactly at the
  source anchor (where the fanout dot marks the connection).
- `WireRoute` struct is unchanged; `to_input` continues to identify the
  sink pin. Dangling pins produce no route.

## C5. Symbol faces and marks (what each TU paints from C2–C3)

- Body: inset rectangle (`kSeqLead=10`, `kSeqPad=4` in `seq_symbol.hpp` —
  same insets as comb bodies so stubs land identically).
- Face letters centered: DFF `D`, JK `JK`, T `T`, SRLATCH `SR`, DLATCH
  `DL`.
- Edge flops (DFF/JK/T): wedge triangle at the CLK anchor pointing into
  the box. Latch (DLATCH): stepped notch at the EN anchor (two-segment
  step — never a triangle). SRLATCH: no wedge, no notch.
- Input stubs: box edge → body border at each C2 anchor (1px overshoot
  into the border, same as OR/XOR stubs). NQ stub: bubble → box edge.
- Labels below boxes, grid, column headers, wire ink, dots: as today,
  with dots per output (Q dot iff `output_fanout > 1` at Q anchor; NQ
  dot iff `nq_fanout > 1` at NQ anchor).

## C6. Determinism and failure behavior

Layout is a pure function of the `Netlist`: slot-order scans, integer
math only, repeat-identical (asserted by the determinism test extended
to sequential nets). Topology failure → empty schematic (`levels==0`);
empty net → margin-only placeholder. Both unchanged.

## C7. Conformance checklist for reviewers / tasks.md

- [ ] No fraction, row, corner, or mark literal for sequential pins appears in
      either raster TU (grep `6` near `draw_seq` must find nothing
      geometry-defining; all values flow from the symbol model;
      wedge/notch/bubble pixel mapping is the only per-TU code).
- [ ] `test_logic_seq_symbol` pins the model (ints == anchor queries +
      NQ/dot predicates, `kKinds` single-owner loop) with no SDL/ImGui.
- [ ] `test_logic_layout` pins every C2 row and a C3 NQ source route
      with integer `==`.
- [ ] Combinational goldens byte-identical (no legacy anchor touched).
- [ ] BMP export of a sequential net: dimensions == 2x layout canvas;
      ink present inside former empty-box region.
- [ ] Manual windowed run: wedge/notch/faces match the BMP.
