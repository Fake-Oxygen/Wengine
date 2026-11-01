# Tasks: DFF Control Set

**Input**: Design documents from `/specs/002-dff-control-set/`

**Prerequisites**: plan.md (required), spec.md (required for user stories), research.md, data-model.md, contracts/

**Tests**: Included per constitution Principle III (independent-reference
testing is NON-NEGOTIABLE for engine/CLI behavior; goldens are the
hand-derived truth table in research.md R3, never the implementation
under test).

**Organization**: Tasks grouped by user story; each story is an
independently testable increment on top of the foundational pin + engine
core.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3)
- Include exact file paths in descriptions

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Baseline gate before any change

- [X] T001 Verify clean baseline in repo root: `cmake -S . -B build -DBUILD_TESTS=ON && cmake --build build && ctest --test-dir build --output-on-failure` passes with zero warnings

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Pin vocabulary + priority engine core + persistence. MUST be
complete before ANY user story session work begins. No user-visible
behavior change yet for D-only netlists (dangling-inactive defaults).

**⚠️ CRITICAL**: No user story work can begin until this phase is complete

- [X] T002 DFF 5-pin vocabulary in `include/wengine/logic/netlist.hpp`: `valid_fanin(kDff)` accepts 2 or 5 (`default_fanin` stays 2); `parse_pin` accepts case-insensitive `RST`/`SET`/`EN` on DFF at indices 2/3/4 with numeric aliases `.3`/`.4`/`.5`; DLATCH pins unchanged
- [X] T003 YAML 2|5 support in `src/logic/yaml_store.cpp`: loader accepts DFF `inputs: 2` (pins 2–4 dangling-inactive fallback) and `inputs: 5` with `ignore:` indices over 1..5; writer emits canonical `RST`/`SET`/`EN` names via `pin_text` and `inputs: 5`
- [X] T004 [P] Priority resolver + snapshot in `include/wengine/logic/seq_step.hpp`: new `noexcept` allocation-free helper `apply_async_controls(nl, st, block_inputs)` (slot order; `RST`→Q=0, else `SET`→Q=1; global across step-member subsets); `seq_step_filtered` snapshot resolves `RST`/`SET`/`EN` per flop in slot order per truth table `RST(1,*,*,*)→0; (0,1,*,*)→1; (0,0,0,*)→hold; (0,0,1,d)→d`, X/Z on any control fails loudly without committing
- [X] T005 Engine truth-table unit tests in `tests/test_logic_seq_controls.cpp` (new file, `test_x: OK` plain-assert, links `wengine_test_assert`): every row of the research.md R3 table asserted with `==`, plus dangling-default rows (`inputs: 2` flop ≡ all-inactive) and `register with add_executable` + `add_test` in `tests/CMakeLists.txt`
- [X] T006 Rebuild + narrowest-tests gate in repo root: new engine tests plus existing `test_logic_seq_*` suites pass (proves zero-migration at engine level)

**Checkpoint**: Foundation ready — pins parse, priority resolves in engine, legacy netlists untouched; session wiring can now begin

---

## Phase 3: User Story 1 - Reset to a known state (Priority: P1) 🎯 MVP

**Goal**: Async `RST` works end-to-end: assert → Q=0 at once, holds while
asserted, release → normal capture resumes

**Independent Test**: Quickstart §1 — build 2-flop circuit, step to Q=1,
`poke DFFn.RST=1`, `peek` reads Q=0/NQ=1 with zero advances, release,
`step 1` counts from zero; replay twice byte-identically

### Implementation for User Story 1

- [X] T007 [US1] Session async-apply wiring in `src/cli/commands/logic/poke.cpp`, `src/cli/commands/logic/step.cpp` (plus wire/tie/load mutation paths): call `apply_async_controls` after every control-net mutation and at top of `step` before edge looping; cycle counters unchanged by async-only activity; forced transitions reuse the existing `DFF1.Q: a->b` report form
- [X] T008 [P] [US1] CLI reset scenario tests in `tests/test_logic_seq_controls.cpp`: quickstart §1 flow, multi-flop reset-hold-release, `EN`-held + `RST` conflict (async wins), release-then-capture resumption
- [X] T009 [P] [US1] YAML RST round-trip test in `tests/test_logic_seq_controls.cpp`: save workspace with `DFF1.RST` wired + `inputs: 5`, reload, re-assert, identical readback; legacy `inputs: 2` file loads with reset inactive

**Checkpoint**: At this point, User Story 1 is fully functional and testable independently (resettable counters/FSMs with zero clocking)

---

## Phase 4: User Story 2 - Preset a flop to 1 (Priority: P2)

**Goal**: Async `SET` works end-to-end: assert → Q=1 at once, `RST`+`SET`
→ Q=0, release latches nothing

**Independent Test**: Quickstart §2 + §4 — `poke SET=1` on idle flop
reads Q=1 with no step; release + `step 1` with D=0 returns Q=0;
`RST`+`SET` reads Q=0, releasing `RST` flips to Q=1 with no advance

### Implementation for User Story 2

- [X] T010 [US2] CLI preset scenario tests in `tests/test_logic_seq_controls.cpp`: quickstart §2 seed flow, `SET`-hold across `step N` (Q stays 1), 3-flop counter-seed via `SET` then counting from seed
- [X] T011 [P] [US2] Simultaneous-assertion tests in `tests/test_logic_seq_controls.cpp`: `RST`+`SET` → Q=0 on REPL/pipe/script spellings with identical readback; `NQ` live-negation after every async force (`peek` shows `Q=1 NQ=0` and `Q=0 NQ=1`)

**Checkpoint**: At this point, User Stories 1 AND 2 both work independently (reset + seedable preset)

---

## Phase 5: User Story 3 - Stall a pipeline (Priority: P2)

**Goal**: `EN=0` holds Q across edges while enabled siblings capture;
selective hold composes with clock-domain subsets

**Independent Test**: Quickstart §3 — two flops, `EN=0` on one with
toggling D, `step 3` leaves held flop at Q=0 (no-change lines) while
sibling captures Q=1; `peek` confirms

### Implementation for User Story 3

- [X] T012 [US3] Hold reporting in `src/cli/commands/logic/step.cpp`: `EN=0` flops emit existing no-change line shapes (`DFF1.Q: v (no change)`) per edge while siblings report captures; held flops excluded from transition diffs
- [X] T013 [US3] CLI stall scenario tests in `tests/test_logic_seq_controls.cpp`: quickstart §3 flow, 5-edge freeze (SC-002), `EN` release mid-sequence resumes capture next edge, dangling-`EN` flop always captures (enabled default)
- [X] T014 [P] [US3] Domain-subset × enable interaction test in `tests/test_logic_seq_controls.cpp`: stepped subset with an `EN=0` member holds while other members capture; non-members hold regardless of `EN` (existing subset rule preserved)

**Checkpoint**: All three control stories independently functional (reset, preset, stall)

---

## Phase 6: User Story 4 - Documented priority rule (Priority: P3)

**Goal**: One priority sentence visible in help/session docs and proven
by a conflict-matrix test; user docs updated

**Independent Test**: Quickstart §4 on all three surfaces (REPL, script
file, pipe) with identical readback for every conflicting combination

### Implementation for User Story 4

- [X] T015 [US4] Help + completion in `include/wengine/cli/logic_registry.hpp` and `src/cli/commands/logic/common.cpp`: pin table (`RST`/`SET`/`EN` + active levels + dangling defaults) and the normative sentence `RST (async) > SET (async) > EN-hold (sync) > D; RST+SET -> Q=0`; completion suggests new pin names after `DFFn.`
- [X] T016 [P] [US4] Full conflict-matrix CLI test in `tests/test_logic_seq_controls.cpp`: all 8 `RST`×`SET`×`EN` combinations × D-toggle × both Q_old values against the research.md R3 table, identical output across session/script/pipe harness spellings
- [X] T017 [P] [US4] `docs/logic.md` update: priority statement, 5-pin table with active levels and dangling defaults, async-vs-sync split, global-async vs per-member-sync scoping rule, DLATCH-unchanged note
- [X] T018 [US4] `README.md` update: Run example (reset + stall), module table entry, test-list entry for `test_logic_seq_controls`

**Checkpoint**: Priority rule is user-visible, machine-proven, and documented; all user stories independently functional

---

## Phase 7: Polish & Cross-Cutting Concerns

**Purpose**: Gates, determinism proof, and review hygiene

- [X] T019 Determinism replay test in `tests/test_logic_seq_controls.cpp`: scripted 10-step reset→preset→stall→advance→readback sequence (SC-005) replayed twice byte-identically, including async assertion between advances
- [X] T020 [P] Quickstart validation: execute all 5 scenarios in `specs/002-dff-control-set/quickstart.md` headlessly and check every expected outcome + checklist box
- [X] T021 Full suite + hygiene gate in repo root: `ctest --test-dir build --output-on-failure` green, zero new compiler warnings, registry-size `static_assert`s intact (no new commands added), `git status` shows only intended files, no commits/pushes
- [X] T022 Constitution compliance review: correctness (goldens independent of implementation), determinism (slot-order + zero-migration), dependency direction (cli→solvers only), docs living-spec updated, residual risks noted

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies — starts immediately; abort on red baseline
- **Foundational (Phase 2)**: Depends on Setup — BLOCKS all user stories
- **User Stories (Phases 3–6)**: All depend on Foundational completion
  - Recommended order P1 → P2 → P3 (US1 → US2/US3 → US4); US2 and US3 are mutually independent once Foundational + US1's session wiring (T007) lands — T007 is the one cross-story dependency
  - US4 (docs/help/matrix) can start after US1–US3 behavior exists
- **Polish (Phase 7)**: Depends on all desired stories being complete

### User Story Dependencies

- **User Story 1 (P1)**: After Foundational — no dependencies on other stories
- **User Story 2 (P2)**: After Foundational + T007 (shared session wiring) — engine `SET` rows already in T005; story adds session proof
- **User Story 3 (P2)**: After Foundational + T007 — engine `EN` rows already in T005; story adds hold reporting + session proof
- **User Story 4 (P3)**: After US1–US3 behavior — documents and matrix-tests the shipped behavior

### Within Each User Story

- Engine/tests-first where applicable (T005 truth table precedes session wiring that depends on it)
- Core implementation before reporting/docs within the story
- Story checkpoint validated before moving to next priority

### Parallel Opportunities

- T003 + T004 can run in parallel (different files, both depend only on T002)
- T008 + T009 in parallel; T010 + T011 in parallel (same-file appends — serialize if harness conflicts, otherwise independent cases)
- T013 + T014 in parallel; T016 + T017 in parallel
- US2 and US3 can proceed in parallel after T007 (different concerns, shared engine already landed)

---

## Parallel Example: Foundational

```bash
# After T002 lands, launch engine + persistence in parallel:
Task: "Priority resolver + snapshot in include/wengine/logic/seq_step.hpp [T004]"
Task: "YAML 2|5 support in src/logic/yaml_store.cpp [T003]"
```

## Parallel Example: User Story 1

```bash
# Launch CLI scenario + round-trip tests together:
Task: "CLI reset scenario tests in tests/test_logic_seq_controls.cpp [T008]"
Task: "YAML RST round-trip test in tests/test_logic_seq_controls.cpp [T009]"
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1: Setup (T001 baseline green)
2. Complete Phase 2: Foundational (T002–T006; pins + resolver + engine goldens)
3. Complete Phase 3: User Story 1 (T007–T009 session wiring + reset proof)
4. **STOP and VALIDATE**: Quickstart §1 + §5 (migration replay) independently
5. Demo: resettable counter from any state with zero clocking

### Incremental Delivery

1. Setup + Foundational → engine resolves full priority, old outputs identical
2. Add US1 → resettable FSMs/counters (MVP!)
3. Add US2 → seedable counters, one-hot FSM entry
4. Add US3 → pipeline stalls, per-stage holds
5. Add US4 → documented, matrix-proven priority rule
6. Polish → determinism proof, full gates green

---

## Notes

- [P] tasks = different files, no dependencies (same-file test appends: prefer serial appends to avoid merge churn when one author)
- [Story] label maps task to user story for traceability (US1–US4 ↔ spec stories 1–4)
- T007 is the single shared session-wiring task intentionally placed in US1 (P1) so US2/US3 stay small and test-focused
- No new CLI commands → no registry-size `static_assert` changes expected (T021 verifies)
- No YAML schema bump (`version: 2` retained; T003 loader accepts both counts)
- No commits, pushes, or branches in any task (per project discipline; T021 asserts clean status only)
