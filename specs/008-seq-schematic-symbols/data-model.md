# Data Model: Sequential Schematic Symbols

**Feature**: `008-seq-schematic-symbols` | **Date**: 2026-11-07
**Sources**: [spec.md](spec.md) Key Entities, [research.md](research.md),
`include/wengine/logic/layout.hpp`, `types.hpp`, `pins.hpp`.

No persistent state is added. All entities below are value-semantic,
draw-time derivations of the existing `Netlist`. Integer coordinates in
1x layout units; raster scale (2x) lives in the edge TUs only.

## Entity 1: Sequential Symbol

One rendered flop/latch body. Derived per live sequential gate from its
`Placed` box + `GateKind` + `input_count`.

| Field | Meaning | Derivation |
|-------|---------|------------|
| `box` | (x, y, w, h) in layout units | Existing `Placed` + `kLayoutBoxW/H` (unchanged) |
| `outline` | Inset rectangle | Same `kLead=10` / `kPad=4` insets as comb bodies |
| `face` | Kind letters (`D`, `JK`, `T`, `SR`, `DL`) centered | `gate_kind_name` mapped to short face (contract table) |
| `clock_mark` | Wedge triangle at CLK anchor (DFF/JK/T) or stepped notch at EN anchor (DLATCH); none for SRLATCH | `is_clockish_pin` query; notch shape visually distinct from wedge |
| `nq_bubble` | Inversion bubble at NQ anchor whenever the gate has NQ consumers or NQ is exposed | `nq_fanout > 0` or `!ignore_nq_output` |

**Validation rules** (from FR-001…003):
- Every live sequential kind renders a rectangle; never a curved comb
  shape, never an empty box.
- Wedge and notch are mutually exclusive per gate and visually distinct.
- Two different kinds never produce the same (outline, face, marks,
  pin-count) tuple.

## Entity 2: Pin Anchor

A fixed attachment point on a symbol for one pin role.

| Field | Meaning | Derivation |
|-------|---------|------------|
| `input_anchors[i]` | Left-edge point for input `i` | Role from `pins.hpp` `KindPin` table → row fractions per [contract](contracts/layout-anchors.md); `x = box.x`, `y = box.y + row*h/6` |
| `q_anchor` | Right-edge normal output | `(box.x + w, box.y + h/2)` — existing output convention, unchanged |
| `nq_anchor` | Right-edge inverted output | `(box.x + w, box.y + 5*h/6)` + bubble |

**Validation rules** (from FR-004, FR-006, FR-011):
- Same (kind, role) → same relative anchor on every symbol (bare and
  full-control DFF share D/CLK rows).
- All input anchors of one gate are pairwise distinct; Q ≠ NQ.
- Dangling pins keep anchor + mark with no wire (renderer never invents
  a driver).

## Entity 3: Clock Net (view)

The set of wire routes feeding one clock driver net to CLK anchors.
Not a new store — a subset of `Schematic.wires` filtered by
sink-is-CLK-pin.

| Field | Meaning | Derivation |
|-------|---------|------------|
| `routes` | Manhattan doglegs driver → each CLK anchor | Same 4-point corner math (`xm = (x0+x1)/2`); `y1` = CLK row, `y0` = driver Q/NQ anchor (R4) |
| `wedge_endpoints` | Each route terminates at a wedge mark | By construction: sink anchor == wedge position |

**Validation rules** (from FR-005): shared-clock routes to different
flops land at the same relative anchor; the wedge is visible at every
clock entry point.

## Entity 4: Shared Layout (extended, not replaced)

`Schematic { gates, wires, levels, width, height }` as today, plus the
guarantee that for sequential endpoints the route points equal the
anchor queries (Entity 2). Deterministic pure function of `Netlist`:
slot-order scans, integer math, repeat-identical.

**State transitions**: none (no lifecycle; error cases keep existing
behavior — topology failure → empty schematic with `levels == 0`;
empty net → margin-only placeholder).

## Entity 5: Display Path (consumer)

Interactive window or exported BMP. Reads boxes/anchors/routes only;
owns zero geometry. Per-output junction dots: Q dot when
`output_fanout > 1` at `q_anchor`, NQ dot when `nq_fanout > 1` at
`nq_anchor` (combinational gates: `nq_fanout == 0`, dots as today).

## Relationships

```text
Netlist ──layout_schematic──▶ Shared Layout ──┬──▶ Display Path (window)
  │   ▲                       (boxes+anchors     └──▶ Display Path (BMP file)
  │   │                        +routes)
  │   └── pins.hpp KindPin roles (input meaning)
  └────── driver_inv (NQ polarity for source anchors)
```
