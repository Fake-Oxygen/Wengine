# Tasks: JK/T Flop Kinds

**Input**: Design documents from `specs/003-jk-t-flops/`

**Prerequisites**: plan.md (required), spec.md (required for user stories), research.md, data-model.md, contracts/

**Tests**: Included as first-class tasks — required by the project constitution (Principle III: new kinds ship with tests against independently derived references) and by spec success criteria SC-001..SC-005.

**Organization**: Tasks are grouped by user story so each story can be implemented, tested, and delivered independently. Engine kind vocabulary (Phase 2) is shared foundation; stepper branches land per-story (T in US1, JK in US2) so each P1 story is a complete vertical slice.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3)
- Include exact file paths in descriptions

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Prove a green baseline before touching anything

- [X] T001 Configure, build, and run the full test suite per AGENTS.md (`cmake -S . -B build -DBUILD_TESTS=ON -DBUILD_BENCHMARKS=ON`, `cmake --build build`, `ctest --test-dir build --output-on-failure`) and record the baseline result

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Engine kind vocabulary for JK/T with zero behavior change (stepper untouched — existing step tests prove no regression)

**⚠️ CRITICAL**: No user story work can begin until this phase is complete

- [X] T002 [P] Add appended kind codes in `include/wengine/logic/types.hpp`: `kJk = 18`, `kT = 19` with `gate_kind_name` `"JK"`/`"T"` cases plus comment update, and grow `next_counter` 18 → 20 entries per research R1
- [X] T003 [P] Extend the pin/kind vocabulary in `include/wengine/logic/netlist.hpp`: `valid_fanin` (JK==3, T==2), `default_fanin` (JK→3, T→2), `is_sequential_kind` += JK/T, `parse_gate_kind` table += `{"JK", kJk}`/`{"T", kT}` with want-string update, `resolve_gate` `kAllKinds` += JK/T, `parse_pin` JK rows (J→0, K→1, CLK→2) and T rows (T→0, CLK→1) per research R2
- [X] T004 [P] Cover the new kinds in combinational paths: `eval_gate` + bit-parallel word eval fallthrough in `include/wengine/logic/eval.hpp`, and audit/extend every kind switch plus the sequential guard in `include/wengine/logic/optimize.hpp` (sites near lines 679, 918, 994, 1107, 1402, 2144) so combinational rewrites skip JK/T exactly like DFF/DLATCH per research R6
- [X] T005 [P] Extend YAML persistence in `src/logic/yaml_store.cpp`: `pin_text` canonical `JKn.J|K|CLK` + `Tn.T|CLK` names and kind want-strings (`want AND|...|JK|T`); loader accepts `inputs: 3` (JK) / `inputs: 2` (T) via `valid_fanin` with no schema version bump per research R6
- [X] T006 Extend kind-vocabulary tests in `tests/test_logic_seq_kinds.cpp`: ordinal `static_assert`s for 18/19, `gate_kind_name`/`parse_gate_kind` incl. case-insensitivity and near-miss rejections for `"T"`/`"JK"`, `parse_pin` rows for J/K/CLK + T/CLK incl. numeric aliases and wrong-kind rejection (no `CMakeLists.txt` change — existing target)

**Checkpoint**: Foundation ready — `test_logic_seq_kinds` plus the full existing suite green, no stepper behavior changed. User story work can now begin (US1 → US2 sequenced: same snapshot region of `seq_step.hpp`).

---

## Phase 3: User Story 1 — Divide a clock with chained T flops (Priority: P1) 🎯 MVP

**Goal**: A working T flop — toggle/hold on the shared stepper, drivable and observable through existing session verbs, persisted through save/load.

**Independent Test**: Quickstart §1–§2 transcripts — single T alternates Q every cycle while T=1 and freezes while T=0; chained `wire T1.Q T2.T` + `step 4` reads back divide-by-2/divide-by-4; replay from a fresh workspace is identical.

- [X] T007 [US1] Implement the T snapshot branch in `include/wengine/logic/seq_step.hpp`: resolve `T` (input 0) in the `seq_step_filtered` snapshot loop, `next_q = T ? NOT q_old : q_old` from pre-edge Q with validated CLK, X/Z failing loudly with state untouched, plus header-comment update per research R3/R5 (no `apply_async_controls` change per R4)
- [X] T008 [P] [US1] Add T excitation engine tests in `tests/test_logic_seq_step.cpp`: research R3 T table × old-Q against the hand-derived reference (not implementation output), hold across N edges, power-on Q=0, X/Z rejection without commit, subset-stepping membership
- [X] T009 [P] [US1] Add T CLI session goldens following the existing `tests/test_logic_seq_cli_*.cpp` file split: `add T`, `poke T1.T=1/0/release`, `step` transition lines `T1.Q: a->b` / no-change shape, `peek` Q/NQ readback, `wire`/`list` coverage, usage-error-no-mutation cases per contracts C1–C5
- [X] T010 [P] [US1] Add T YAML round-trip tests in `tests/test_logic_yaml.cpp`: `kind: t, inputs: 2` rows with wires + `ignore:` indices save/load identically; legacy files without T load and step exactly as before

**Checkpoint**: At this point, User Story 1 is fully functional and testable independently — divider designs work end-to-end with no JK involvement.

---

## Phase 4: User Story 2 — Build a counter from JK flops (Priority: P2→P1 in spec; second slice)

**Goal**: A working JK flop — all four excitation rows on the shared stepper, same verb/YAML coverage as T.

**Independent Test**: Quickstart §3–§4 transcripts — one flop driven through J/K = 00/10/01/11 yields hold/set/reset/toggle per advance; 2-bit counter walks 00 → 01 → 10 → 11 → 00 over 4 advances.

- [X] T011 [US2] Implement the JK snapshot branch in `include/wengine/logic/seq_step.hpp`: resolve `J` (input 0) / `K` (input 1) in the same snapshot loop as T007, `next_q` per the four-row table (hold / Q=0 / Q=1 / NOT q_old) from pre-edge Q, plus header-comment update per research R3/R5
- [X] T012 [P] [US2] Add JK excitation engine tests in `tests/test_logic_seq_step.cpp`: research R3 JK table × old-Q against the hand-derived reference, 2-bit synchronous-counter sequence with wrap-around, determinism replay (same drive→advance→observe twice → identical)
- [X] T013 [P] [US2] Add JK CLI session goldens following the existing `tests/test_logic_seq_cli_*.cpp` file split: `add JK` (default 3 inputs, `--inputs` misuse rejected), J/K poke/step/peek/wire/list coverage, wrong-kind pin errors (`DFF1.J`, `JK1.D`) per contracts C1–C5
- [X] T014 [P] [US2] Add JK YAML round-trip tests in `tests/test_logic_yaml.cpp`: `kind: jk, inputs: 3` rows with `JKn.J|K|CLK` wire endpoints + `ignore:` indices save/load identically

**Checkpoint**: At this point, User Stories 1 AND 2 both work — T dividers and JK counters each independently functional on the shared stepper.

---

## Phase 5: User Story 3 — Mix DFF, JK, and T flops on one clock stepper (Priority: P2)

**Goal**: Proven kind coexistence — one advance steps every kind correctly with a single shared cycle count.

**Independent Test**: Quickstart §5 transcript — one DFF + one JK + one T in one domain, `step 2`, per-kind-correct Q with one cycle count; the same script replayed twice yields byte-identical outputs.

- [X] T015 [US3] Add mixed-kind stepping tests in `tests/test_logic_seq_step.cpp` (or the cycles file per the existing split): DFF capture + JK toggle + T toggle/hold in one domain over N edges, selective-domain stepping with JK/T members vs non-members, cross-kind NQ polarity, determinism replay
- [X] T016 [P] [US3] Add the mixed-workspace CLI session golden in `tests/test_logic_seq_cli_step.cpp`: quickstart §5 transcript verbatim (`add DFF/JK/T`, drive, `step 2`, `peek`), plus replay-twice-identical assertion per SC-003/SC-005

**Checkpoint**: Mixed designs (JK counter feeding a DFF register beside a T divider) step deterministically on one clock.

---

## Phase 6: User Story 4 — Read one excitation table per kind (Priority: P3)

**Goal**: Both tables documented in help/completion and in the living docs, so predictions never require trial and error.

**Independent Test**: Quickstart §6 transcript — `help logic` lists both kinds with pin tables and the normative sentences; completion suggests kinds and pins; each table row driven once yields the documented next state on every surface.

- [X] T017 [US4] Extend help + completion: kind list, pin tables, and the C0/C6 excitation sentences in `include/wengine/cli/logic_registry.hpp`; usage/kind/completion strings in `src/cli/commands/logic/add.cpp` (`kCompleteKinds` += JK/T, fan-in error text); J/K/CLK + T/CLK pin completion wherever `DFFn.` RST/SET/EN completion lives (`src/cli/commands/logic/common.cpp`, `common.hpp` — grep `RST`); update `tests/test_cli_args.cpp` goldens/registry-size assert only if affected
- [X] T018 [P] [US4] Update living docs: kind table (§12) + excitation tables in `docs/logic.md`; `README.md` logic module-table row, run examples (divider + counter), and test list

**Checkpoint**: All user stories independently functional; every table row observable and documented.

---

## Phase 7: Polish & Cross-Cutting Concerns

**Purpose**: Final verification and consistency sweep

- [X] T019 Run the full verification gate: `ctest --test-dir build --output-on-failure` (both `dev` and `test-opt` presets where practical), confirm zero new compiler warnings, and execute every `specs/003-jk-t-flops/quickstart.md` §1–§6 transcript end-to-end (headless REPL + script-file replay)
- [X] T020 [P] Final consistency audit via content search: every `switch (kind)`, kind-name want-string, help kind list, `docs/logic.md` table, and README mention covers JK/T; SC-004 zero-migration spot-check (pre-feature netlist outputs vs post-feature outputs step-for-step)

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies — starts immediately
- **Foundational (Phase 2)**: Depends on Setup — BLOCKS all user stories
- **User Stories (Phases 3–6)**: All depend on Foundational completion
  - US1 → US2 sequenced (adjacent branches in the same `seq_step.hpp` snapshot loop — same-file region, not parallel-safe)
  - US3 depends on US1 + US2 behavior (mixed stepping needs both branches)
  - US4 (docs/help) can proceed in parallel with US3 once US1/US2 engine work is done
- **Polish (Phase 7)**: Depends on all user stories being complete

### User Story Dependencies

- **User Story 1 (P1, T divider)**: After Foundational — no other story dependencies. **MVP scope: Phases 1→3 only.**
- **User Story 2 (P1, JK counter)**: After Foundational + US1 snapshot branch (same-file ordering) — independently testable per its own transcripts
- **User Story 3 (P2, mixed)**: After US1 + US2 — mixed assertions need both excitation branches
- **User Story 4 (P3, tables)**: After US1 + US2 engine work — documents implemented behavior

### Within Each User Story

- Engine tests first (vs hand-derived R3 reference), FAIL before the snapshot branch, PASS after
- Snapshot branch before CLI goldens (goldens assert engine behavior through the session)
- YAML round-trip alongside CLI goldens (independent files — parallel-safe)
- Checkpoint validation before moving to the next story

### Parallel Opportunities

- T002 + T003 + T004 + T005 in parallel (four different files: `types.hpp`, `netlist.hpp`, `eval.hpp`+`optimize.hpp`, `yaml_store.cpp`)
- T008 + T009 + T010 in parallel after T007 (engine test file, CLI test files, YAML test file)
- T012 + T013 + T014 in parallel after T011 (same file split as US1)
- T016 in parallel with T015 (CLI golden vs engine test); T018 in parallel with T015–T017 (docs vs code)
- T020 in parallel with T019 (read-only audit vs test runs)

---

## Parallel Example: Foundational Phase

```bash
# Launch all vocabulary tasks together (different files, no dependencies):
Task: "Add appended kind codes in include/wengine/logic/types.hpp (T002)"
Task: "Extend pin/kind vocabulary in include/wengine/logic/netlist.hpp (T003)"
Task: "Cover new kinds in include/wengine/logic/eval.hpp + optimize.hpp (T004)"
Task: "Extend YAML persistence in src/logic/yaml_store.cpp (T005)"
```

## Parallel Example: User Story 1 (after T007)

```bash
# Launch verification tasks together (different test files, no dependencies):
Task: "Add T excitation engine tests in tests/test_logic_seq_step.cpp (T008)"
Task: "Add T CLI session goldens in tests/test_logic_seq_cli_*.cpp (T009)"
Task: "Add T YAML round-trip tests in tests/test_logic_yaml.cpp (T010)"
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1: Setup (T001 — green baseline)
2. Complete Phase 2: Foundational (T002–T006 — vocabulary, no behavior change)
3. Complete Phase 3: User Story 1 (T007–T010 — T divider end-to-end)
4. **STOP and VALIDATE**: Quickstart §1–§2 transcripts pass; full suite green
5. Deploy/demo if ready — dividers work without any JK code

### Incremental Delivery

1. Setup + Foundational → vocabulary ready, zero behavior delta
2. Add US1 (T007–T010) → Test independently → Demo (MVP: dividers!)
3. Add US2 (T011–T014) → Test independently → Demo (counters!)
4. Add US3 (T015–T016) → mixed coexistence proven
5. Add US4 (T017–T018) → documented tables + living docs
6. Polish (T019–T020) → gates green, quickstart validated

### Parallel Team Strategy

With multiple developers:

1. Team completes Setup + Foundational together (T002–T005 parallelize across four developers; T006 follows)
2. Once Foundational is done:
   - Developer A: US1 stepper + engine tests (T007–T008), then US2 stepper + engine tests (T011–T012, same-file sequencing)
   - Developer B: US1/US2 CLI + YAML goldens (T009–T010, T013–T014)
   - Developer C: US3 mixed tests (T015–T016) once branches land, then US4 docs (T017–T018)
3. Stories integrate through the shared stepper with no cross-story breakage (SC-004 guards existing behavior)

---

## Notes

- [P] tasks = different files, no dependencies
- [Story] label maps task to specific user story for traceability
- Each user story is independently completable and testable via its quickstart transcripts
- Engine tests assert against the hand-derived research R3 tables, never implementation output (constitution I)
- Commit after each task or logical group; stop at any checkpoint to validate
- Avoid: vague tasks, same-file parallel edits (`seq_step.hpp` snapshot loop is sequential T007→T011), cross-story dependencies that break independence

---

## Phase 8: Convergence

**Purpose**: Close the assessed gap between the artifacts and the code (converge run, 2026-11-01). One LOW finding; no CRITICAL/HIGH issues.

- [X] T021 Update the flop-aware cycle-validation sentence in `docs/logic.md` to enumerate all sequential input sets (five DFF + two DLATCH + three JK + two T) per T018/`docs/logic.md` §12 (partial)
