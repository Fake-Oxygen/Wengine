# Tasks: Enable-Gated Buffer plus Per-Net Pull-Up/Down (012)

**Input**: Design documents from `/specs/012-gated-buffer-pulls/`
(plan.md, spec.md, research.md D1–D11, data-model.md, contracts/
engine-seam.md, cli-verbs.md, workspace-yaml.md, quickstart.md)

**Prerequisites**: plan.md, spec.md, research.md, data-model.md, contracts/

**Tests**: Included — the spec demands reference-anchored verification
(FR-013: buffer truth table and pulled resolution verified against
independently derived tables, never the implementation's output) and
the constitution requires multi-boundary coverage. New binary
`tests/test_logic_buffer.cpp` carries the story tests; existing
binaries gain extension pins.

**Organization**: Tasks grouped by user story; each story phase is an
independently testable increment on top of the foundation.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (US1…US5)
- Exact file paths in every description

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Baseline measurements and test-binary scaffold before any
behavior changes.

- [X] T001 Record pre-change baseline per research D11 in benchmarks/bench_logic.cpp (run `./build-bench/bench_logic 8 8 2000`, save ns/gate-word) and confirm `ctest --test-dir build --output-on-failure` green
- [X] T002 Create tests/test_logic_buffer.cpp scaffold (empty-OK `test_buffer: OK` binary) and register it in CMakeLists.txt (`add_executable` + `target_link_libraries` + `add_test`, inherits `wengine_test_assert` via the `test_` prefix)

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Engine core both stories build on — kind, storage,
resolution, eval. No user-story work begins until this is complete.

**⚠️ CRITICAL**: No user story work can begin until this phase is complete

- [X] T003 [P] Add `GateKind::kTbuf = 8`, `gate_kind_name` arm, `Netlist::pull_cfg` 8 KB array (values 0 = none, 1 = up, 2 = down, zero-init default), and `is_pulled_flat` / `pull_level_flat` helpers in include/wengine/logic/types.hpp (research D1, D3)
- [X] T004 [P] Add the TBUF kind-table row (exactly 2 inputs, default 2) in include/wengine/logic/pins.hpp, bump `kind_table_valid` count 13 → 14, extend the `parse_gate_kind` want-string with `TBUF` (numeric pins only: 1 = data, 2 = enable)
- [X] T005 Admit kind byte 8 in the `add_gate` unknown-kind guard and add `set_pull` / `clear_pull` engine functions with tie/ignore disjointness refusals (both directions, netlist untouched on failure) in include/wengine/logic/netlist.hpp + include/wengine/logic/tie_set.hpp
- [X] T006 Clear the slot's `pull_cfg` flats in `remove_gate` and zero the array in `reset_netlist` in include/wengine/logic/netlist.hpp (pulls die with their slot; no stale pulls behind recycled slots)
- [X] T007 [P] Integrate pull mapping in include/wengine/logic/block_interface.hpp: shared-branch all-Z → idle map with pre-map contention counting, loud level check on the single-driver branch, terminated-dangling exclusion in `is_block_input` + idle resolve in `fill_resolved`, `has_buffer_gates()` / `any_pull()` predicates (research D4)
- [X] T008 [P] Add the `eval_gate` `kTbuf` arm (enabled → data, else high-Z) and the loud-failure `kTbuf` arm in `eval_block_word` in include/wengine/logic/eval.hpp (research D2, D4)

**Checkpoint**: Foundation ready — `cmake --build build` green on engine
headers (existing `test_logic*` binaries compile against the extended
vocabulary); user story implementation can now begin.

---

## Phase 3: User Story 1 — Disconnect with an enable-gated buffer (Priority: P1) 🎯 MVP

**Goal**: A buffer whose asserted enable drives its data level and
whose released enable contributes high-Z, so one driver can step aside
on a shared bus.

**Independent Test**: Toggle one buffer's enable and assert the net
tracks the data level while asserted and resolves as if the driver
were absent while released (spec US1) — runnable via quickstart
Scenario 1.

### Tests for User Story 1

> **NOTE: Write these tests FIRST, ensure they FAIL before implementation
> (engine tasks T003–T008 land first, so kind/eval tests fail until then;
> CLI/draw tasks fail until their story tasks land)**

- [X] T009 [US1] Kind + eval truth-table tests in tests/test_logic_buffer.cpp (byte 8 naming `TBUF<n>`, fan-in exactly 2, per-kind counters never reused) and kind-table pins in tests/test_logic_seq_kinds.cpp (14 rows, `TBUF` parse/name/ordinal) against the independently derived EN×D table (research D2)
- [X] T010 [US1] Netlist-seam buffer tests in tests/test_logic_buffer.cpp (wire data/enable with full validation, enabled drives data identically to an ordinary driver, released contributes high-Z, X/Z/dangling enable and data fail loudly with state/timing untouched, buffer as shared-net attachment within the cap)

### Implementation for User Story 1

- [X] T011 [P] [US1] Accept `add TBUF [--inputs 2]` in src/cli/commands/logic/add.cpp (kind want-strings, usage text with pin contract 1 = data / 2 = enable active-high, `kCompleteKinds` + completion) and extend `LogicAddSpec::kUsage` in include/wengine/cli/logic_registry.hpp
- [X] T012 [P] [US1] Render the TBUF schematic symbol (BUF triangle + enable side-stub) in src/cli/logic_draw_sdl.cpp and src/cli/logic_draw_imgui.cpp (research D7; layout boxes are input-count-sized — verify kind-agnostic at implementation)

**Checkpoint**: US1 fully functional and independently testable —
quickstart Scenario 1 passes; buffer-free flows byte-identical.

---

## Phase 4: User Story 2 — Defined idle level via per-net pulls (Priority: P1)

**Goal**: A per-input pull (up/down/none, default none) mapping
all-floating nets to an idle level, with pulled dangling inputs
terminated out of the block interface.

**Independent Test**: Park a bus (all drivers high-Z) and assert
pull-up reads high, pull-down reads low, none reads high-Z — while a
driven or contended net under any pull resolves exactly as without it
(spec US2) — runnable via quickstart Scenarios 2–4.

### Tests for User Story 2

- [X] T013 [US2] Pull-mapping tests in tests/test_logic_buffer.cpp (all-Z + up → High, all-Z + down → Low, all-Z + none → HighZ; driven agreement, low-vs-high contention with count, and unknown-dominance inert under every pull setting; permutation invariance)
- [X] T014 [US2] Termination + disjointness tests in tests/test_logic_buffer.cpp (pulled dangling excluded from interface derivation and resolved to idle without consuming a block-input column, `poke` refusal, tie/pull and ignore/pull refusals in both directions, unwire-to-dangling keeps the stored pull)

### Implementation for User Story 2

- [X] T015 [US2] Declare `LogicPullSpec` / `LogicUnpullSpec` in include/wengine/cli/logic_registry.hpp (appended in registry order), implement `pull <TO.i> <up|down>` in src/cli/commands/logic/pull.cpp (new file, added to `wengine_cli` sources), update `LogicSpec::kUsage` verb list
- [X] T016 [US2] Implement `unpull <TO.i>` in src/cli/commands/logic/unpull.cpp (new file, added to `wengine_cli` sources) with lifecycle refusals mirroring `unshare` (research D5)
- [X] T017 [P] [US2] Annotate pulled inputs (net + level) in `list` output in src/cli/commands/logic/list.cpp, byte-identical for pull-free workspaces
- [X] T018 [P] [US2] Render high-Z block outputs explicitly as `Z` in src/cli/commands/logic/solve.cpp, src/cli/commands/logic/step.cpp, and src/cli/commands/logic/common.cpp (`bit_of`), reachable only with buffers (research D4, D8)

**Checkpoint**: US1 + US2 work independently — quickstart Scenarios
2–4 pass; buffer/pull-free outputs byte-identical.

---

## Phase 5: User Story 3 — One funnel, optimizer soundness (Priority: P1)

**Goal**: Both mechanisms resolve through the single shipped funnel
with no second resolver, and every optimizer pass stays sound,
idempotent, and loud on buffer/pull netlists.

**Independent Test**: Exhaustive gated-span verification (every
enabled/disabled × ordinary mix up to the cap, every pull setting,
all attachment orders) resolves per the shipped table + two
float-mapping rows with identical results and counts (spec US3) —
plus optimize-of-optimize idempotence on a buffer/pull netlist.

### Tests for User Story 3

- [X] T019 [US3] Exhaustive gated-span tests in tests/test_logic_buffer.cpp (enabled/disabled/ordinary mixes up to 4 attachments with polarity mixes incl. self-contention shapes, every pull setting, permutation invariance of values and counts, single-driver-no-pull path bit-identical with benchmark cover in Phase 8)

### Implementation for User Story 3

- [X] T020 [P] [US3] Add `has_pulled_input()` beside `has_marked_input` and extend the conservative whole-net skip (exact flavor) to buffers/pulls in include/wengine/logic/optimize_barrier.hpp (research D7)
- [X] T021 [P] [US3] Skip `kTbuf` gates in `const_fold_pass` + root-collapse and skip gates with pulled inputs (sink-side, flat-keyed `pull_cfg` rationale) in include/wengine/logic/optimize_passes.hpp; document the `classify` default-arm `kV` hold for `kTbuf`
- [X] T022 [P] [US3] Refuse O2/O2-min loudly on live buffers or pulls in include/wengine/logic/optimize.hpp and add the unreachable loud-failure `kTbuf` arm in include/wengine/logic/optimize_o2_emit.hpp (research D7)
- [X] T023 [US3] Refuse `truth` / `export --truth` loudly on live-buffer netlists via `has_buffer_gates()` at the three word-loop entries in src/cli/commands/logic/truth.cpp and src/cli/commands/logic/common.cpp (`write_truth_csv`, `write_truth_pretty`) — pull-only netlists stream unchanged

**Checkpoint**: All P1 stories independently functional —
quickstart Scenarios 3 and 6 (O0/O1 + refusal halves) pass.

---

## Phase 6: User Story 4 — Save, reload, keep every flow bit-identical (Priority: P2)

**Goal**: `tbuf` rows and the `pulls:` section round-trip
byte-identically; every buffer/pull-free flow behaves exactly as
before.

**Independent Test**: Save a marked two-driver gated bus with pulls,
reload, and assert identical kinds, wirings, marks, attachments,
pulls, and replay outputs; full pre-existing suite green (spec US4) —
runnable via quickstart Scenario 5.

### Tests for User Story 4

- [X] T024 [US4] YAML round-trip tests in tests/test_logic_buffer.cpp (save → load → replay byte-identical incl. `pulls:` + `tbuf` rows) and unchanged-refusal pins in tests/test_logic_yaml.cpp (pre-012 files load byte-identically, duplicate `to:` without mark, tied/ignored pull destinations, unknown levels/keys fail loudly)

### Implementation for User Story 4

- [X] T025 [US4] Emit/parse the additive `pulls:` section (`- {net: PIN, pull: up|down}`, canonical order `version, gates, wires, shared, pulls, ties, clocks, meta`) and lowercase `tbuf` kind rows in include/wengine/logic/yaml_store.hpp + src/logic/yaml_store.cpp, with loud loader validation replayed through the engine seams (contracts/workspace-yaml.md)

**Checkpoint**: US4 complete — quickstart Scenario 5 passes; no
retired generations, marker stays `version: 1`.

---

## Phase 7: User Story 5 — One vocabulary on every driving surface (Priority: P2)

**Goal**: `pull`/`unpull`/`add TBUF` covered by completion and
byte-identical across headless/interactive front ends; registry,
example, and audit pins synced.

**Independent Test**: Drive mark/wire/pull/unpull through session
verbs (same acceptance as the engine seam), through interactive forms
(byte-identical to headless), and completion over the new names/forms
(spec US5).

### Tests for User Story 5

- [X] T026 [US5] Registry + CLI golden tests in tests/test_cli_args.cpp (`LogicRegistry::all().size() == 25`, per-verb `find` blocks, `verbs.size()` asserts, `p`-prefix gains `pull`, `u`-prefix gains `unpull`, `add` TBUF want-strings/completion, `pull`/`unpull` completion vectors, headless/interactive parity pins)

### Implementation for User Story 5

- [X] T027 [P] [US5] Optimizer idempotence + parity verification tests for buffer/pull netlists (optimize-of-optimize identical, O2/word-path loud refusals, TUI byte-parity via `TuiExecutor` → `cli::Shell`) in tests/test_logic_buffer.cpp
- [X] T028 [P] [US5] Add examples/logic/buffer_bus.yaml (gated bus with pulls — joins the automatic `test_examples` golden table)

**Checkpoint**: All user stories independently functional —
quickstart Scenario 6 fully passes.

---

## Phase 8: Polish & Cross-Cutting Concerns

**Purpose**: Living-spec duties, measurement close-out, full-suite
validation.

- [X] T029 [P] Documentation updates: docs/logic.md (§9 termination note, §10 `pull`/`unpull` + TBUF grammar, §11 drop tri-state/pull non-goals, §12 shipped buffer/pull + O2 follow-on, schema notes for additive `pulls:`), docs/tests/logic.md (new binary page), README.md (verb sentence, runnable bus example, module table, test list)
- [X] T030 Run quickstart.md Scenarios 1–7 end-to-end, full `ctest --test-dir build --output-on-failure`, and the post-change `./build-bench/bench_logic 8 8 2000` vs the T001 baseline (apply research D11 contingency and re-measure if outside noise)
- [X] T031 Confirm `docs/audit_catalogs.py` zero-missing/zero-ghost (new binary, example, verbs) and zero new compiler warnings (`-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wnon-virtual-dtor` on `wengine_cli`)

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies — starts immediately (T001 baseline before any code change).
- **Foundational (Phase 2)**: Depends on Setup — BLOCKS all user stories (kind, storage, and funnel integration are the floor every story stands on).
- **User Stories (Phase 3–7)**: All depend on Foundational completion; US1 → US2 → US3 → US4 → US5 in priority order, each independently testable at its checkpoint. US4/US5 are P2 and may lag the P1 cut.
- **Polish (Phase 8)**: Depends on all desired user stories being complete.

### User Story Dependencies

- **US1 (P1, buffer)**: After Foundational — no story dependencies. **MVP scope**: Setup + Foundational + US1 (a disconnecting driver; floating nets fail loudly — pulls arrive in US2).
- **US2 (P1, pulls)**: After Foundational — integrates with US1 constructs (gated bus parking) but testable on ordinary nets alone.
- **US3 (P1, funnel/optimizer)**: After Foundational — verifier + optimizer policy over US1/US2 constructs; no new vocabulary.
- **US4 (P2, persistence)**: After US1–US3 constructs exist — round-trips them.
- **US5 (P2, vocabulary)**: After US1–US4 — pins the complete surface.

### Within Each User Story

- Tests written FIRST and observed to FAIL before the story's implementation tasks.
- Engine seam before CLI verbs before readback/docs within a story.
- New `GateKind`/schema bytes land only in Foundational/US4 — never in story phases.
- Story checkpoint (quickstart scenario) green before moving to the next priority.

### Parallel Opportunities

- T003 ‖ T004 (different headers, no shared edits).
- T011 ‖ T012 (CLI add vs draw backends, different files).
- T017 ‖ T018 (list annotations vs Z rendering, different files).
- T020 ‖ T021 ‖ T022 (barrier, passes, O2 gate/emit — different files).
- T027 ‖ T028 (tests vs example YAML, different files).
- T029 ‖ (T030, T031) — docs vs measurement/audit runs.
- Once Foundational completes, stories can be staffed in parallel (US1+US2 first by priority); within a story, [P] tasks run together.

---

## Parallel Example: User Story 2

```bash
# Tests first (sequential — same binary file):
Task: "Pull-mapping tests in tests/test_logic_buffer.cpp"            # T013
Task: "Termination + disjointness tests in tests/test_logic_buffer.cpp" # T014

# Then implementation in parallel (different files):
Task: "LogicPullSpec/LogicUnpullSpec + pull.cpp in include/wengine/cli/logic_registry.hpp + src/cli/commands/logic/pull.cpp" # T015
Task: "list pull annotations in src/cli/commands/logic/list.cpp"      # T017 [P]
Task: "Z output rendering in solve.cpp + step.cpp + common.cpp"       # T018 [P]
# Then: unpull.cpp (depends on T015 registry structs)                 # T016
```

## Parallel Example: User Story 3

```bash
# One test task, then three parallel implementation tasks (different files):
Task: "has_pulled_input + exact skip in optimize_barrier.hpp"         # T020 [P]
Task: "const_fold/collapse skips in optimize_passes.hpp"              # T021 [P]
Task: "O2 refusal + o2_emit arm in optimize.hpp + optimize_o2_emit.hpp" # T022 [P]
# Then: truth/export entry guards (depends on has_buffer_gates)       # T023
```

---

## Implementation Strategy

### MVP First (P1 buffer core)

1. Complete Phase 1 (Setup — baseline recorded).
2. Complete Phase 2 (Foundational — kind, storage, funnel).
3. Complete Phase 3 (US1 — disconnecting driver).
4. **STOP and VALIDATE**: quickstart Scenario 1 + `ctest`; buffer-free flows byte-identical.
5. Demo: gated bus with loud float (pulls not yet present).

### Incremental Delivery

1. Setup + Foundational → engine floor ready.
2. + US1 → MVP: disconnecting driver (loud floats).
3. + US2 → usable buses: idle levels, termination, two new verbs.
4. + US3 → proven sound: exhaustive spans, optimizer guarantees.
5. + US4 → real designs: versioned persistence.
6. + US5 → coherent surface: completion, parity, example.
7. + Polish → living spec + measured fast path.

### Parallel Team Strategy

With multiple developers: team completes Setup + Foundational
together; then Developer A takes US1+US2 (constructs + verbs),
Developer B takes US3 (spans + optimizer), Developer C takes
US4+US5+Polish (schema, surface, docs) — stories merge in priority
order at their checkpoints.

---

## Notes

- [P] tasks = different files, no dependencies; same-file tasks stay sequential.
- [Story] label maps each story-phase task to its spec user story for traceability.
- FR-013 constraint quoted into tasks: T009/T013/T019 verify against independently derived tables, never the implementation's output.
- Data-model constraints quoted into tasks: `pull_cfg` values 0 = none / 1 = up / 2 = down (T003); canonical section order `version, gates, wires, shared, pulls, ties, clocks, meta` (T025); no `none` rows written (T025).
- Stop at any checkpoint to validate the story independently; never commit/push/branch unless explicitly requested.
