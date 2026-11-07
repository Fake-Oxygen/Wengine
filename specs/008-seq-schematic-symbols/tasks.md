# Tasks: Sequential Schematic Symbols

**Input**: Design documents from `/specs/008-seq-schematic-symbols/`
(plan.md, spec.md, research.md, data-model.md, contracts/layout-anchors.md, quickstart.md)

**Prerequisites**: plan.md (required), spec.md (required for user stories), research.md, data-model.md, contracts/

**Tests**: Included — required by the spec's per-story Independent Test
criteria and constitution Principle III (multi-boundary verification:
layout goldens → BMP path → manual canvas). Layout goldens use integer
`==`; no `near()`.

**Organization**: Tasks grouped by user story; each story is an
independently testable increment. File-path conventions: single project
(`include/`, `src/`, `tests/` at root).

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3)
- Include exact file paths in descriptions

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Baseline before any geometry change

- [X] T001 Configure dev preset, build, and record baseline `ctest` results from repo root (`cmake -S . -B build -DBUILD_TESTS=ON -DBUILD_BENCHMARKS=ON && cmake --build build && ctest --test-dir build --output-on-failure`)

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Shared layout math owns all sequential geometry (contract
C1–C4, C6) — MUST complete before any raster TU work

**⚠️ CRITICAL**: No user story work can begin until this phase is complete

### Tests for the foundation (write FIRST — must FAIL before T003)

- [X] T002 Write sequential layout goldens in tests/test_logic_layout.cpp (DFF bare + full-control D/CLK rows equal per invariant I-CLK; JK J/K/CLK rows 1/6,4/6,2/6; T rows; DLATCH D/EN rows; SRLATCH S/R rows 1/6,4/6; shared-clock two-flop routes both land at row 2/6 with horizontal mid legs; NQ-source route starts at row 5/6 via driver_inv; dangling-clock flop layouts with no wire; repeat + twin-net identity with sequential gates; all integer `==`; existing combinational tests untouched)

### Implementation for the foundation

- [X] T003 Implement role→row anchor queries, output anchors, and sequential route endpoints in include/wengine/logic/layout.hpp (constexpr `seq_input_row(kind, input)` per contract C2 deriving roles from the pins.hpp KindPin table; Q/NQ output anchors per C3; `layout_schematic` sink `y1` from role rows for sequential sinks and NQ-aware source `y0` via `nl.driver_inv` per C4 with legacy spread/corner math otherwise untouched; `WireRoute` struct unchanged; header stays SDL/ImGui-free, integer-only, `[[nodiscard]]` pure queries; replace nothing combinational)
- [X] T004 Build and run test_logic_layout plus full `ctest --test-dir build --output-on-failure` green with zero warnings (proves T003 satisfies T002 and no combinational golden moved — FR-008/FR-010 gate)

**Checkpoint**: Foundation ready — anchor/route geometry pinned; raster work can now begin

---

## Phase 3: User Story 1 — Tell state elements apart from combinational gates at a glance (Priority: P1) 🎯 MVP

**Goal**: Sequential gates render as distinct rectangular symbols (not
curved comb shapes, not empty boxes) in the file-export path

**Independent Test**: Export a BMP of a net with one DFF, one DLATCH,
and one AND gate: each sequential box shows rectangle + kind-specific
mark, the AND keeps its curved shape (spec US1 acceptance scenarios)

### Tests for User Story 1 (write FIRST — must FAIL before T005)

- [X] T005 [P] [US1] Add sequential BMP assertions in tests/test_cli_args.cpp (sequential-net `logic draw --out`: exit 0, BMP `BM` magic, pixel dims == 2x `layout_schematic` canvas per the existing pattern, and dark ink present inside the former empty-box regions; combinational BMP assertions unchanged). *Note: test-writing may start in parallel with T006 (different files); it passes only after T006.*

### Implementation for User Story 1

- [X] T006 [US1] Implement `draw_seq_body` in src/cli/logic_draw_sdl.cpp (inset rectangle with `kLead=10`/`kPad=4`, centered face letters D/JK/T/SR/DL, wedge triangle at the CLK anchor for DFF/JK/T, stepped notch at the EN anchor for DLATCH, NQ bubble + stub where applicable, input stubs box-edge→border at the C2 anchors — every coordinate from the layout.hpp queries, no local geometry literals; replace the SEQ12 placeholder `break`)
- [X] T007 [US1] Build and run test_cli_args plus full `ctest --test-dir build --output-on-failure` green with zero warnings

**Checkpoint**: At this point, User Story 1 is fully functional — headless BMP export shows distinct flop/latch symbols (MVP: no display needed)

---

## Phase 4: User Story 2 — Trace the clock net from driver to every flop (Priority: P2→P1)

**Goal**: Clock pins at fixed distinct anchors with wedge marks make
every clock net traceable in the interactive window, including
full-control flops and inverted outputs

**Independent Test**: Windowed run of a two-flop shared-clock net plus a
full-control flop: both clock wires land on wedge anchors at equal
height, five control/data pins sit at distinct anchors, NQ wire leaves
from a marked distinct anchor (spec US2 acceptance scenarios; manual —
no input-automation tooling exists)

### Implementation for User Story 2

- [X] T008 [US2] Implement `draw_seq_body` in src/cli/logic_draw_imgui.cpp (mirror of T006 through the same layout queries: rectangle, face letters via the embedded TTF, wedge/notch/bubble/stubs at the identical anchors; input stubs drawn AFTER the body per the file's existing overfill note; replace the SEQ12 placeholder `break`)
- [X] T009 [US2] Manual windowed verification of clock traceability (build with the imgui canvas, draw a shared-clock two-flop net + a full-control DFF + an NQ-feedback net + a dangling-clock flop: wedges at equal height, five distinct anchors on the full-control flop, NQ bubble at the source, undriven clock pin shown with no wire; record result as a comment on the tracking task)

**Checkpoint**: At this point, User Stories 1 AND 2 work — both raster paths paint distinct symbols with traceable clock nets

---

## Phase 5: User Story 3 — See the identical schematic in the window and in the exported file (Priority: P2)

**Goal**: Both display paths consume the shared layout with no private
geometry, proven by audit + automated assertions

**Independent Test**: Same sequential net laid out once renders the
same boxes/anchors/routes on screen and in the BMP; layout twice gives
identical coordinates (spec US3 acceptance scenarios)

### Implementation for User Story 3

- [X] T010 [US3] Add cross-path parity assertions in tests/test_cli_args.cpp (sequential-net BMP: route-spot checks that painted wire endpoints equal the `layout_schematic` route points for a clock wire and an NQ wire; unavailable-backend loud-failure behavior unchanged) and perform contract C7 source audit (no sequential fraction/row/corner literal in either raster TU — all values flow from layout queries; record the grep evidence in the task comment)
- [X] T011 [US3] Build and run test_cli_args plus full `ctest --test-dir build --output-on-failure` green with zero warnings

**Checkpoint**: Parity holds — layout is the single source of truth for both raster paths

---

## Phase 6: User Story 4 — Read dense sequential schematics without overlap confusion (Priority: P3)

**Goal**: Per-output junction dots, preserved legibility invariants, and
docs for the new symbols

**Independent Test**: 4-flop counter with shared clock, fanout, and
inverted feedback draws with no overlapping symbols, no
wire-through-body, and correct junction dots (spec US4 acceptance
scenarios)

### Implementation for User Story 4

- [X] T012 [US4] Split junction dots per output in src/cli/logic_draw_sdl.cpp (Q dot at the Q anchor iff `output_fanout > 1`, NQ dot at the NQ anchor iff `nq_fanout > 1`; combinational gates have `nq_fanout == 0` so their dots render exactly as today)
- [X] T013 [P] [US4] Split junction dots per output in src/cli/logic_draw_imgui.cpp (same rule as T012 — this also fixes the latent gap where NQ-only fanout shows no dot in the window; different file from T012 so the writing parallels it)
- [X] T014 [P] [US4] Add legibility golden assertions in tests/test_logic_layout.cpp (4-flop shared-clock counter net: no two symbol boxes overlap, every route endpoint lies on its anchor, multi-consumer drivers identified per output for the dot rule; different files from T012/T013 so the writing parallels them)
- [X] T015 [P] [US4] Document sequential symbols + clock routing in docs/logic.md (§8 draw + export: rectangle/wedge/notch/faces, anchor table summary, layout-single-source statement) and check the README test list for updates. *Note: may be drafted in parallel once contract C2–C5 are fixed (end of Phase 2).*
- [X] T016 [US4] Final validation: full `ctest --test-dir build --output-on-failure` green on dev and test-opt presets with zero warnings; walk through quickstart.md in specs/008-seq-schematic-symbols/quickstart.md

**Checkpoint**: All user stories independently functional

---

## Phase 7: Polish & Cross-Cutting Concerns

**Purpose**: Residue checks that affect the whole feature

- [X] T017 Verify no stray SEQ12 placeholder comments remain in src/cli/logic_draw_sdl.cpp and src/cli/logic_draw_imgui.cpp and run complete quickstart.md validation in specs/008-seq-schematic-symbols/quickstart.md (layout goldens + BMP export + full suite + manual canvas parity per its troubleshooting expectations)

---

## Phase 8: Convergence (follow-up fixes)

**Purpose**: User-reported readability bug — unrelated nets sharing one column gap collapsed into a single trunk (screenshot: T1.Q→AND1.1 and SRLATCH1.Q→AND1.2 both rode xm = 150)

- [X] T018 Implement deterministic wire-lane stagger in include/wengine/logic/layout.hpp (`wire_lane_offset`: 0, +8, −8, … by same-gap ordinal; same-driver branches still meet at the source anchor where the fanout dot sits), update the two existing xm-pinning goldens in tests/test_logic_layout.cpp (fanout dogleg, input-spread diamond), and add the screenshot-net regression test (`test_unrelated_trunks_stagger`: distinct verticals, anchored endpoints)
- [X] T019 Update contracts/layout-anchors.md (C4 lanes), docs/logic.md (§8 lane sentence), spec.md (corner-math assumption); full `ctest --test-dir build --output-on-failure` green with zero warnings; before/after BMP pixel verification (542px merged trunk → two separated verticals 16px apart, each traceable end to end)

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies — start immediately (T001)
- **Foundational (Phase 2)**: Depends on Setup — BLOCKS all stories (T002→T004)
  - US1 (T005–T007) first — MVP (headless BMP path, no display needed)
  - US2 (T008–T009) after foundation (needs T003 queries; manual windowed check)
  - US3 (T010–T011) after US1+US2 (asserts parity of their implementation)
  - US4 (T012–T016) after US1+US2 (dots/legibility build on both bodies; T014–T015 writing may start in parallel with US1/US2 work — different files)
- **Polish (Phase 7)**: Depends on all stories complete (T017)

### User Story Dependencies

- **User Story 1 (P1)**: Starts after Foundational — no dependencies on other stories; MVP
- **User Story 2 (P1)**: Starts after Foundational — needs T003 queries; independently verifiable in the window
- **User Story 3 (P2)**: Starts after US1+US2 bodies exist — asserts their parity; independently testable per its acceptance scenarios
- **User Story 4 (P3)**: Starts after US1+US2 — dots refine their bodies; legibility goldens need T003 geometry only

### Within Each Phase

- Tests MUST be written and FAIL before implementation (T002 before T003; T005 before T006)
- Layout before rasters (T003 before T006/T008)
- Bodies before dots (T006/T008 before T012/T013)
- Story complete (validation gate green) before next priority

### Parallel Opportunities

- T005 (`tests/test_cli_args.cpp`) writing parallels T006 (`src/cli/logic_draw_sdl.cpp`) — different files
- T012 (SDL TU), T013 (ImGui TU), T014 (layout test), T015 (docs) can all be written in parallel — different files
- T001, T004, T007, T011, T016 validation runs are sequential gates, not parallel work

---

## Parallel Example: User Story 1

```bash
# Test-writing parallels body implementation (different files):
Task: "T005 [US1] Add sequential BMP assertions in tests/test_cli_args.cpp"
Task: "T006 [US1] Implement draw_seq_body in src/cli/logic_draw_sdl.cpp"
# Gate after both:
Task: "T007 [US1] Build and run test_cli_args plus full ctest green"
```

## Parallel Example: User Story 4

```bash
Task: "T012 [US4] Split junction dots per output in src/cli/logic_draw_sdl.cpp"
Task: "T013 [US4] Split junction dots per output in src/cli/logic_draw_imgui.cpp"
Task: "T014 [US4] Add legibility golden assertions in tests/test_logic_layout.cpp"
Task: "T015 [US4] Document sequential symbols + clock routing in docs/logic.md"
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1: Setup (T001)
2. Complete Phase 2: Foundational (T002 write goldens, verify FAIL, T003 implement, T004 green)
3. Complete Phase 3: User Story 1 (T005 write BMP tests, verify FAIL, T006 implement, T007 green)
4. **STOP and VALIDATE**: Export a DFF+DLATCH+AND BMP independently per quickstart.md step 2
5. Deploy/demo if ready (headless-safe artifact, no display needed)

### Incremental Delivery

1. Setup + Foundational → geometry pinned, combinational goldens unchanged
2. Add US1 → BMP shows distinct symbols → validate (MVP!)
3. Add US2 → window shows traceable clock nets → manual verify
4. Add US3 → parity assertions + C7 audit → validate
5. Add US4 → per-output dots + legibility goldens + docs → validate
6. Polish → placeholder sweep + full quickstart

### Parallel Team Strategy

With multiple developers:

1. Team completes Setup + Foundational together
2. Once Foundational is done:
   - Developer A: US1 (SDL body + BMP tests)
   - Developer B: US2 (ImGui body + manual verification)
3. US3/US4 after both bodies land (dots/docs/goldens parallelizable per Parallel Example US4)

---

## Notes

- [P] = different files, no ordering dependency for *writing* (passing may still need the sibling impl — noted per task)
- Constitution gates enforced per task: plain-`assert` style, `test_` targets keep the assert policy, zero warnings on `wengine_cli`, no new commands/kinds/formats/registries, header stays SDL-free, determinism oracles (repeat + twin-net identity), docs updated in-story (T015)
- Commit after each task or logical group (only if explicitly requested — default is no commits per AGENTS.md §14)
- Stop at any checkpoint to validate the story independently
- Avoid: vague tasks, same-file parallel edits, local geometry literals in raster TUs (contract C7), restyling combinational shapes (FR-008)
