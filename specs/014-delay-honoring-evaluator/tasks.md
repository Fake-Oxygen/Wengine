# Tasks: Opt-In Delay-Honoring Evaluator for Glitch/Race Observation

**Input**: Design documents from `/specs/014-delay-honoring-evaluator/`

**Prerequisites**: plan.md (required), spec.md (required for user stories), research.md, data-model.md, contracts/

**Tests**: Tests ARE explicitly requested — spec FR-014 mandates independently derived trace goldens, CLI representation, byte-identity of pre-feature flows, and the unchanged bench suite; plan.md adds `tests/test_logic_timing.cpp` plus completion vectors and example-workspace replay pins.

**Organization**: Tasks are grouped by user story to enable independent implementation and testing of each story.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3)
- Include exact file paths in descriptions

## Path Conventions

- Single C++20 project: `include/` (headers), `src/` (TUs), `tests/`, `examples/`, `docs/`
- Engine modules are header-only `INTERFACE` (`wengine_logic`); NO new `.cpp` TU for the wheel
- CLI verbs live in `src/cli/commands/logic/`; usage text in `include/wengine/cli/commands.hpp`

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Verify baseline build, confirm pin locations, no behavior change

- [X] T001 Verify baseline build and test suite passes via `cmake -S . -B build -DBUILD_TESTS=ON && cmake --build build && ctest --test-dir build --output-on-failure`
- [X] T002 [P] Confirm registry-size pin location and value (27 verbs) in `tests/test_cli_args.cpp` and verb list in `include/wengine/cli/logic_registry.hpp`
- [X] T003 [P] Read current solve/step/truth verb implementations in `src/cli/commands/logic/solve.cpp`, `src/cli/commands/logic/step.cpp`, `src/cli/commands/logic/truth.cpp` and shared helper in `src/cli/commands/logic/common.hpp`
- [X] T004 [P] Read zero-delay evaluator and stepper seams to be left untouched in `include/wengine/logic/eval.hpp` and `include/wengine/logic/seq_step.hpp`

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Core timing wheel + shared print helper that ALL user stories depend on

**⚠️ CRITICAL**: No user story work can begin until this phase is complete

**Checkpoint**: Foundation ready — wheel compiles header-only, printer helper exists, usage text names the flag

### Engine wheel skeleton

- [X] T005 Create `include/wengine/logic/timing.hpp` with Doxygen `@file` role paragraph plus `TimingSeed`, `TimingEvent`, `TimingTrace` (fixed-capacity event array, `n_events`, `drain_ps`, `n_annotated`, `unit_ps=1`), `TimingScratch` structs per `specs/014-delay-honoring-evaluator/contracts/engine-seam.md`
- [X] T006 Implement 4-state dominant-value gate evaluation tables (AND/NAND, OR/NOR, XOR/XNOR, NOT/BUF, TBUF; Z-at-input reads as X) in `include/wengine/logic/timing.hpp` per `specs/014-delay-honoring-evaluator/contracts/timing-semantics.md` (IEEE 1364 reference, research D5)
- [X] T007 Implement transient-tolerant input resolution wrapper reusing shipped `resolve_net` funnel read-only in `include/wengine/logic/timing.hpp` (research D1/D5)
- [X] T008 Implement `run_timing_window` transport wheel in `include/wengine/logic/timing.hpp`: seeds at t=0, per-transition (gate, t+delay) scheduling with u64 checked adds, same-(gate,time) collapse, no-op silence, pinned (time, slot-order) event order with NQ-after-Q pairing, unit floor ≥1ps (research D2/D3/D4)
- [X] T009 Implement loud refusal paths in `include/wengine/logic/timing.hpp`: constant `kMaxTimingEvents` cap breach and u64 time-axis overflow as `core::Status` runtime failures naming the cap/shape, no partial trace (research D9, contracts/engine-seam.md failure vocabulary)
- [X] T010 [P] Register `test_logic_timing` target in `CMakeLists.txt` (`add_executable` + `target_link_libraries` + `add_test`, inherits `wengine_test_assert` policy via `test_` prefix; `wengine_logic` stays `INTERFACE`, no new TU)

### Shared CLI plumbing

- [X] T011 Add `--with-delays` spelling constant and shared timing-block printer helper (D8 grammar: `timing: unit 1ps, <K> annotated` header, `t=<T>ps <PIN>: <old>-><new>` lines in pinned order, `settled in <D>ps`) in `src/cli/commands/logic/common.hpp` and `src/cli/commands/logic/common.cpp`
- [X] T012 Add `--with-delays` to Solve/Step/Truth `kUsage` strings in `include/wengine/cli/commands.hpp` (text only; registry size unchanged at 27)

**Checkpoint**: `test_logic_timing` skeleton builds; printer helper unit-callable; flag-off output unchanged (no adapter wired yet)

---

## Phase 3: User Story 1 — See the hidden pulse (Priority: P1) 🎯 MVP

**Goal**: `solve --with-delays` runs the transport wheel over the applied assignment and prints the ordered event trace through transients to the drained settled state; settled values equal zero-delay (FR-002/FR-003/FR-004/FR-005)

**Independent Test**: Drive the hazard net (direct path + N-inverter chain into one gate) through timing-mode solve; assert pulse width/placement against hand-derived path-delay difference (never implementation readback) and settled values equal to zero-delay solve

### Tests for User Story 1

> **NOTE: Write these tests FIRST, ensure they FAIL before implementation**

- [X] T013 [P] [US1] Hand-derived combinational pulse goldens (3-inverter chain vs direct path: 3ps window width, placement at gate delay; annotated variant scaling) in `tests/test_logic_timing.cpp`
- [X] T014 [P] [US1] Transport-semantics test (overlapping pending output event still fires at its own offset — no inertial cancellation) in `tests/test_logic_timing.cpp`
- [X] T015 [P] [US1] Unit-floor tests (un-annotated gates take 1ps; annotation 0 means un-annotated; header names the unit) in `tests/test_logic_timing.cpp`
- [X] T016 [P] [US1] Same-picosecond ordering test (time first, then slot order; repeated runs byte-identical) in `tests/test_logic_timing.cpp`
- [X] T017 [P] [US1] Solve parity test (wheel end-state == zero-delay `solve()` for same assignment) in `tests/test_logic_timing.cpp`

### Implementation for User Story 1

- [X] T018 [US1] Implement `solve --with-delays` adapter in `src/cli/commands/logic/solve.cpp`: zero-delay solve of all-zero vector for pre-state, seed diff of applied assignment, `run_timing_window`, D8 trace block, then unchanged settled tail from ordinary `solve()`
- [X] T019 [P] [US1] Create annotated inverter-chain hazard demo with expected trace in header in `examples/logic/glitch_chain.yaml`
- [X] T020 [US1] CLI representation test for `solve --with-delays` block grammar (header, event lines, `settled in <D>ps`, empty-window `settled in 0ps`, byte-identical settled tail) in `tests/test_logic_timing.cpp`

**Checkpoint**: At this point, User Story 1 is fully functional and testable independently — `logic solve --in 1 --with-delays` on `examples/logic/glitch_chain.yaml` shows the hand-derived pulse

---

## Phase 4: User Story 2 — The default never moves (Priority: P1)

**Goal**: Every flag-off flow is byte-identical to pre-feature; default path reads no delay field, branches on no mode; `bench_logic` measures unchanged (FR-007)

**Independent Test**: Run every pre-feature golden, example replay, refusal test, and the bench suite asserting byte-identical outputs and unchanged measurements; evaluate same netlist with/without annotations in default mode asserting identical results

### Tests for User Story 2

- [X] T021 [P] [US2] Flag-off byte-identity tests for solve/step/truth (flag-off output equals pre-feature goldens; settled tail identical with flag on/off) in `tests/test_logic_timing.cpp`
- [X] T022 [P] [US2] Annotation-blindness test (same netlist with and without `propagation_delay_ps` annotations evaluates identically in default mode) in `tests/test_logic_timing.cpp`
- [X] T023 [US2] Verify `eval.hpp`/`seq_step.hpp`/`types.hpp`/`block_interface.hpp` gain zero code (diff check) and run unchanged `bench_logic` as no-regression proof via `ctest --test-dir build -R bench_logic --output-on-failure`

### Implementation for User Story 2

- [X] T024 [US2] Audit default code paths in `src/cli/commands/logic/solve.cpp`, `src/cli/commands/logic/step.cpp`, `src/cli/commands/logic/truth.cpp` to confirm flag-off path contains no delay reads and no mode branches (review-only; fix only if drift found)

**Checkpoint**: At this point, User Stories 1 AND 2 both hold — timing visible on demand, default provably untouched

---

## Phase 5: User Story 3 — Races at the sequential boundary (Priority: P2)

**Goal**: `step --with-delays` anchors each edge window at offset zero, reports Q/NQ at annotated clock-to-Q plus combinational ripples; trajectories match zero-delay step-for-step; transient S=R=1 renders X, settled S=R=1 refuses as today (FR-006/FR-009)

**Independent Test**: Step a clocked net with annotated clock-to-Q in both modes; assert identical state trajectories readback step-for-step plus trace arrival offsets hand-derived from clock-to-Q + downstream path delays

### Tests for User Story 3

- [X] T025 [P] [US3] Edge-flop clock-to-Q golden (Q transitions exactly annotation after edge; NQ shares the timestamp, never lags) in `tests/test_logic_timing.cpp`
- [X] T026 [P] [US3] D-arrival race-offset golden (hand-derived clock-to-Q + downstream path sum; sampled state equals zero-delay stepping) in `tests/test_logic_timing.cpp`
- [X] T027 [P] [US3] Level-latch response-delay test (mid-window net change lands at arrival + annotation; level rules unchanged) in `tests/test_logic_timing.cpp`
- [X] T028 [P] [US3] Transient 4-state window tests (AND(0,X)=0 no pulse; AND(1,X)=X pulse; TBUF released Z window; transient S=R=1 renders X while settled S=R=1 refuses) in `tests/test_logic_timing.cpp`
- [X] T029 [P] [US3] Step trajectory parity + determinism test (same step sequence equal across modes; repeated timing runs byte-identical) in `tests/test_logic_timing.cpp`
- [X] T030 [P] [US3] Refusal tests (event-cap breach on reconvergent annotated bomb; u64 overflow with near-cap annotation; settled X/Z refusal; workspace/state untouched) in `tests/test_logic_timing.cpp`

### Implementation for User Story 3

- [X] T031 [US3] Implement `step --with-delays` adapter in `src/cli/commands/logic/step.cpp`: unchanged async/level settle + `seq_step_filtered` edge loop first, per-edge Q-diff seeds (NQ shares Q timestamp), one window per edge, `edge k:` separators for `--cycles N`, unchanged settled tail
- [X] T032 [P] [US3] Create annotated clock-to-Q flop feeding D-path race demo with expected arrival offsets in header in `examples/logic/race_window.yaml`

**Checkpoint**: All combinational AND sequential stories independently functional — glitches and races observable, trajectories pinned

---

## Phase 6: User Story 4 — One vocabulary, deterministic by construction (Priority: P2)

**Goal**: `--with-delays` on existing verbs only (no new commands, registry stays 27); completion covers the flag; session/one-shot byte parity; per-invocation only, nothing persisted (FR-012/FR-013)

**Independent Test**: Drive same timing invocations through session and one-shot paths asserting byte parity; assert completion offers the flag; assert save/load round-trips and workspace schema untouched (no version bump)

### Tests for User Story 4

- [X] T033 [P] [US4] Completion vectors for `--with-delays` on solve/step/truth in `tests/test_cli_args.cpp` (registry-size `static_assert` at 27 unchanged)
- [X] T034 [P] [US4] Session vs one-shot byte-parity test for timing invocations in `tests/test_logic_timing.cpp`
- [X] T035 [P] [US4] No-persist test (workspace save/load round-trip byte-identical; no schema/version change; next invocation starts in default mode) in `tests/test_logic_timing.cpp`
- [X] T036 [P] [US4] Refusal-convention tests (misspelled flag → usage error exit 2 workspace untouched; wheel cap/overflow → runtime failure exit 1) in `tests/test_logic_timing.cpp`

### Implementation for User Story 4

- [X] T037 [US4] Wire `--with-delays` flag parsing via existing `take_flag` in `src/cli/commands/logic/solve.cpp`, `src/cli/commands/logic/step.cpp`, `src/cli/commands/logic/truth.cpp` with stray-argument usage-error path for unknown spellings
- [X] T038 [US4] Add `--with-delays` to each verb's completion option list in `src/cli/commands/logic/solve.cpp`, `src/cli/commands/logic/step.cpp`, `src/cli/commands/logic/truth.cpp` (or their `complete` specs)

**Checkpoint**: Vocabulary pinned — one flag, three verbs, no registry growth, parity across surfaces

---

## Phase 7: User Story 5 — Critical-path visibility on the truth surface (Priority: P3)

**Goal**: `truth --with-delays` streams rows identical in values to zero-delay truth plus per-row drain time (assignment's critical path in whole ps) (FR-011)

**Independent Test**: Stream a small net with known maximum path in timing mode; assert each row's drain time matches its hand-derived critical path while row values match zero-delay truth

### Tests for User Story 5

- [X] T039 [P] [US5] Truth drain-time goldens (each row's settle equals hand-derived longest activated annotated path; row values identical to flag-off truth) in `tests/test_logic_timing.cpp`
- [X] T040 [P] [US5] Truth refusal-inheritance test (shared attachments, live buffers refuse identically in timing mode) in `tests/test_logic_timing.cpp`

### Implementation for User Story 5

- [X] T041 [US5] Implement `truth --with-delays` adapter in `src/cli/commands/logic/truth.cpp`: per-row wheel (all-zero pre-vector, same row convention), appended `settle` column in pretty rows and trailing `settle_ps` column in `--csv` rows, unchanged `wrote N rows` summary

**Checkpoint**: All user stories independently functional — survey view (truth) joins observation views (solve/step)

---

## Phase 8: Polish & Cross-Cutting Concerns

**Purpose**: Documentation suite duties, catalog audits, full verification

- [X] T042 [P] Document `--with-delays` verb grammar in §10 and ship §12 Timing slot in `docs/logic.md`
- [X] T043 [P] Add `test_logic_timing` page (goldens, 4-state windows, refusals, parity, determinism guards) in `docs/tests/logic.md`
- [X] T044 [P] Update verb list with flag, runnable glitch/race examples, test list, and module-table wording in `README.md`
- [X] T045 Run catalog audit `python3 docs/audit_catalogs.py` (zero missing / zero ghost for new test and examples) and fix any gaps
- [X] T046 Run full suite `ctest --test-dir build --output-on-failure` including `test_logic_timing` and unchanged `bench_logic`
- [X] T047 Validate `specs/014-delay-honoring-evaluator/quickstart.md` scenarios end-to-end (SC-001 glitch, SC-003 race, SC-002 default-identity, SC-004 determinism, truth settle times)

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies — can start immediately
- **Foundational (Phase 2)**: Depends on Setup completion — BLOCKS all user stories
- **User Stories (Phase 3+)**: All depend on Foundational phase completion
  - User stories can then proceed in parallel (if staffed)
  - Or sequentially in priority order (US1 → US2 → US3 → US4 → US5)
- **Polish (Final Phase)**: Depends on all desired user stories being complete

### User Story Dependencies

- **User Story 1 (P1)**: Can start after Foundational (Phase 2) — No dependencies on other stories
- **User Story 2 (P1)**: Can start after Foundational (Phase 2) — Behavioral pins over US1's code; independently testable via flag-off flows
- **User Story 3 (P2)**: Can start after Foundational (Phase 2) — Rides US1's wheel; independently testable via step adapter
- **User Story 4 (P2)**: Can start after Foundational (Phase 2) — Flag/completion wiring across all three verbs; independently testable
- **User Story 5 (P3)**: Can start after Foundational (Phase 2) — Truth adapter only; independently testable

### Within Each User Story

- Tests MUST be written and FAIL before implementation
- Wheel/scratch before adapters
- Core implementation before CLI representation tests
- Story complete before moving to next priority

### Parallel Opportunities

- All Setup tasks marked [P] can run in parallel
- T006/T007/T008/T009 engine pieces are sequential (same header); T010/T011/T012 can run in parallel with engine work (different files)
- Once Foundational completes, US1–US5 test tasks marked [P] can run in parallel (same test file — coordinate appends)
- US1 solve adapter (T018) and US3 step adapter (T031) and US5 truth adapter (T041) touch different files — parallelizable once tests exist
- Examples T019/T032 and docs T042/T043/T044 touch different files — parallelizable

---

## Parallel Example: User Story 1

```bash
# Launch all US1 test tasks together (same file — coordinate append order):
Task: "Hand-derived pulse goldens in tests/test_logic_timing.cpp (T013)"
Task: "Transport-semantics test in tests/test_logic_timing.cpp (T014)"
Task: "Unit-floor tests in tests/test_logic_timing.cpp (T015)"
Task: "Ordering + determinism tests in tests/test_logic_timing.cpp (T016)"
Task: "Solve parity test in tests/test_logic_timing.cpp (T017)"

# Implementation needs no parallelism (single adapter file):
Task: "solve --with-delays adapter in src/cli/commands/logic/solve.cpp (T018)"
```

## Parallel Example: User Story 3

```bash
# All US3 test tasks together:
Task: "Clock-to-Q golden in tests/test_logic_timing.cpp (T025)"
Task: "D-arrival race offset in tests/test_logic_timing.cpp (T026)"
Task: "Latch response delay in tests/test_logic_timing.cpp (T027)"
Task: "Transient 4-state windows in tests/test_logic_timing.cpp (T028)"
Task: "Parity + determinism in tests/test_logic_timing.cpp (T029)"
Task: "Refusal tests in tests/test_logic_timing.cpp (T030)"
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1: Setup
2. Complete Phase 2: Foundational (CRITICAL — blocks all stories)
3. Complete Phase 3: User Story 1
4. **STOP and VALIDATE**: Run `logic solve --in 1 --with-delays` on `examples/logic/glitch_chain.yaml`; assert hand-derived pulse + settled parity
5. Deploy/demo if ready

### Incremental Delivery

1. Complete Setup + Foundational → Foundation ready
2. Add User Story 1 → Test independently → Deploy/Demo (MVP — glitch visible!)
3. Add User Story 2 → Default-identity pins hold → Deploy/Demo
4. Add User Story 3 → Race windows + `race_window.yaml` → Deploy/Demo
5. Add User Story 4 → Completion + parity pins → Deploy/Demo
6. Add User Story 5 → Truth settle column → Deploy/Demo
7. Polish → docs + catalog audit + full suite green

### Parallel Team Strategy

With multiple developers:

1. Team completes Setup + Foundational together
2. Once Foundational is done:
   - Developer A: User Story 1 (solve adapter + glitch example)
   - Developer B: User Story 3 (step adapter + race example)
   - Developer C: User Story 5 (truth adapter + drain goldens)
3. US2 (identity pins) and US4 (completion/parity) land alongside; stories integrate via the shared wheel + printer with no conflicts (different verbs/files)

---

## Notes

- [P] tasks = different files, no dependencies
- [Story] label maps task to specific user story for traceability
- Each user story is independently completable and testable
- Tests requested per FR-014: write tests, verify they FAIL before implementing
- Goldens MUST be hand-derived from path-delay arithmetic / IEEE 1364 tables, never implementation readback (constitution I/III)
- `eval.hpp` / `seq_step.hpp` / `types.hpp` / `block_interface.hpp` gain ZERO code — any diff there is a scope violation (research D1/D10)
- Registry stays 27 — no new verbs; only completion vectors change in `tests/test_cli_args.cpp`
- No schema/version bump anywhere; `sim run --solver logic` stays zero-delay (named follow-on per research D12)
- Commit after each task or logical group
- Stop at any checkpoint to validate story independently

---

## Phase 9: Convergence

**Purpose**: Close the two convergence findings (no missing/contradicting/unrequested code — both behaviors verified working; only regression pins absent)

- [X] T048 Add shared-net transient-contention trace test in `tests/test_logic_timing.cpp` (two staggered drivers on one net through `solve --with-delays`, asserting the X window opens/closes via the drivers' entries and the settled tail stays byte-identical to flag-off) per FR-009 (partial)
- [X] T049 Add `race_window.yaml` replay test in `tests/test_logic_timing.cpp` (load, poke stimulus, `step --with-delays` asserting the header's expected arrival offsets verbatim, plus the header's second-step capture) per FR-014 (partial)
