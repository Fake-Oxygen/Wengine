# Implementation Plan: Sequential Schematic Symbols

**Branch**: `008-seq-schematic-symbols` | **Date**: 2026-11-07 | **Spec**: [spec.md](spec.md)

**Input**: Feature specification from `/specs/008-seq-schematic-symbols/spec.md`

## Summary

Sequential gates (DFF, DLATCH, JK, T, SRLATCH) currently draw nothing
inside their layout boxes — both raster paths end in
`break; // SEQ12 owns flop/latch symbols`. This plan gives them distinct
rectangular symbols (clock wedge for edge flops, transparency mark for
latches, kind marking on the face, inversion bubble on NQ) and
role-based pin anchors with clock-net wire routes, all computed in the
shared header-only layout math (`include/wengine/logic/layout.hpp`) so
the SDL BMP exporter and the ImGui canvas consume identical geometry.
Approach: extend the layout with a normative per-role anchor table
(CLK row aligned across all edge kinds for horizontal clock tracing),
route wires to those anchors (NQ-aware source anchors via
`driver_inv`), then implement one matching `draw_seq_body` in each
raster TU from layout anchors only. Combinational rendering is
untouched; existing layout goldens must pass byte-identical.

## Technical Context

**Language/Version**: C++20, GCC 13.3 baseline, CMake ≥ 3.20,
out-of-source builds only (`dev` / `test-opt` / `bench` presets)

**Primary Dependencies**: None new. Layout stays dependency-free and
SDL-free (header-only `INTERFACE`); raster work lives in the two
existing CLI edge TUs (`src/cli/logic_draw_sdl.cpp` SDL2 software
raster, `src/cli/logic_draw_imgui.cpp` ImGui canvas). Vendored
imgui/SDL2/yaml-cpp untouched.

**Storage**: N/A — in-memory `Netlist`; YAML workspace schema unchanged
(no format bump, no migration).

**Testing**: Plain-`assert` binaries printing `test_x: OK`, registered
with `add_executable` + `target_link_libraries` + `add_test`; extended
`test_logic_layout` (headless, links `wengine_core` alone) owns anchor +
route goldens; `test_cli_args` owns the headless BMP-path checks
(`logic draw --out`). ImGui window verified by manual windowed run (no
input-automation tooling exists). Every `test_*` target links
`wengine_test_assert` per policy.

**Target Platform**: Linux, both headless (SDL2 software surface) and
display (ImGui canvas) builds; missing-backend loud-failure contracts
unchanged.

**Project Type**: Single project: header-only engine + CLI edge
(Option 1).

**Performance Goals**: Negligible impact — anchor math is O(pins) integer
arithmetic riding the existing layout pass; no benchmark required
beyond the layout suite passing (per spec Assumptions).

**Constraints**: Zero-warning target on `wengine_cli`
(`-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wnon-virtual-dtor`);
layout header must stay SDL/ImGui-free so unit tests link
`wengine_core` alone; determinism (slot-order scans, integer anchor
math, identical coordinates on repeat layout); BMP stays 2x raster of
the 1x layout canvas.

**Scale/Scope**: 5 sequential kinds, 1 shared header extended, 2 raster
TUs extended, 1 layout test extended + BMP assertions extended. No new
targets, commands, flags, registries, or file formats.

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

- [x] **I. Correctness**: No solver/eval/state change — drawing only.
  Anchor fractions hand-derived per kind in `research.md`; goldens use
  `==` on integer coordinates (exactly reproducible, no `near()`).
- [x] **II. Determinism**: Layout remains a pure function of the net:
  slot-order scans, fixed box metrics, integer anchor math. Repeat
  layout asserted identical (existing `test_deterministic` extended to
  sequential nets). No RNG, FP-order, or iteration-order change.
- [x] **III. Testing**: Multi-boundary verification — layout goldens
  (integer `==`) → BMP-path assertions (dimensions + ink presence) →
  manual windowed run for the canvas. Existing combinational goldens
  must pass unchanged (regression guard for FR-008). No new test
  target needed; registry-size asserts unaffected (no registry change).
- [x] **IV. Performance**: No hot path touched (layout runs at draw
  time, allocation already happens there). No benchmark needed;
  claim is "negligible", verified by inspection (O(pins) integer ops).
- [x] **V. Simplicity/scope/discipline**: Smallest coherent diff —
  one header extended + two edge TUs extended. Dependency direction
  preserved (`cli -> solvers`, rasters consume layout; no reverse).
  Engine stays header-only `INTERFACE`; no exceptions/IO/strings in
  layout (anchors are `constexpr` integer functions; `[[nodiscard]]`
  on pure queries). No unrelated cleanup or restyling.
- [x] **Docs**: User-visible change → update `docs/logic.md` §8
  (draw + export: sequential symbols + clock routing paragraph) and
  the draw/export verb help text only if it names symbol behavior.
  README module table/test list only if the test inventory changes
  (new test functions inside existing targets need no README change
  unless the README pins them — check before editing).
- [x] **Git**: No commit/push/branch/PR (not requested).

*Post-design re-check (Phase 1 complete): design keeps all boxes above —
no new deps, no format/registry/eval changes, contracts add no runtime
surface. PASS.*

## Project Structure

### Documentation (this feature)

```text
specs/008-seq-schematic-symbols/
├── plan.md              # This file (/speckit.plan command output)
├── research.md          # Phase 0 output (/speckit.plan command)
├── data-model.md        # Phase 1 output (/speckit.plan command)
├── quickstart.md        # Phase 1 output (/speckit.plan command)
├── contracts/           # Phase 1 output (/speckit.plan command)
│   └── layout-anchors.md
└── tasks.md             # Phase 2 output (/speckit.tasks command - NOT created by /speckit.plan)
```

### Source Code (repository root)

```text
include/wengine/logic/
├── layout.hpp            # EXTEND: role-based seq anchors + NQ-aware routes
├── types.hpp             # read-only: GateKind, Gate, Netlist, driver_inv
├── pins.hpp              # read-only: KindPin role table (D/CLK/RST/SET/EN, J/K/CLK, T/CLK, D/EN, S/R)
└── topo.hpp              # read-only: rebuild + topo_level (column source)

src/cli/
├── logic_draw_sdl.cpp    # EXTEND: draw_seq_body (SDL software raster)
└── logic_draw_imgui.cpp  # EXTEND: draw_seq_body (ImGui canvas) + fix NQ fanout-dot condition

tests/
├── test_logic_layout.cpp # EXTEND: sequential anchor/route goldens (headless)
└── test_cli_args.cpp     # EXTEND: sequential BMP-path assertions (headless)
```

**Structure Decision**: Single-project layout preserved — engine stays
header-only (`layout.hpp` gains `constexpr` anchor queries beside the
existing box metrics; no new header, no new TU, no new target). Raster
symmetry is enforced by construction: both TUs may only read anchors
and routes from layout queries, never compute pin geometry locally
(contract in `contracts/layout-anchors.md`).

## Complexity Tracking

> **Fill ONLY if Constitution Check has violations that must be justified**

| Violation | Why Needed | Simpler Alternative Rejected Because |
|-----------|------------|-------------------------------------|
| — (none) | — | — |
