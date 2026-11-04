# Tasks: Sequential Nets Runner

**Input**: Design documents from `/specs/005-sequential-nets-runner/` (spec.md, plan.md, research.md, data-model.md, contracts/, quickstart.md)

**Prerequisites**: plan.md (required), spec.md (required for user stories), research.md, data-model.md, contracts/

**Tests**: Included — required by the feature's own testability contract (spec acceptance scenarios per story), constitution Principle III (independent-reference testing), and the AGENTS.md stepped-solver recipe (new solvers ship reference-anchored tests; every registry-size `static_assert` updated). Test tasks write goldens FIRST against hand-derived references, then implementation.

**Organization**: Tasks grouped by user story; each story is an independently testable increment.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2)
- Include exact file paths in descriptions

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Baseline before any code changes

- [X] T001 Verify clean baseline: `cmake -S . -B build -DBUILD_TESTS=ON && cmake --build build && ctest --test-dir build --output-on-failure` green before changes

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Shared types, State pool, snapshot rows, v6 checkpoints, solver + registry + section member — compiles, no driver behavior yet

**⚠️ CRITICAL**: No user story work can begin until this phase is complete

- [X] T002 [P] Create `SequentialRunConfig` struct in `include/wengine/logic/seq_config.hpp` (netlist, stimulus, members, step_all_domains default true, set flag; parsed values only, no strings)
- [X] T003 [P] Add sequential pool + snapshot rows in `include/wengine/world/state.hpp` (State: netlist, seq, seq_inputs, seq_members, seq_outputs + clear()/==; Snapshot: seq_serial/seq_value/seq_output + fill_snapshot reuse path)
- [X] T004 [P] Add format-v6 seq columns + v1–v5 fallback loaders in `include/wengine/world/serialize.hpp` (fixed-stride topology + Q + stimulus + members; derived fanouts/topo recomputed, never stored)
- [X] T005 [P] Create header-only `SequentialSolver` (`kName="logic"`, create, dt-validated end-of-step `step()` via `seq_step_filtered`) in `include/wengine/solvers/sequential.hpp` and register it in `include/wengine/solvers/solver_registry.hpp`
- [X] T006 [P] Add defaulted `operator==` to `Gate`/`Netlist` in `include/wengine/logic/types.hpp` + `include/wengine/logic/netlist.hpp` and to `SeqState` in `include/wengine/logic/seq_state.hpp`; aggregate `logic::SequentialRunConfig seq` into `core::RunConfig` in `include/wengine/core/run_config.hpp`
- [X] T007 Compile gate: build all core targets so T002–T006 type changes compile with zero new warnings before driver work

**Checkpoint**: Foundation ready — types compile, solver registered, State carries the pool; user story implementation can now begin

---

## Phase 3: User Story 1 — Run a sequential net for N fixed steps and replay it identically (Priority: P1) 🎯 MVP

**Goal**: Deterministic batch stepping through `core::Runner` with the default all-domains path; identical runs produce byte-identical results

**Independent Test**: Build a DFF counter / T divider net, run N steps via `run_headless` with the logic section, record readback, re-run identical config and confirm identical `seq_serial`/`seq_value`/`seq_output` (delivers deterministic batch stepping with no domain selection or sink plumbing)

- [X] T008 [US1] Implement the logic branch in `ConfiguredRun::build` in `include/wengine/solvers/configured_run.hpp` (set/solver gating both directions; `validate_sequential`; stimulus size Low/High check; populate State pools; step_all default path with empty members; error strings for each rejection)
- [X] T009 [US1] Add solver engine goldens in `tests/test_logic_seq_runner.cpp` (2-bit counter 00→01→10→11→00 over 4 steps, T-divider divide-by-2/4, latch-only net transparency/hold across steps, dt<=0/non-finite rejection, determinism replay — all vs hand-derived references)
- [X] T010 [US1] Add driver goldens in `tests/test_logic_seq_runner.cpp` (run_headless counter sequence; identical-config replay equality; steps<1 and bad-dt rejection; section-set-with-non-logic-solver and logic-without-section rejection; empty-sequential-net run)
- [X] T011 [US1] Update registry asserts to 3 + `find("logic")` in `tests/test_solver_registry.cpp` and the `SolverRegistry` size assert in `tests/test_signal.cpp`
- [X] T012 [US1] Register the new binary in `CMakeLists.txt` (`add_executable test_logic_seq_runner` + `target_link_libraries` + `add_test`)
- [X] T013 [US1] Build + run suites: new runner test plus `run_config`, `solver_registry`, `signal`, `runner`, `logic_seq_step` — all green, zero new warnings

**Checkpoint**: User Story 1 fully functional and independently testable — batch runs work end-to-end via `run_headless`

---

## Phase 4: User Story 2 — Select which clock(s) advance on a multi-clock net (Priority: P2→P1)

**Goal**: One named domain, all domains, or explicit subset per step; unselected flops hold while combinational outputs still settle

**Independent Test**: Two-independent-clock net, N steps with only the first domain selected → first domain advanced N times, second held; all-domains → both advanced (delivers per-domain selectivity with no sink involvement)

- [X] T014 [US2] Implement member-list validation in the logic build arm in `include/wengine/solvers/configured_run.hpp` (step_all/members consistency both directions; every member a live DFF/JK/T slot; no duplicates; empty selection rejected; members retained into `State::seq_members`)
- [X] T015 [US2] Add selection goldens in `tests/test_logic_seq_runner.cpp` (two-domain selective advance/hold over 3 steps; all-domains advance; dangling-CLK implicit-domain membership; member validation rejections with state untouched)
- [X] T016 [US2] Build + run suites: runner test plus `logic_seq_step`, `run_config` — all green, zero new warnings

**Checkpoint**: User Stories 1 AND 2 both work — default and selective stepping verified independently

---

## Phase 5: User Story 3 — Observe sequential state through snapshots and sinks (Priority: P2)

**Goal**: Per-step stored values + settled outputs in Snapshot, Frame channels, and CSV artifacts with identical values across surfaces

**Independent Test**: Short run with recording enabled → recording entries match live `take_snapshot` readback step-for-step; replay → identical recording (delivers observable engine runs with no new recording format)

- [X] T017 [P] [US3] Add `frame_from_logic_snapshot` adapter (`seq.count` + `seq.<serial>.Q` channels in slot order, no items) in `include/wengine/link/frame.hpp`
- [X] T018 [P] [US3] Add the `{"logic", ...}` frame-source registry entry in `apps/gui/views/frame_source.hpp` (views untouched)
- [X] T019 [US3] Create `LogicCsvSink` (header `step,time,seq_id,value`, `max_digits10`, sticky never-throw status) in `include/wengine/emit/logic_sink.hpp`
- [X] T020 [US3] Wire logic emit dispatch in `src/cli/commands/sim.cpp` (CSV sink selection + SHM `FrameAdapter` selection for solver logic; 1+N≤64 over-bound loud refusal with per-cause counting, mirroring the signal path)
- [X] T021 [US3] Add observation goldens in `tests/test_logic_seq_runner.cpp` (fill_snapshot slot order + serial stability across recycle; take_snapshot parity; CSV rows + re-read `==`; mid-run snapshot equality across replay)
- [X] T022 [US3] Update frame-source asserts to 3 + logic adapter channel goldens in `tests/test_frame_source.cpp`
- [X] T023 [US3] Build + run suites: runner, frame_source, sim, snapshot/serialize/emit suites — all green, zero new warnings

**Checkpoint**: All observation surfaces verified — snapshots, Frame, CSV agree entry-for-entry

---

## Phase 6: User Story 4 — Carry stimulus, controls, and legality rules into the engine path (Priority: P2)

**Goal**: `sim run --solver logic` with `--seq-file/--seq-clocks/--seq-from`; async priority, level settle, and illegal-combination/X-Z refusal identical to interactive stepping

**Independent Test**: Async-reset-forced run reads reset regardless of clocks; illegal-combination run fails loudly with time/index frozen and state unchanged; `--seq-clocks nope` exits 2 naming the domain (delivers semantic parity with no new verbs)

- [X] T024 [US4] Implement `--seq-file/--seq-clocks/--seq-from` parsing + name→slot resolution + usage errors (exit 2) + `complete()` coverage in `src/cli/commands/sim.cpp` (YAML via existing `logic::yaml::load_text`; orphan `--seq-*` rejected for non-logic solvers; stimulus order matches `compose_block_inputs`)
- [X] T025 [US4] Add refusal/control goldens in `tests/test_logic_seq_runner.cpp` (mid-run S=R=1 abort: step index frozen, prior entries intact, pool untouched; X/Z stimulus refusal; RST>SET async priority vs clocks; EN-defaults-High stimulus parity)
- [X] T026 [P] [US4] Add CLI goldens in `tests/test_cli_args.cpp` (sim run logic happy path; usage/stray/orphan-option exits 2; unknown-domain and empty-selection errors; completion for the three flags)
- [X] T027 [US4] Build + run suites: cli_args, sim, runner, routing suites — all green, zero new warnings

**Checkpoint**: All user stories independently functional — engine, selection, observation, and CLI parity complete

---

## Phase 7: Polish & Cross-Cutting Concerns

**Purpose**: Docs, full-suite gate, end-to-end validation

- [X] T028 Update `README.md` (sim run logic example, solvers module-table row, test-list entry for `test_logic_seq_runner`)
- [X] T029 Update the shipped-future-work note in `docs/logic.md` §12 (`SequentialSolver` + `RunConfig` section no longer future work) and run `quickstart.md` scenarios 1–4 end-to-end
- [X] T030 Full gate: `cmake --build build && ctest --test-dir build --output-on-failure` green with zero new warnings; report remaining uncertainty

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies — T001 starts immediately
- **Foundational (Phase 2)**: Depends on Setup — T002–T006 [P] in parallel (different files), T007 compile gate after all land. **BLOCKS all user stories**
- **User Stories**: US1 → US2 sequential (same build-arm function + same test file); US3 needs only Foundational + US1 driver (adapter/sink/CSV independent of selection — may overlap US2 only if editing different files: T017/T018/T019 are disjoint files, parallel-safe once US1 lands); US4 needs Foundational + US1/US2 (CLI resolution builds on member semantics). Recommended order: P1 US1 → P1 US2 → P2 US3 → P2 US4
- **Polish**: Depends on all stories complete

### Within Each User Story

- Goldens reference hand-derived values, written before/with implementation per repo convention (assert-first, never widen tolerances)
- Same-file tasks run sequentially (T009→T010, T015 extends that file, T021/T025 extend it further)
- Build + suite gate closes each story phase

### Parallel Opportunities

- Phase 2: T002, T003, T004, T005, T006 in parallel (5 disjoint headers); T007 after
- Phase 5: T017 (frame.hpp) + T018 (frame_source.hpp) in parallel (disjoint files)
- Phase 6: T026 (test_cli_args.cpp) parallel with T025 (test_logic_seq_runner.cpp) once T024 lands
- Stories US3 vs US2 file sets are disjoint except the shared test file — coordinate same-file edits sequentially

---

## Parallel Example: Foundational Phase

```bash
# Launch disjoint header tasks together (different files, no dependencies):
Task: "Create SequentialRunConfig in include/wengine/logic/seq_config.hpp"        # T002
Task: "Add State pool + snapshot rows in include/wengine/world/state.hpp"          # T003
Task: "Add v6 columns + fallbacks in include/wengine/world/serialize.hpp"         # T004
Task: "Create SequentialSolver + registry entry (sequential.hpp, solver_registry.hpp)"  # T005
Task: "Add == operators + RunConfig member (types/netlist/seq_state/run_config)" # T006
# Then: T007 compile gate
```

## Parallel Example: User Story 3

```bash
# Disjoint files once US1 driver lands:
Task: "Add frame_from_logic_snapshot in include/wengine/link/frame.hpp"  # T017
Task: "Add {logic,...} registry entry in apps/gui/views/frame_source.hpp"  # T018
```

---

## Implementation Strategy

### MVP First (Foundational + User Story 1)

1. Complete Phase 1: Setup (T001 baseline)
2. Complete Phase 2: Foundational (T002–T007 — types, pool, snapshot, v6, solver, section member)
3. Complete Phase 3: User Story 1 (T008–T013 — build arm + goldens + registry asserts)
4. **STOP and VALIDATE**: `run_headless` logic runs deterministic via `test_logic_seq_runner`; replay identical
5. MVP usable programmatically (CLI arrives in US4)

### Incremental Delivery

1. Setup + Foundational → compiles, solver registered
2. + US1 → deterministic batch stepping (MVP)
3. + US2 → multi-clock selectivity
4. + US3 → snapshots/Frame/CSV observation
5. + US4 → `sim run --solver logic` user surface
6. + Polish → docs + full gate; each increment keeps prior suites green

---

## Notes

- [P] tasks = different files, no dependencies — safe to parallelize
- [USn] label maps each story-phase task to its spec story for traceability
- Same-file edits (configured_run.hpp: T008→T014; test_logic_seq_runner.cpp: T009→T010→T015→T021→T025; sim.cpp: T020→T024) are intentionally sequential
- Determinism rule: slot order everywhere, serials never reused, static stimulus, end-of-step value-at-T+dt
- Failure rule: `kInvalidArgument`, pool untouched at the failing boundary, Runner latch freezes time/index, emit skipped
- Stop at any checkpoint to validate the story independently; do not batch completions across stories
