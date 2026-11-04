# Tasks: Versioned Workspace v1 (Flop Initial States + Clock-Domain Metadata)

**Input**: Design documents from `/specs/006-versioned-workspace-v1/` (spec.md, plan.md, research.md, data-model.md, contracts/workspace-v1.md, quickstart.md)

**Prerequisites**: plan.md (required), spec.md (required for user stories), research.md, data-model.md, contracts/

**Tests**: INCLUDED — not optional here. Constitution Principle III (independent-reference testing) mandates multi-boundary verification for engine/store changes, and every spec acceptance scenario requires executable proof. Test tasks are therefore governance-required, and the tree must build green plus `ctest` clean after every phase.

**Organization**: Tasks grouped by user story. Real layering: US2/US3/US4/US5 all build on US1's loader/saver (documented below); US1 alone is the MVP increment.

**Implementation status (2026-11-04, /speckit.implement)**: T001–T027 complete, full `ctest` green (73/73) with zero new warnings. Notes: T022's five-file conversion was pulled forward into T011 (required for the US1 green checkpoint); T015's determinism test lives in tests/test_logic_seq_cli_step.cpp (session-level load+run, more fitting than projectile-focused test_determinism.cpp); T026 spans test_logic_yaml.cpp (bundle pins) + test_logic_seq_cli_step.cpp (Shell transcripts, via a test-only WENGINE_LOGIC_EXAMPLE_DIR compile def following the test_logic_yaml precedent).

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3)
- Exact file paths in every description

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Confirm a green, conflict-free starting point in the existing repo (no new scaffolding needed).

- [X] T001 Establish green baseline: configure, build, and run the full ctest suite from the repo root into build/; record any pre-existing failures before touching code
- [X] T002 [P] Inspect git status/diff for pre-existing local changes and record what must be preserved untouched during edits (repo root)

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Shared types and single derivation implementation that all stories depend on. Headers only — no behavior change, tree stays green.

**⚠️ CRITICAL**: No user story work can begin until this phase is complete.

- [X] T003 [P] Create pure engine-owned clock-domain derivation in include/wengine/logic/clock_domains.hpp (header-only, no yaml includes: source-pin spelling names, bare `clk` implicit domain, edge kinds DFF/JK/T only, first-seen slot order, slot-ordered members)
- [X] T004 [P] Add plain-data Workspace bundle struct plus load/save_workspace declarations in include/wengine/logic/yaml_store.hpp (Netlist layout untouched per the additive-only rule)
- [X] T005 [P] Add per-slot power-on init field with all-Low default to SequentialRunConfig in include/wengine/logic/seq_config.hpp (inside the existing nested section; no flat fields)

**Checkpoint**: Headers compile standalone; no behavior change; full suite still green — user story work can now begin.

---

## Phase 3: User Story 1 — Save and reopen a sequential workspace identically (Priority: P1) 🎯 MVP

**Goal**: New-generation `version: 1` schema readable and writable end-to-end: per-gate `init:` keys, required `clocks:` section, session save/load round-trip with new-lifetime pool handling, all production callers converted.

**Independent Test**: quickstart.md §1 — build a sequential net, save, reload fresh, byte-identical re-save (`cmp` silent), `version: 1` + `clocks:` markers present.

### Tests for User Story 1 (governance-required — write FIRST, ensure they FAIL before T007+)

- [X] T006 [US1] Extend tests/test_logic_yaml.cpp with v1 canonical-bytes golden, YAML-superset tolerance, save/load/save stability, and the full contract §2 loader-gate matrix (accept new v1; refuse legacy-v1-shaped, legacy v2, and foreign versions with version-naming diagnostics)

### Implementation for User Story 1

- [X] T007 [P] [US1] Implement save_workspace/load_workspace v1 schema in src/logic/yaml_store.cpp (gates with sequential-only `init:` keys defaulting 0, wires, ties, required `clocks:` with set-equality validation against the engine derivation, meta; version gate `want 1`; delete both legacy fallback paths)
- [X] T008 [P] [US1] Replace the session ClockDomain derivation with the engine header in src/cli/commands/logic/common.cpp (vocabulary-preserving; existing step tests pin behavior)
- [X] T009 [US1] Rewire session save/load onto the bundle API in src/cli/commands/logic/common.cpp (save_yaml streams live pool Q as inits plus derived clocks; yaml::load_text returns the bundle) and implement new-lifetime pool handling in src/cli/commands/logic/load.cpp (reset, resync, apply inits, then existing async-control/level-settle application; snapshot and restore pool plus workspace on any failure)
- [X] T010 [P] [US1] Convert remaining production callers to the bundle API in src/cli/commands/sim.cpp (--seq-file load fills netlist plus section inits; completion loader path) and pass pool Q through the schema export in src/cli/commands/logic/export.cpp
- [X] T011 [US1] Rework version-pinned fixtures to the v1 schema in tests/test_logic_yaml.cpp (remaining goldens: empty/tie/sequential round-trips), tests/test_cli_args.cpp (save goldens, refusal strings), tests/test_logic_seq_controls.cpp, and tests/test_logic_seq_cli_step.cpp (inline `version:` fixtures)
- [X] T012 [P] [US1] Update save/load usage strings to version 1 with required sections in include/wengine/cli/logic_registry.hpp

**Checkpoint**: US1 fully functional — quickstart §1 green, full ctest green, zero new warnings. MVP shippable.

---

## Phase 4: User Story 2 — Reopened initial states take effect on power-on (Priority: P2→P1)

**Goal**: File inits drive actual power-on state in session and engine runs; stepping evolves deterministically from stored values; missing inits default Low.

**Independent Test**: quickstart.md §§2, 6 — first `peek` after load shows stored mixed Q before any advance; 3-step trajectory matches the pre-save run; `sim run --seq-file` reproduces the session trajectory.

**Depends on**: US1 (loader + session apply).

### Tests for User Story 2 (governance-required — write FIRST, ensure engine parts FAIL before T014)

- [X] T013 [US2] Add power-on readback tests in tests/test_logic_seq_cli_peek.cpp (loaded mixed inits visible before any advance; missing init reads Low) and engine-honors-inits tests via direct section construction in tests/test_logic_seq_runner.cpp (pool powers to section inits; default-constructed section stays all-Low)

### Implementation for User Story 2

- [X] T014 [US2] Apply section inits to the run pool post-sync, pre-first-step in the logic branch of include/wengine/solvers/configured_run.hpp
- [X] T015 [P] [US2] Add repeated load-plus-run determinism test proving identical state readbacks on every repetition in tests/test_determinism.cpp

**Checkpoint**: US1 + US2 work — power-on fidelity proven in session and engine; suite green.

---

## Phase 5: User Story 3 — Named clock domains survive the round-trip (Priority: P1)

**Goal**: Prove domain pinning: mismatched `clocks:` metadata is refused, and selective stepping by domain name behaves identically before save and after load.

**Independent Test**: quickstart.md §3 — reload two-clock net, step one domain ×3 (other holds), step-all parity with pre-save behavior.

**Depends on**: US1 (loader + derivation).

### Tests for User Story 3 (governance-required)

- [X] T016 [US3] Add `clocks:`-mismatch refusal tests in tests/test_logic_yaml.cpp covering every contract case verbatim (unknown name, duplicate name, unresolvable source, empty-domain entry, missing entry for a derived domain, extra entry for no derived domain — each naming the domain)
- [X] T017 [P] [US3] Add selective-step-after-reload test in tests/test_logic_seq_cli_step.cpp (two-clock net file: step one domain by name ×3 advances it while the other holds; step-all advances both)
- [X] T018 [P] [US3] Add member-selection parity test in tests/test_logic_seq_runner.cpp (derived domains flatten to member slots; unselected domain holds across engine steps)

**Checkpoint**: US1–US3 work — domain fidelity proven on both session and engine paths; suite green.

---

## Phase 6: User Story 4 — Legacy v1/v2 files refused loudly, never half-loaded (Priority: P2)

**Goal**: Refusal quality plus provable zero legacy trace: diagnostics name versions/gates/domains, and no fallback, fixture, shipped file, or doc reference to loading legacy remains.

**Independent Test**: quickstart.md §4 + SC-007 — legacy samples refused with version-naming diagnostics and untouched workspace; repo-wide inspection finds no legacy support.

**Depends on**: US1 (refusing loader).

- [X] T019 [US4] Strengthen refusal diagnostics tests in tests/test_logic_yaml.cpp asserting message content names the found version, and bad-init/bad-clocks messages name the offending gate/domain
- [X] T020 [P] [US4] Eliminate remaining legacy fixtures and expectations across tests/ (grep for legacy markers; convert or delete; no test may load a legacy file expectantly)
- [X] T021 [P] [US4] Perform the repo-wide no-trace sweep over src/, include/, tests/, examples/, docs/, and README.md (remove fallback code, `1..2`/want-2 acceptance wording, and stale version-2 schema statements; only the version-naming refusal plus neutral history may remain)

**Checkpoint**: US1–US4 work — breaking change is clean, loud, and total; suite green.

---

## Phase 7: User Story 5 — Example library fully converted, plus three advanced references (Priority: P2)

**Goal**: All eight shipped files loadable in v1 with documented Run/Expect headers: five converted combinational examples plus three new advanced sequential references.

**Independent Test**: quickstart.md §5 — each of the eight files loads fresh and produces its documented output first attempt; re-save is stable.

**Depends on**: US1 (v1 format).

- [X] T022 [US5] Convert the five shipped examples in examples/logic/ to the v1 schema (nand_pair.yaml, fanout.yaml, parity3.yaml, tieoff.yaml, tied_demorgan.yaml: `version: 1`, required sections, `init:` keys on any sequential rows, accurate Run/Expect headers)
- [X] T023 [P] [US5] Author advanced example A in examples/logic/: two-clock-domain counter with mixed initial states (header Run/Expect proving selective stepping)
- [X] T024 [P] [US5] Author advanced example B in examples/logic/: enable-gated full-control-set counter with asynchronous reset/set (header Run/Expect proving control persistence)
- [X] T025 [P] [US5] Author advanced example C in examples/logic/: mixed JK/T divider stages with level-sensitive latch logic (header Run/Expect proving all-kind behavior after reload)
- [X] T026 [US5] Extend the shipped-examples pin test in tests/test_logic_yaml.cpp to all eight files (load, documented goldens, save/load/save stability)
- [X] T027 [US5] Update docs/logic.md (§9 schema with `init`/`clocks:`, canonical bytes, refusal table; §12 schema-evolution entry retiring legacy with no fallback) and the workspace paragraph in README.md to the v1 schema

**Checkpoint**: All user stories complete — reference corpus is the future test bed; suite green.

---

## Phase 8: Polish & Cross-Cutting Concerns

**Purpose**: Final verification across all stories.

- [X] T028 Run the full ctest suite with --output-on-failure from the repo root and confirm zero new compiler warnings
- [X] T029 [P] Run the complete quickstart.md validation end-to-end (scenarios §§1–6) and confirm every expected outcome
- [X] T030 [P] Final review pass against the constitution completion checklist: no unrelated files changed, architecture/dependency rules preserved, determinism considered, docs updated, no unrequested git actions, residual uncertainty reported

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies — starts immediately.
- **Foundational (Phase 2)**: Depends on Setup — BLOCKS all user stories.
- **User stories (Phase 3+)**: All depend on Foundational; additionally US2/US3/US4/US5 depend on US1's loader/saver (documented per phase, not just screen order).
  - After US1, US2/US3/US4/US5 may proceed in parallel if staffed (different files except noted test-file overlaps: coordinate T016/T019/T026/T006/T011, all in tests/test_logic_yaml.cpp).
- **Polish (Phase 8)**: Depends on all desired user stories being complete.

### User Story Dependencies

- **US1 (P1)**: After Foundational only — MVP.
- **US2 (P1)**: After US1 (needs bundle loader + session apply).
- **US3 (P1)**: After US1 (needs `clocks:` validation + derivation).
- **US4 (P2)**: After US1 (needs refusing loader to test against and sweep).
- **US5 (P2)**: After US1 (needs v1 format for files).

### Within Each Story

- Governance-required tests written FIRST and observed to FAIL before the implementing task (T006→T007, T013→T014, T016–T018 prove refusals/selection against the US1 loader).
- Headers/types before TU bodies before callers (T003–T005 → T007 → T008–T010).
- Story checkpoint green before the next priority starts.

### Parallel Opportunities

- Phase 2: T003, T004, T005 all [P] (three different headers).
- US1: T007 (yaml TU), T008 (common.cpp), T010 (sim.cpp/export.cpp), T012 (registry header) are [P] with each other; T009 follows T008 (same file); T011 follows T007 (needs failing-then-passing fixtures).
- US2: T015 [P] with T014 (tests/test_determinism.cpp vs configured_run.hpp).
- US3: T017 and T018 [P] (different test files); T016 touches test_logic_yaml.cpp — sequence after US1/US4 edits to that file if parallel staffed.
- US4: T020 and T021 [P] (tests vs repo sweep).
- US5: T023, T024, T025 [P] (three new files); T022 (conversions) independent file set — also parallelizable with the new files.
- Polish: T029 and T030 [P].

---

## Parallel Example: Foundational + US1 kickoff

```bash
# Phase 2 — all three headers together (different files, no interdependency):
Task: "Create pure engine-owned clock-domain derivation in include/wengine/logic/clock_domains.hpp"
Task: "Add plain-data Workspace bundle struct plus load/save_workspace declarations in include/wengine/logic/yaml_store.hpp"
Task: "Add per-slot power-on init field with all-Low default to SequentialRunConfig in include/wengine/logic/seq_config.hpp"

# Phase 3 — after T006 tests fail as required, launch the independent implementations together:
Task: "Implement save_workspace/load_workspace v1 schema in src/logic/yaml_store.cpp"
Task: "Replace the session ClockDomain derivation with the engine header in src/cli/commands/logic/common.cpp"
Task: "Convert remaining production callers to the bundle API in src/cli/commands/sim.cpp and src/cli/commands/logic/export.cpp"
Task: "Update save/load usage strings to version 1 in include/wengine/cli/logic_registry.hpp"
```

---

## Implementation Strategy

### MVP First (US1 Only)

1. Complete Phase 1: Setup (T001–T002).
2. Complete Phase 2: Foundational (T003–T005) — CRITICAL, blocks everything.
3. Complete Phase 3: US1 (T006–T012) — full v1 round-trip in session.
4. **STOP and VALIDATE**: quickstart §1 independently (save → reload → `cmp`-identical).
5. Demo-ready: lossless persistence works; inits/domains round-trip in-file.

### Incremental Delivery

1. Setup + Foundational → shared types ready.
2. + US1 → MVP: persist/reopen proven (demos SC-001/SC-002).
3. + US2 → power-on fidelity + engine parity (demos SC-005).
4. + US3 → domain fidelity proven (demos SC-003).
5. + US4 → breaking change clean and total (demos SC-004/SC-007).
6. + US5 → reference corpus + docs (every shipped file a regression pin).
7. Each increment keeps `ctest` green and warning-free.

### Parallel Team Strategy

With multiple developers: team completes Setup + Foundational together; then Developer A takes US2, B takes US3, C takes US4/US5 (coordinate the shared test_logic_yaml.cpp edits: T006/T011 landed in US1, T016 in US3, T019 in US4, T026 in US5 — serialize or split by test function).

---

## Notes

- [P] tasks = different files, no dependencies — verified per task above.
- [Story] label maps each story-phase task to its spec user story for traceability.
- Test tasks are governance-required (Constitution III), not optional, for this engine change.
- Stop at any checkpoint to validate the story independently via its quickstart scenario.
- Avoid: vague tasks, same-file parallel edits, cross-story behavior dependencies beyond the documented US1 foundation.
- No commit/push/PR/branch actions are included (not requested; per repo git discipline).
