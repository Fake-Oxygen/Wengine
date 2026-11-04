# Tasks: SR/D Latch Pair

**Input**: Design documents from `specs/004-sr-d-latches/`

**Prerequisites**: plan.md (required), spec.md (required for user stories), research.md, data-model.md, contracts/

**Tests**: Included as first-class tasks — required by the project constitution (Principle III: new kinds ship with tests against independently derived references) and by spec success criteria SC-001..SC-005.

**Organization**: Tasks are grouped by user story so each story can be implemented, tested, and delivered independently. SR kind vocabulary (Phase 2) is shared foundation with zero behavior change; the level-settle pass lands with US1 (DLATCH arm) and grows its SR arm in US2, so each P1 story is a complete vertical slice.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3)
- Include exact file paths in descriptions

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Prove a green baseline before touching anything

- [X] T001 Configure, build, and run the full test suite per AGENTS.md (`cmake -S . -B build -DBUILD_TESTS=ON -DBUILD_BENCHMARKS=ON`, `cmake --build build`, `ctest --test-dir build --output-on-failure`) and record the baseline result

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: SR kind vocabulary with zero behavior change (stepper untouched — SR temporarily rides the existing SEQ4 edge-capture fallthrough like DLATCH does today; existing step tests prove no regression)

**⚠️ CRITICAL**: No user story work can begin until this phase is complete

- [X] T002 [P] Add appended kind code in `include/wengine/logic/types.hpp`: `kSrlatch = 20` with `gate_kind_name` `"SRLATCH"` case plus comment update, and grow `next_counter` 20 → 21 entries per research R1
- [X] T003 [P] Extend the pin/kind vocabulary in `include/wengine/logic/netlist.hpp`: `valid_fanin` (SR==2), `default_fanin` (SR→2), `is_sequential_kind` += SR, `parse_gate_kind` table += `{"SRLATCH", kSrlatch}` with want-string update, `resolve_gate` `kAllKinds` += SR, `parse_pin` SR rows (S→0, R→1) ahead of the Q/NQ checks per research R2
- [X] T004 [P] Cover the new kind in combinational paths: `eval_gate` + bit-parallel word eval fallthrough in `include/wengine/logic/eval.hpp`, and audit/extend every kind switch plus the sequential guard in `include/wengine/logic/optimize.hpp` so combinational rewrites skip SR exactly like DFF/DLATCH/JK/T per research R6
- [X] T005 [P] Extend YAML persistence in `src/logic/yaml_store.cpp`: `pin_text` canonical `SRn.S|R` names and kind want-strings (`want AND|...|SRLATCH`); loader accepts `inputs: 2` (SR) via `valid_fanin` with no schema version bump per research R7
- [X] T006 Extend kind-vocabulary tests in `tests/test_logic_seq_kinds.cpp`: ordinal `static_assert` for 20, `gate_kind_name`/`parse_gate_kind` incl. case-insensitivity and near-miss rejections, `parse_pin` rows for S/R incl. numeric aliases and wrong-kind rejection (no `CMakeLists.txt` change — existing target)

**Checkpoint**: Foundation ready — `test_logic_seq_kinds` plus the full existing suite green, no stepper behavior changed. User story work can now begin.

---

## Phase 3: User Story 1 — Hold a bus value with a transparent D latch (Priority: P1) 🎯 MVP

**Goal**: A working level-sensitive D latch — transparent follow while EN active, frozen hold while EN released, with no clock advance — refined from the SEQ4 edge-capture simplification.

**Independent Test**: Quickstart §1 transcript — `poke D=1` with EN active moves Q at once; D toggles with EN released move nothing; `step 3` leaves frozen Q unchanged; re-asserting EN re-tracks D; replay from a fresh workspace is identical.

### Tests for User Story 1

> **NOTE: Write these tests FIRST, ensure they FAIL before implementation**

- [X] T007 [P] [US1] Add DLATCH level engine tests in `tests/test_logic_seq_step.cpp`: research R3 DLATCH table × old-Q against the hand-derived reference (not implementation output), transparent-follow-without-advance, frozen hold across D toggles and N advances, power-on Q=0, X/Z rejection without commit, determinism replay
- [X] T008 [P] [US1] Add DLATCH level CLI session goldens following the existing `tests/test_logic_seq_cli_*.cpp` file split: transparent/frozen `poke` Q reports per contracts C2, `step` pre-edge re-track + `Q: a->b` / no-change lines per C3, `peek` Q/NQ readback, `wire`/`list` coverage, usage-error-no-mutation cases per contracts C1–C5
- [X] T009 [P] [US1] Add DLATCH YAML coverage in `tests/test_logic_yaml.cpp`: `kind: dlatch, inputs: 2` rows with `DLATCHn.D|EN` wires + `ignore:` indices save/load identically under the new level rule

### Implementation for User Story 1

- [X] T010 [US1] Implement the level-settle helper + DLATCH arm in `include/wengine/logic/seq_step.hpp`: new level pass reusing `eval_comb_phase1` + `resolve_step_input` (resolve-all-then-commit, slot order), `Q_next = EN ? D : q_old` per research R3/R4, DLATCH-exempt from the `seq_step_filtered` edge snapshot, header-comment update replacing the SEQ4 simplification note (SR keeps the old edge fallthrough until US2 — documented interim)
- [X] T011 [US1] Exclude level enables from clock domains in `src/cli/commands/logic/common.cpp`: `clock_domains()` covers edge kinds only (DFF/JK/T), DLATCH EN no longer forms a domain per research R6; wire the level pass into the session apply path (`apply_async_and_report` sibling call) and `step.cpp` pre-edge position per research R4

**Checkpoint**: At this point, User Story 1 is fully functional and testable independently — bus-hold designs work end-to-end with no SR involvement.

---

## Phase 4: User Story 2 — Build an async handshake from an SR latch (Priority: P2→P1 in spec; second slice)

**Goal**: A working SR latch — set/reset/hold with no clock plus loud `S=R=1` refusal with state untouched on every entry path (poke/wire/tie/load/step).

**Independent Test**: Quickstart §2 transcript — set then release holds 1, reset then release holds 0, all with no `step`; attempted `S=R=1` is a usage error (exit 2) naming the combination with Q unchanged; replay from a fresh workspace is identical.

### Tests for User Story 2

> **NOTE: Write these tests FIRST, ensure they FAIL before implementation**

- [X] T012 [P] [US2] Add SR excitation + illegal-refusal engine tests in `tests/test_logic_seq_step.cpp`: research R3 SR table × old-Q against the hand-derived reference, `S=R=1` failing loudly with nothing committed (direct, wired-comb, and tied-leader combinations), hold across N advances, determinism replay
- [X] T013 [P] [US2] Add SR CLI session goldens following the existing `tests/test_logic_seq_cli_*.cpp` file split: `add SRLATCH`, immediate set/reset/hold `poke` reports, illegal-completion `poke`/`wire`/`tie` rejections with state-untouched assertions, `peek` Q/NQ readback, `step`-with-illegal failure before any edge or counter movement per contracts C1–C5
- [X] T014 [P] [US2] Add SR YAML round-trip + illegal-load tests in `tests/test_logic_yaml.cpp`: `kind: srlatch, inputs: 2` rows with `SRn.S|R` wires + `ignore:` indices save/load identically; a file whose wiring resolves `S=R=1` fails the load atomically (workspace kept); legacy SR-free files load unchanged

### Implementation for User Story 2

- [X] T015 [US2] Implement the SR arm + illegal refusal in `include/wengine/logic/seq_step.hpp`: extend the Phase-3 level pass with `resolve_sr` per research R3/R4 (any member resolving `S=kHigh, R=kHigh` returns failure before any `st.q` write), extend the edge-exemption guard to SR, per research R5
- [X] T016 [US2] Add probe-then-revert level checks to the session verbs in `src/cli/commands/logic/` (`poke.cpp`, `wire.cpp`, `tie.cpp`, load path): each mutation probes the level pass before committing the drive/driver/class/file per research R5, mapping failures to usage errors (exit 2) naming the illegal combination with state untouched

**Checkpoint**: At this point, User Stories 1 AND 2 both work independently — bus holds and async handshakes each function end-to-end on the shared level pass.

---

## Phase 5: User Story 3 — Mix level latches with edge flops on one workspace (Priority: P2)

**Goal**: Kind coexistence — level latches (DLATCH/SR) and edge flops (DFF/JK/T) side by side under shared verbs, each following its own rule with one consistent readback.

**Independent Test**: Quickstart §3 transcript — one DFF + one SR + one DLATCH in one workspace, level Q resolved at once while DFF captures at the edge, one shared cycle count; run twice from the same start → byte-identical outputs.

- [X] T017 [P] [US3] Add mixed-kind stepping tests in `tests/test_logic_seq_step.cpp`: transparent/frozen/held level Q beside DFF/JK/T edge capture over N advances, selective-domain stepping with level members present vs absent, cross-kind NQ polarity, determinism replay
- [X] T018 [US3] Add the mixed-workspace CLI session golden in `tests/test_logic_seq_cli_step.cpp`: quickstart §3 transcript verbatim (`add DFF/SRLATCH/DLATCH`, drive, `step`, `peek`), plus replay-twice-identical assertion per SC-003/SC-005

**Checkpoint**: Mixed designs (handshake flag feeding an edge register beside a bus hold) step deterministically on one workspace.

---

## Phase 6: User Story 4 — Read one documented rule per latch kind (Priority: P3)

**Goal**: One normative sentence per kind, visible in help and completed in the session, predicting every input combination without trial and error.

**Independent Test**: Quickstart §4 transcript — `help logic` lists the pin tables + level sentences; every row of each table driven once yields the documented outcome; invalid drives rejected without mutation.

- [X] T019 [P] [US4] Extend help + completion: kind list, pin tables, and the C0/C6 level sentences in `include/wengine/cli/logic_registry.hpp`; usage/kind/completion strings in `src/cli/commands/logic/add.cpp` (`kCompleteKinds` += SRLATCH, fan-in error text); S/R pin completion wherever `DLATCHn.` EN completion lives (`src/cli/commands/logic/common.cpp`, `common.hpp` — grep `EN`)
- [X] T020 [P] [US4] Add help/completion coverage in `tests/test_cli_args.cpp` (goldens/registry-size `static_assert` only if top-level completion changes) and the session help golden asserting the C6 sentences verbatim
- [X] T021 [US4] Update `docs/logic.md` (kind table + level tables + domain-exclusion note per research R7) and `README.md` (logic module-table row, run examples, test list) with the transparent/frozen rule, the SR table including the illegal row, and the DLATCH edge-to-level refinement notice

**Checkpoint**: All user stories are now independently functional with documented, completable rules.

---

## Phase 7: Polish & Cross-Cutting Concerns

**Purpose**: Final consistency, gates, and validation across all stories

- [X] T022 Full test-suite + zero-warning gate: `cmake --preset dev`, build, `ctest --test-dir build --output-on-failure` green with no new compiler warnings; SC-004 zero-migration spot-check (pre-feature SR-free netlist outputs vs post-feature outputs step-for-step, modulo the documented DLATCH refinement)
- [X] T023 [P] Run `quickstart.md` end-to-end validation (§1–§4 transcripts verbatim in REPL, script file, and piped invocation with identical readback) and fix any drift between contracts, help text, and observed output

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies - can start immediately
- **Foundational (Phase 2)**: Depends on Setup completion - BLOCKS all user stories
- **User Stories (Phase 3+)**: All depend on Foundational phase completion
  - US1 → US2 sequenced (same level-pass region of `seq_step.hpp`; US2 extends the US1 helper)
  - US3 depends on US1 + US2 (mixed coverage needs both level arms)
  - US4 (docs/help) can overlap US3 implementation but lands after behavior freezes
  - Or sequentially in priority order (US1 → US2 → US3 → US4)
- **Polish (Final Phase)**: Depends on all desired user stories being complete

### User Story Dependencies

- **User Story 1 (P1, D latch)**: Can start after Foundational (Phase 2) - No dependencies on other stories; delivers the MVP (bus hold)
- **User Story 2 (P1, SR latch)**: Can start after US1 level-pass helper exists (extends it with the SR arm + refusal) - independently testable per its own transcripts
- **User Story 3 (P2, mixed)**: Depends on US1 + US2 behavior - independently testable per the mixed transcript
- **User Story 4 (P3, docs/help)**: Depends on frozen behavior wording from US1 + US2 - independently testable per the help transcript

### Within Each User Story

- Tests MUST be written and FAIL before implementation
- Engine (`seq_step.hpp`) before session verbs (`common.cpp`/`step.cpp`/poke/wire/tie/load)
- Core implementation before integration
- Story complete before moving to next priority

### Parallel Opportunities

- All Foundational tasks marked [P] can run in parallel (T002–T005 touch different files)
- All tests for a user story marked [P] can run in parallel (T007–T009; T012–T014; T017–T018 pair with T019–T020 across stories only after behavior freeze)
- T010 (engine header) and T011 (CLI session file) touch different files but T011 calls the T010 helper — run T010 first, then T011
- Different user stories can be worked on in parallel only after the level-pass interface settles (US1 defines it; US2 extends it)

---

## Parallel Example: User Story 1

```bash
# Launch all tests for User Story 1 together (engine + CLI + YAML goldens):
Task: "Add DLATCH level engine tests in tests/test_logic_seq_step.cpp"
Task: "Add DLATCH level CLI session goldens in tests/test_logic_seq_cli_*.cpp"
Task: "Add DLATCH YAML coverage in tests/test_logic_yaml.cpp"

# Then implement (engine first, session second — not parallel, helper dependency):
Task: "Implement the level-settle helper + DLATCH arm in include/wengine/logic/seq_step.hpp"
Task: "Exclude level enables from clock domains in src/cli/commands/logic/common.cpp"
```

## Parallel Example: User Story 2

```bash
# Launch all tests for User Story 2 together:
Task: "Add SR excitation + illegal-refusal engine tests in tests/test_logic_seq_step.cpp"
Task: "Add SR CLI session goldens in tests/test_logic_seq_cli_*.cpp"
Task: "Add SR YAML round-trip + illegal-load tests in tests/test_logic_yaml.cpp"
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1: Setup (baseline green)
2. Complete Phase 2: Foundational (SR vocabulary, no behavior change)
3. Complete Phase 3: User Story 1 (D-latch transparent/frozen level pass)
4. **STOP and VALIDATE**: Test User Story 1 independently via quickstart §1
5. Deploy/demo if ready — bus holds work with no SR code

### Incremental Delivery

1. Complete Setup + Foundational → Foundation ready
2. Add User Story 1 → Test independently → Deploy/Demo (MVP!)
3. Add User Story 2 → Test independently → Deploy/Demo (pair complete: hold + handshake)
4. Add User Story 3 → Test independently → Deploy/Demo (mixed palette)
5. Add User Story 4 → Documented, completable rules
6. Each story adds value without breaking previous stories

### Parallel Team Strategy

With multiple developers:

1. Team completes Setup + Foundational together
2. Once Foundational is done:
   - Developer A: User Story 1 (level pass + DLATCH arm)
   - Developer B: SR test scaffolding (T012–T014 test files only — implementation waits for the US1 helper interface)
3. US2 implementation, then US3/US4, each complete and integrate independently

---

## Notes

- [P] tasks = different files, no dependencies
- [Story] label maps task to specific user story for traceability
- Each user story is independently completable and testable (US1 is the MVP: D latch alone delivers the bus hold)
- Verify tests fail before implementing
- DLATCH SEQ4 edge-capture goldens are *updated* to the R3 table, not deleted; SC-004 explicitly scopes the one intentional behavior change
- Stop at any checkpoint to validate the story independently
- Avoid: vague tasks, same-file conflicts, cross-story dependencies that break independence

---

## Phase 8: Convergence

**Purpose**: Close the residual gap found by the converge assessment (2026-11-04): all functional requirements, acceptance criteria, plan decisions, and constitution principles are satisfied by the current code (23/23 tasks verified, 72/72 tests green, GUI build green); only comment drift remains.

- [X] T024 Update the four stale sequential-kind enumerations in header comments to include SRLATCH per plan: research R7 (partial): `include/wengine/logic/seq_state.hpp` file-header line ("Sequential gates (DFF/DLATCH/JK/T)") and `validate_sequential` pin list ("DFF.D/CLK, DLATCH.D/EN, JK.J/K/CLK, T.T/CLK" — append SR S/R); `include/wengine/logic/netlist.hpp` `parse_gate_kind` comment ("Sequential kinds (DFF/DLATCH/JK/T)") and `resolve_gate` comment ("Covers all sequential kinds (DFF/DLATCH/JK/T)"); code at all four sites is already generic (`is_sequential_kind`), comments only — verify with grep that no `DFF/DLATCH/JK/T` enumeration without SR remains in `include/`
