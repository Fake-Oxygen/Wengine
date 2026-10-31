# Tasks: Sequential Session Verbs

**Input**: Design documents from `specs/001-seq-session-verbs/`
(`spec.md`, `plan.md`, `research.md`, `data-model.md`, `contracts/`,
`quickstart.md`)

**Prerequisites**: plan.md, spec.md, research.md, data-model.md,
contracts/step.md, contracts/poke.md, contracts/peek.md

**Tests**: Included — required by project governance (Constitution III:
new verbs ship CLI run/usage/stray/completion tests; registry-size
`static_assert` kept in sync; multi-boundary verification).

**Organization**: Grouped by user story; each story is an independently
testable increment after Foundational completes.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (US1, US2, US3)

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Clean baseline before changes

- [X] T001 Verify green baseline via `CMakePresets.json` (preset `dev`): configure, build, `ctest --output-on-failure` passes with no new warnings

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Engine + shared display work every story builds on
(research D2, D7, D8; data-model §6)

**⚠️ CRITICAL**: No user story work can begin until this phase is complete

- [X] T002 Named-pin alias table + `parse_pin` extension in `include/wengine/logic/netlist.hpp` (DFF: D=0/CLK=1/Q; DLATCH: D=0/EN=1/Q; `NQ` output selector; case-insensitive names; numerics preserved; names on combinational kinds rejected)
- [X] T003 Sequential creation path in `include/wengine/logic/netlist.hpp` (allow `kDff`/`kDlatch` in `add_gate` with exactly 2 inputs; extend `next_counter` coverage to ordinals 16/17 with wrap-skip-0)
- [X] T004 Update `tests/test_logic_seq_kinds.cpp` (pinned `add_gate` refusal of DFF/DLATCH flips to acceptance: creation goldens, per-kind counters, fan-in rules)
- [X] T005 NQ state + wiring ops in `include/wengine/logic/netlist.hpp` (per-sink polarity array; `nq_fanout` with `output_fanout` keeping Q semantics; per-NQ ignore flag with `ignore_output` keeping Q semantics; `wire_pins` carries polarity from source ref; `unwire`/`remove_gate` clear polarity; block output counts/interface enumerate dangling Q then NQ per gate in slot order; slot-level `wire()` stays non-inverted)
- [X] T006 Apply sink polarity at input-resolution sites in `include/wengine/logic/eval.hpp` (scalar solve path + bit-parallel word path: invert after `resolve_net` when polarity set)
- [X] T007 Apply sink polarity in `include/wengine/logic/seq_step.hpp` (phase-1 eval, D/CLK snapshot, phase-2 re-settle via `resolve_step_input`)
- [X] T008 [P] NQ YAML support in `src/logic/yaml_store.cpp` (`from: DFF1.NQ` via `parse_pin`, writer emits canonical named pins incl. NQ sources, new ignore-nq key with missing-key default; interface after T002/T005 lands)
- [X] T009 [P] Named-pin display + completion in `src/cli/commands/logic/common.cpp` (`pin_text` shows `DFF1.D`/`DFF1.CLK`/`DFF1.Q`/`DFF1.NQ`; `collect_pins` lists Q+NQ outputs; interface after T002/T005 lands)
- [X] T010 Show both seq outputs in `src/cli/commands/logic/list.cpp` (Q + NQ lines via updated `pin_text`); verify `src/cli/commands/logic/draw.cpp` path handles NQ-wired nets (BMP-out smoke, no new rendering)
- [X] T011 New `tests/test_logic_seq_cli_pins.cpp` (alias parsing both directions; wrong-kind name errors; NQ inversion goldens incl. power-on; fanout/interface counts with NQ wired/dangling; YAML round-trip with NQ sources) + register in `CMakeLists.txt` + `add_test`

**Checkpoint**: Foundation ready — engine accepts `add DFF`, named pins parse everywhere, NQ inverts live, YAML round-trips; user story implementation can now begin

---

## Phase 3: User Story 1 - Advance clocks N cycles (Priority: P1) 🎯 MVP

**Goal**: `step [DOMAIN] [N]` advances all flops (global) or one clock
domain (selective) with exact cycle reporting (contracts/step.md)

**Independent Test**: quickstart.md Scenario 1 (named-pin toggle loop:
`peek` → `step 1` → `peek` → `step 2`, exact Q/NQ transitions + cycle
counts) and Scenario 4 (100-cycle piped replay is byte-identical);
delivers a working advance-and-observe loop alone

### Tests for User Story 1

> **NOTE: Write these tests FIRST, ensure they FAIL before implementation**

- [X] T012 [US1] Step goldens in `tests/test_logic_seq_cli_step.cpp` (2-bit counter + shift-register vectors from `test_logic_seq_step.cpp` driven through verbs; selective domains `clk_a` vs `clk_b`; zero-cycle no-op report; unknown-domain/negative-count usage errors change nothing; determinism replay)

### Implementation for User Story 1

- [X] T013 [US1] Subset-step core in `include/wengine/logic/seq_step.hpp` (flop-subset snapshot/commit in slot order + global combinational re-settle; global call = all flops, preserving SEQ4 semantics)
- [X] T014 [US1] CLI session state in `src/cli/commands/logic.cpp` + `src/cli/commands/logic/common.hpp` + `src/cli/commands/logic/common.cpp` (ClockDomain derivation per data-model §1; sticky poke map + block-input composition defaulting unset pins Low; per-domain cycle counters; `reset` clears all three)
- [X] T015 [US1] `step` verb in `src/cli/commands/logic/step.cpp` + spec in `include/wengine/cli/logic_registry.hpp` + `LogicRegistry` append (19→20) with `complete()` (domain names incl. NQ-driven nets, `--cycles=` form)
- [X] T016 [US1] Register `tests/test_logic_seq_cli_step.cpp` in `CMakeLists.txt` + `add_test`; sync `static_assert` 19→20 and add step help/completion goldens in `tests/test_cli_args.cpp`

**Checkpoint**: At this point, User Story 1 should be fully functional and testable independently (Scenario 1 + 4 pass)

---

## Phase 4: User Story 2 - Poke block inputs (Priority: P2)

**Goal**: `poke PIN=0|1|release` drives/releases dangling block inputs
with sticky session state (contracts/poke.md)

**Independent Test**: quickstart.md Scenario 2 up to the first `step`
(poke `DFF1.D=1`, bare `poke` lists the map, wrong-kind `AND1.D`
rejected with exit 2 and no state change); delivers controllable
stimulus independent of clocking

### Tests for User Story 2

> **NOTE: Write these tests FIRST, ensure they FAIL before implementation**

- [X] T017 [US2] Poke goldens in `tests/test_logic_seq_cli_poke.cpp` (drive/release/list-map; named spelling + numeric alias resolve to one entry; unknown-pin/wired-pin/bad-value/duplicate errors; poke composes into `step` block inputs per data-model §3)

### Implementation for User Story 2

- [X] T018 [US2] `poke` verb in `src/cli/commands/logic/poke.cpp` + spec in `include/wengine/cli/logic_registry.hpp` + `LogicRegistry` append (20→21) with `complete()` (reuse `complete_from_value` helper; `--from=`/`--clear`/`--help` forms); sync `static_assert` 20→21 in `tests/test_cli_args.cpp` and register `tests/test_logic_seq_cli_poke.cpp` in `CMakeLists.txt` + `add_test`

**Checkpoint**: At this point, User Stories 1 AND 2 work (poke → step → peek loop closable from session, one-shot, and pipes)

---

## Phase 5: User Story 3 - Peek flop Q state (Priority: P3)

**Goal**: `peek [GATE]` reads Q (+NQ) at single-flop, per-block, and
workspace scopes in slot order (contracts/peek.md)

**Independent Test**: Build a sequential circuit with known reset state,
`peek` without advancing, confirm `DFF1.Q=0 NQ=1 (serial 0)` lines +
`flops: <k>` count; piped one-shot lists all flops in stable order;
delivers readback with no other new action

### Tests for User Story 3

> **NOTE: Write these tests FIRST, ensure they FAIL before implementation**

- [X] T019 [US3] Peek goldens in `tests/test_logic_seq_cli_peek.cpp` (all three scopes; Q+NQ line format per contract; empty-workspace `no flops` success; unknown-gate/combinational-gate errors; serial stability across steps)

### Implementation for User Story 3

- [X] T020 [US3] `peek` verb in `src/cli/commands/logic/peek.cpp` + spec in `include/wengine/cli/logic_registry.hpp` + `LogicRegistry` append (21→22) with `complete()` (live sequential gate names only); sync `static_assert` 21→22 in `tests/test_cli_args.cpp` and register `tests/test_logic_seq_cli_peek.cpp` in `CMakeLists.txt` + `add_test`

**Checkpoint**: All user stories independently functional (full poke → step → peek loop)

---

## Phase 6: Polish & Cross-Cutting Concerns

**Purpose**: Docs, full-suite verification, quickstart proof

- [X] T021 Document named pins + NQ + step/poke/peek verbs in `docs/logic.md` (pin table D/CLK/EN/Q/NQ, alias rule, NQ wiring/interface semantics, verb summary)
- [X] T022 Update README Run examples, module table, and test list for the three verbs + named pins + new test binaries
- [X] T023 Full verification: `dev` + `test-opt` presets configure/build, `ctest --output-on-failure` green, zero new compiler warnings (`-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wnon-virtual-dtor` on `wengine_cli`)
- [X] T024 Run `specs/001-seq-session-verbs/quickstart.md` Scenarios 1–4 verbatim incl. the replay gate (`cmp` silent)

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies — T001 starts immediately
- **Foundational (Phase 2)**: Depends on Setup — BLOCKS all user stories (T002→T011 in numeric order; T008/T009 develop against D7/D8 interfaces, integrate after T002/T005 land)
- **User Stories (Phase 3–5)**: Depend on Foundational; run sequentially in priority order (US1 → US2 → US3) because all three append to the shared `LogicRegistry` and `tests/test_cli_args.cpp`/`CMakeLists.txt`
- **Polish (Phase 6)**: Depends on all user stories complete

### User Story Dependencies

- **User Story 1 (P1)**: After Foundational — no story dependencies; MVP = Phase 1 + 2 + 3 (STOP and VALIDATE per quickstart Scenarios 1 + 4)
- **User Story 2 (P1)**: After Foundational + US1 (consumes T014 poke-map infrastructure; independently testable per its criteria)
- **User Story 3 (P2)**: After Foundational — may land after US1/US2; reads `SeqState` only, no dependency on poke/step verbs

### Within Each User Story

- Verb tests written FIRST and FAIL before the verb implementation (T012 before T013–T016; T017 before T018; T019 before T020)
- Engine change before CLI verb; CLI verb before registration/assert sync
- Registry/`test_cli_args.cpp`/`CMakeLists.txt` edits serialize (shared files, never [P])

### Parallel Opportunities

- T008 (`src/logic/yaml_store.cpp`) + T009 (`src/cli/commands/logic/common.cpp`) develop in parallel against T002/T005 interfaces (different files; integrate after)
- After Foundational: US3 verb work (T019/T020, read-only over `SeqState`) can parallelize with US1/US2 at the risk of registry-file conflicts — sequential recommended
- T021 (docs/logic.md) + T022 (README) can run in parallel (different files)

---

## Parallel Example: Foundational display tasks

```bash
# Different files, additive interfaces from research D7/D8 — develop together:
Task: "NQ YAML support in src/logic/yaml_store.cpp"            # T008 [P]
Task: "Named-pin display + completion in src/cli/commands/logic/common.cpp"  # T009 [P]
# Integrate after T002 (parse_pin aliases) + T005 (NQ state) land.
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1: Setup (T001 green baseline)
2. Complete Phase 2: Foundational (T002–T011 — engine + pins + NQ + display)
3. Complete Phase 3: User Story 1 (T012–T016)
4. **STOP and VALIDATE**: quickstart.md Scenarios 1 + 4 pass verbatim
5. Deploy/demo if ready (advance-and-observe loop works headless)

### Incremental Delivery

1. Setup + Foundational → sequential circuits buildable with named pins + NQ
2. + US1 → `step` works (MVP!)
3. + US2 → `poke` works (stimulus expressible)
4. + US3 → `peek` works (state assertable); full loop scriptable
5. + Polish → docs + full-suite proof

---

## Notes

- [P] tasks = different files, no dependencies (T008/T009 integrate after T002/T005)
- [Story] label maps task to user story for traceability (US1 = step, US2 = poke, US3 = peek)
- Registry appends + `static_assert` + `CMakeLists.txt` + `add_test` ride with their feature task (never batched separately — the build must stay green after every task)
- Constitution gates per task: determinism (slot order everywhere), correctness (goldens from existing engine vectors, never the new code), zero warnings, no Netlist field removal/renumber

---

## Phase 7: Convergence

**Purpose**: NQ ignored-by-default (user-requested readability default)

- [X] T025 Change NQ outputs to ignored-by-default (dangling NQ hidden from interface/list/outputs until explicitly `use`d or wired; `peek` unchanged) across engine defaults, `add_gate` slot-recycle hygiene, YAML loader default, affected tests, and spec/plan/contracts/quickstart/docs per FR-011 as amended by user request (contradicts)
