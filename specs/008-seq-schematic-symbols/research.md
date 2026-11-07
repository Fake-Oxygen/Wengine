# Research: Sequential Schematic Symbols

**Feature**: `008-seq-schematic-symbols` | **Date**: 2026-11-07
**Status**: All unknowns resolved — no NEEDS CLARIFICATION remained after
code inspection (pin roles in `pins.hpp`, layout math in `layout.hpp`,
raster seams in both draw TUs, BMP test pattern in `test_cli_args.cpp`).

## R1. Where pin roles come from (no new table)

**Decision**: Derive every anchor from the existing `KindPin` role table
in `include/wengine/logic/pins.hpp` (`kKinds`), matched by
`(kind, input_index)` → role name. No new role table, no duplicated
pin knowledge.

**Rationale**: The table already owns per-kind pin names and fan-in
validity with a `static_assert` (`kind_table_valid`): DFF
`D0/CLK1/RST2/SET3/EN4` ({2,5} inputs), DLATCH `D0/EN1`, JK `J0/K1/CLK2`,
T `T0/CLK1`, SRLATCH `S0/R1`. Anchor code switches on the resolved
role, so a future kind reuses the same path by adding its row.

**Alternatives considered**:
- Index-based anchors (`(i+1)*h/(n+1)` as today): rejected — bare vs
  full-control DFF would place CLK at different heights and JK's CLK
  (index 2) would sit below K, breaking horizontal clock tracing.
- New parallel role enum: rejected — duplicates `KindPin` names and
  drifts from the `static_assert`-pinned table.

## R2. Anchor grid: fixed 6ths, CLK row aligned across edge kinds

**Decision**: Box interior split into 6ths; role → row:

| Role | Row (fraction of box height, from top) |
|------|----------------------------------------|
| D, J, T, S (data / first pin) | 1/6 |
| CLK (DFF/JK/T), EN (DLATCH) | 2/6 |
| RST (DFF full-control) | 3/6 |
| K (JK second), R (SRLATCH second), SET (DFF) | 4/6 |
| EN (DFF full-control) | 5/6 |

Wait — K and SET share 4/6 on different kinds; that is fine (same-kind
uniqueness is what matters: JK rows are 1/6,4/6,2/6 — all distinct;
DFF rows 1/6,2/6,3/6,4/6,5/6 — all distinct; DLATCH 1/6,2/6; T 1/6,2/6;
SRLATCH 1/6,4/6 — S top / R bottom reads naturally).

Outputs (right edge): Q → mid (`h/2`, the existing output convention,
unchanged so Q-driven wire starts match old files pixel-wise where a
box used to sit); NQ → 5/6 with an inversion bubble at the source.

**Rationale**: The single most valuable geometric property is that
**every clock wedge sits at the same relative height (2/6)** on every
edge flop, so shared-clock wires run horizontally and the eye follows
them. Data always sits top (1/6). Async controls stack below the clock
in priority order (RST > SET > EN mirrors `seq_step.hpp` priority).
Box metrics (`kLayoutBoxW/H`, margins, columns) are untouched, so all
existing combinational goldens hold.

**Alternatives considered**:
- Even spread per fan-in (status quo): rejected per R1 (CLK height
  varies by kind/fan-in).
- Dedicated clock-spine layer / orthogonal router: rejected — spec
  Assumptions pin the existing mid-column dogleg family; a new router
  is out of scope and risks determinism goldens.
- New wire colors/widths for clocks: rejected — same assumption; wedge
  + anchor alignment carry the distinction.

## R3. Symbol faces: rectangle + kind letters + wedge/notch + NQ bubble

**Decision**: Sequential body = inset rectangle outline (same
`kLead=10`/`kPad=4` insets as comb bodies so input stubs land
identically), kind letters centered on the face (`D`, `JK`, `T`, `SR`,
`DL` — short enough for both the SDL pixel font and the TTF path),
clock wedge = small triangle pointing into the box at the CLK anchor
for DFF/JK/T, transparency mark = stepped notch (two-segment step,
visually distinct from the wedge) at the EN anchor for DLATCH, SRLATCH
gets no wedge (no clock) but keeps the rectangle + `SR` face. NQ source
= bubble at the NQ anchor + stub to the box edge (mirrors the existing
`bubble_at`/`halo_circle` inversion vocabulary).

**Rationale**: Reuses the established in-TU inversion vocabulary
(bubbles) and text helpers (`gfx::draw_text` / TTF `AddText`) with zero
new deps; rectangle vs curved outline satisfies FR-001; wedge vs notch
satisfies FR-002; face letters + pin arrangement satisfy FR-003. Strict
IEEE-91 shapes are out of scope per spec Assumptions.

**Alternatives considered**:
- Full IEEE distinctive shapes for flops: rejected — new geometry
  family, harder parity proof across two raster backends, no user asked.
- No face letters (pin arrangement only): rejected — T vs DLATCH
  (both 2-input) would be near-identical boxes.

## R4. NQ-aware wire sources via `driver_inv` (layout reads, rasters reuse)

**Decision**: In `layout_schematic`, the source endpoint `y0` becomes
role-aware: for a wire whose sink reads NQ
(`nl.driver_inv[s*kMaxInputs+i] != 0`, set only by `wire_pins` from NQ
sources), start at the NQ anchor (5/6); otherwise start at the Q anchor
(mid). Sink endpoint `y1` uses the role-row table (R2) for sequential
sinks, unchanged spread formula for combinational sinks. `WireRoute`
struct is unchanged (endpoints already carry the anchors); `to_input`
already identifies the sink pin.

**Rationale**: Today `y0` is always the source mid, so an NQ-driven
wire would start at the wrong anchor once symbols distinguish Q/NQ.
`driver_inv` is the existing single source of NQ truth (zero-init =
non-inverted); layout only reads it. Rasters need no new data: routes
already encode both ends, and each TU already receives `nl` for body
drawing (to resolve NQ bubbles and face letters).

**Alternatives considered**:
- New `from_output` field on `WireRoute`: viable but unnecessary —
  endpoints + `nl.driver_inv` fully determine Q-vs-NQ at draw time;
  keep the struct stable so existing golden comparators keep working.
- Ignoring NQ (always start at mid): rejected — wire would visibly
  detach from the NQ bubble.

## R5. Fanout dots: per-output anchors, ImGui condition fix

**Decision**: Junction dots move from "one dot at mid-right when total
fanout > 1" to per-output dots: Q dot at the Q anchor when
`output_fanout > 1`, NQ dot at the NQ anchor when `nq_fanout > 1`.
This also fixes a latent inconsistency: the SDL TU dots on
`output_fanout + nq_fanout > 1` while the ImGui TU dots only on
`output_fanout > 1` (NQ-only fanout shows no dot in the window).

**Rationale**: With Q and NQ at different anchors, a single mid dot
would float between them. Per-output dots satisfy spec FR-009
("normal and inverted fanout counted"). Combinational gates have no NQ
(`nq_fanout == 0`), so their dots render exactly as today.

**Alternatives considered**:
- Keep single mid dot: rejected — geometrically wrong once outputs
  split.
- Dot only Q: rejected — NQ fanout branches would lose their branch
  mark.

## R6. Parity enforcement: layout owns all geometry, TUs are pure paint

**Decision**: New `constexpr` queries in `layout.hpp` beside the box
metrics: `seq_input_row(kind, input)` (role→row numerator, R2 table),
`seq_input_anchor(...)` / `seq_output_anchor(...)` (pixel points given
a `Placed` box), `is_clockish_pin(kind, input)`. Both raster TUs call
these for stubs, wedges, notches, bubbles, and face-letter placement;
neither TU hard-codes a sequential fraction. Layout goldens pin the
numbers; the contract file pins the rule for reviewers.

**Rationale**: This is the spec's architectural invariant (FR-007,
FR-010) made reviewable: any future geometry tweak lands in one header
and both pictures move together. Header stays SDL/ImGui-free
(constitution V), integer-only (determinism II), `[[nodiscard]]`
pure queries.

## R7. Test strategy per boundary (constitution III)

**Decision**:
- `test_logic_layout` (headless, `wengine_core` only): new goldens —
  DFF bare + full-control anchors, JK/T/DLATCH/SRLATCH anchors,
  shared-clock two-flop route (both land at 2/6 row, horizontal wire),
  NQ-source route (starts at 5/6), dangling-clock flop (no wire, layout
  still succeeds), sequential determinism (repeat layout identical,
  twin-net identical). All `==` on integers. Existing combinational
  tests untouched (FR-008 guard).
- `test_cli_args` BMP path: sequential net `draw --out` asserts
  dimensions == 2x layout canvas (existing pattern) + ink-presence:
  BMP differs from the pre-feature empty-box render (spot-check
  non-white pixel count grows / a dark pixel exists inside a former
  empty box region). Exact byte-pin of the whole BMP is avoided
  (font/halo raster details are backend-owned); geometry is pinned in
  layout goldens instead.
- Manual: ImGui window run + screenshot eyeball for wedge/notch/face
  parity (no automation tooling exists per AGENTS.md §9).

**Alternatives considered**:
- Pixel-perfect BMP golden: rejected — couples the test to backend
  raster minutiae (halo discs, font builds) that the architecture
  deliberately leaves backend-owned.
- New test target: rejected — existing targets own their seams;
  no registry/size asserts affected.
