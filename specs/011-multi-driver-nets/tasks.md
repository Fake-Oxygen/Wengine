---

description: "Task list template for feature implementation"
---

# Tasks: Allowlisted Multi-Driver Nets with Loud Contention Counting

**Input**: Design documents from `/specs/011-multi-driver-nets/`

**Prerequisites**: plan.md (required), spec.md (required), research.md, data-model.md, contracts/

**Tests**: Included — plan.md §Technical Context explicitly requires the new `test_logic_shared` binary plus extensions to `test_logic.cpp`, `test_cli_args.cpp`, `test_logic_yaml.cpp` (constitution III independent-reference testing: engine seam → verbs → YAML round-trip).

**Organization**: Tasks are grouped by user story to enable independent implementation and testing of each story.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3)
- Include exact file paths in descriptions

## Path Conventions

- Engine headers: `include/wengine/logic/*.hpp`
- CLI verbs: `src/cli/commands/logic/*.cpp`, registry `include/wengine/cli/logic_registry.hpp`
- YAML store: `include/wengine/logic/yaml_store.hpp` + `src/logic/yaml_store.cpp`
- Tests: `tests/test_*.cpp`, registered in `CMakeLists.txt`
- Example: `examples/logic/shared_bus.yaml`

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Baseline measurements and green-tree confirmation before any change

- [X] T001 Record `bench_logic` fast-path baseline in `benchmarks/bench_logic.cpp` (run `./build-bench/bench_logic 8 8 2000`, save ns/gate-word; research D11 protocol) — BASELINE: 38.39 / 38.15 / 39.57 ns/gate-word (checksum 13247722040876395946)
- [X] T002 [P] Confirm baseline green via `ctest --test-dir build --output-on-failure` (no code change; record failures if any) — 74/74 PASS

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Additive storage + diagnostic channel type that ALL user stories build on; no behavior change

**⚠️ CRITICAL**: No user story work can begin until this phase is complete

- [X] T003 Add shared-net storage to `Netlist` in `include/wengine/logic/types.hpp`: `kMaxSharedExtra = 3` constant plus `shared_meta` (`std::array<std::uint8_t, kMaxGates * kMaxInputs>`, "bit0 = mark, bits1..2 = extra count", byte value 0 = unmarked fast test), `extra_drivers` (`std::array<std::uint16_t, kMaxGates * kMaxInputs * kMaxSharedExtra>`), `extra_inv` (`std::array<std::uint8_t, kMaxGates * kMaxInputs * kMaxSharedExtra>`); `Gate` unchanged (40 B); zero-initialized by `Netlist{}`
- [X] T004 [P] Define `SolveDiag` POD diagnostic channel in `include/wengine/logic/block_interface.hpp`: `std::array<std::uint32_t, kMaxGates * kMaxInputs> per_net`, zeroed per session, never serialized
- [X] T005 Register new test binary skeleton `tests/test_logic_shared.cpp` in `CMakeLists.txt` (`add_executable` + `target_link_libraries wengine_cli wengine_logic_yaml` + `add_test`; inherits `wengine_test_assert` via `test_` prefix)

**Checkpoint**: Foundation ready — `Netlist` carries additive storage, `SolveDiag` type exists, empty test binary builds; unmarked netlists byte-identical — user story implementation can now begin

---

## Phase 3: User Story 1 - Opt a net into multi-driver use (Priority: P1) 🎯 MVP

**Goal**: Per-input explicit opt-in gate — unmarked occupied refusal byte-identical to today; marked inputs accept up to cap 4; unmark refused while ≥2 attached

**Independent Test**: Wire second driver onto unmarked occupied input → loud refusal, netlist byte-identical; mark + rewire → success; unmark with 2 attached → loud refusal; unwire to 1 + unmark → success (no resolution/counting involved)

### Tests for User Story 1 ⚠️

> **NOTE: Write these tests FIRST, ensure they FAIL before implementation**

- [X] T006 [P] [US1] Engine authoring-gate tests in `tests/test_logic_shared.cpp`: unmarked occupied `wire()` refuses `kInvalidArgument` with netlist untouched; mark-then-wire succeeds; `unshare` with ≥2 attachments refuses; `unshare` at ≤1 succeeds; cap overflow at 4 attachments refuses loudly; 0-driver marked input legal
- [X] T007 [P] [US1] CLI authoring goldens in `tests/test_cli_args.cpp`: `LogicRegistry::all().size() == 23` (was 22), `wire --shared` usage/option forms, `unshare` verb find-block + `verbs.size()` asserts, `logic u` completion vector gains `unshare` in registry order

### Implementation for User Story 1

- [X] T008 [US1] Implement mark lifecycle + shared wire branches in `include/wengine/logic/netlist.hpp`: `wire()` keeps loud refusal on unmarked occupied (byte-identical), appends extras on marked (per-wire validation: "live slots, input range, untied destination, flop-aware cycle check (`topo::reaches`, `kCombOnly`), cap (loud at 4 attachments)"); `unwire()` promotes extra 0 to primary / left-compacts extras ("Primary exists iff the input has ≥1 attachment", "Extras are dense: indices `0..count-1`, no holes"); `remove_gate()` sweeps extras as-driver polarity-aware, slot marks die with gate
- [X] T009 [US1] Implement `share`/`unshare` engine functions in `include/wengine/logic/netlist.hpp`: mark legal on unoccupied (pre-mark) + idempotent on marked, refused on tied inputs ("marks apply to wired-driver inputs only"); `unshare` refused loudly while extra count > 0, refused on unmarked input
- [X] T010 [US1] Add `--shared` flag to wire verb in `src/cli/commands/logic/wire.cpp`: marks-then-attaches on unmarked, idempotent attach on marked, plain `wire` on marked attaches naming shared state, sequential tentative-probe learns extra-append branch, `kOpts` + completion gains `--shared`, cap overflow usage error exit 2; vacuous-settle early-out in `seq_step.hpp` (`apply_async_controls`/`apply_level_latches` succeed trivially without seq gates so contention never refuses authoring — FR-004; seq control nets keep the SR-illegal-style loud refusal)
- [X] T011 [US1] Add `unshare` verb in `src/cli/commands/logic/unshare.cpp` (new file) + spec/registration in `include/wengine/cli/logic_registry.hpp` (`LogicUnshareSpec`, append to `LogicRegistry`, `LogicSpec::kUsage` verb list); usage errors exit 2 ("unwire extras first" / "input is not shared"); completion over input pins via `collect_pins`
- [X] T012 [US1] Show shared marks + attachment counts in `src/cli/commands/logic/list.cpp`; unshared netlists list byte-identically

**Checkpoint**: At this point, User Story 1 should be fully functional and testable independently

---

## Phase 4: User Story 2 - Resolve N:1 nets through the shipped funnel (Priority: P1)

**Goal**: Every attached driver contributes its polarized value to the unchanged single `resolve_net` funnel; agreement → level, low+high → X, empty → Z; single-driver path bit-identical

**Independent Test**: Evaluate spans `{low,low}`→low, `{low,low,low}`→low, `{low,high}`→X, one driver→level, zero→Z, Q+NQ same source→X — against the independent reference table (specs/010 contracts/resolve-net.md), never implementation output; all attachment orders identical

### Tests for User Story 2 ⚠️

- [X] T013 [P] [US2] Span-resolution unit tests beside the funnel region in `tests/test_logic.cpp`: exhaustive L/H multisets over 2..4 attachments, per-attachment polarity mixes including Q+NQ self-contention, permutation invariance, empty-span → `kHighZ`, single-attachment → pre-feature value bit-identical

### Implementation for User Story 2

- [X] T014 [US2] Assemble shared span in `resolve_input` in `include/wengine/logic/block_interface.hpp`: when `shared_meta[flat]` reports extras, fold primary (`driver_inv` polarity) + extras (`extra_inv` polarity) in compact index order on a stack buffer (cap 4) through unchanged `resolve_net`; `n ≤ 1` path stays structurally today's code (unmarked nets never assemble a span)
- [X] T015 [US2] Thread defaulted `SolveDiag* diag = nullptr` through `solve()` in `include/wengine/logic/eval.hpp` and `seq_step_filtered()` in `include/wengine/logic/seq_step.hpp`; all existing callers compile and behave identically with null channel
- [X] T016 [US2] Refuse word-parallel path loudly in `include/wengine/logic/block_interface.hpp` (`eval_block_word`/`resolve_word` guard on `shared_meta[flat]` byte test when any shared input holds ≥2 attachments) + `truth` verb refusal in `src/cli/commands/logic/truth.cpp` naming the construct and pointing at scalar `solve`; ≤1-attachment marks ride the primary word path unchanged — measured refinement: refusal hoisted from per-word to once-per-word-loop-entry (`truth.cpp`, `support::write_truth_*`, `bench_logic.cpp`) via `shared_word_blocked`, because the per-word scan regressed bench ~15% (D11 protocol; loudness preserved at every entry, hot path branch-identical)

**Checkpoint**: At this point, User Stories 1 AND 2 should both work — buses resolve wired consensus/conflict through the shipped table

---

## Phase 5: User Story 3 - Contend loudly: counted, visible, diagnostic-only (Priority: P1)

**Goal**: Every low-vs-high contended resolution increments the per-net count by exactly one; visible in solve/step status; cleared by reset; never serialized; deterministic

**Independent Test**: Drive shared bus into disagreement N times → readback count exactly N; agreeing runs → 0; reset → cleared; saves byte-identical with/without history; identical runs → identical count sequences

### Tests for User Story 3 ⚠️

- [X] T017 [P] [US3] Contention-count tests in `tests/test_logic_shared.cpp`: contended resolution increments `per_net[flat]` by exactly one with value X; agreeing span leaves count unchanged; two identical runs give identical count sequences; save bytes identical with/without contention history

### Implementation for User Story 3

- [X] T018 [US3] Count at span assembly in `include/wengine/logic/block_interface.hpp`: compute `saw_low && saw_high` across the polarized span before/regardless of `resolve_net`, increment `diag->per_net[flat]` when contended and channel present (null channel = identical bytes)
- [X] T019 [US3] Own session `SolveDiag` beside `poke_map`/`cycle_counts` in `src/cli/commands/logic.cpp` anonymous-namespace singletons; clear in `reset_seq_session_state()` wired from `src/cli/commands/logic/reset.cpp` (never serialized)
- [X] T020 [US3] Append contention status line in `src/cli/commands/logic/solve.cpp` when any input marked (`contention: <NET>=<N> (total <T>)` with zeros so counting is visibly live); byte-identical output when none marked
- [X] T021 [US3] Append same contention line to per-step report in `src/cli/commands/logic/step.cpp`; counts accumulate across session; engine `Runner`/`SequentialSolver` path attaches no channel

**Checkpoint**: At this point, contention is loud — every bus fight visible in health/readback without watching net values

---

## Phase 6: User Story 4 - Save, reload, keep every v1 flow bit-identical (Priority: P2)

**Goal**: Marks + attachments round-trip save→load→replay byte-identically; additive schema (`shared:` section, repeated wire rows), marker stays `version: 1`; all mark-free flows byte-identical

**Independent Test**: Build marked 2-driver netlist, save, reload → identical marks/attachments/replay; pre-existing logic suite + saved-workspace refusals all pass unchanged

### Tests for User Story 4 ⚠️

- [X] T022 [P] [US4] Shared round-trip + refusal tests in `tests/test_logic_yaml.cpp`: marks + repeated wires survive save→load→save byte-identically; zero-attachment marks round-trip; unmarked duplicate `to:` keeps existing refusal; new loud rows (unknown key in `shared:`, duplicate `net:`, cap exceed, marked-also-tied); existing `test_canonical_bytes_v1` + `test_examples` stay green untouched

### Implementation for User Story 4

- [X] T023 [US4] Declare additive shared schema in `include/wengine/logic/yaml_store.hpp` (platform-free header): optional `shared:` section, repeated `to:` rows legal iff marked
- [X] T024 [US4] Emit + load shared constructs in `src/logic/yaml_store.cpp`: save `shared:` iff ≥1 mark in canonical order `version, gates, wires, shared, ties, clocks, meta`; loader accepts `shared` root key (unknown keys stay loud), absent section = no marks, `net:`-only rows resolved to live inputs, repeated `to:` validated per attachment (live slots, direction, untied, per-edge flop-aware cycle, cap 4), file-order replay, atomic load via temporary netlist
- [X] T025 [US4] Add `examples/logic/shared_bus.yaml` golden (marked bus + agreeing drivers) joining the `test_examples` golden table in `tests/test_logic_yaml.cpp`

**Checkpoint**: Shared designs persist; every v1 flow untouched

---

## Phase 7: User Story 5 - One vocabulary across every driving surface (Priority: P2)

**Goal**: CLI verbs match engine seam exactly; completion covers new names/forms; headless/interactive byte-identical by construction; optimizer sound on shared nets (freeze/skip, O2 loud refusal, idempotent)

**Independent Test**: Same mark/wire/unmark ops through session verbs → same accept/refusals as engine seam; interactive forms → byte-identical output; every optimizer pass over shared netlist → resolution results unchanged + idempotent

### Tests for User Story 5 ⚠️

- [X] T026 [P] [US5] Optimizer soundness tests in `tests/test_logic_shared.cpp`: O0/O1 over shared netlist leave resolution results unchanged and are idempotent; O2 refuses loudly with netlist untouched; shared-feeding gates survive DCE/dedup/double-inv passes

### Implementation for User Story 5

- [X] T027 [US5] Add `drives_shared` freeze predicate + driver-side freeze guards in `include/wengine/logic/optimize_passes.hpp`: shared-feeding gates never removed/retagged/rewired-through (guards at transform decision points, never inside `rewire_users`); DCE treats them as roots (barrier-root precedent)
- [X] T028 [US5] Sink-side skip + conservative `exact_simplify_pass` whole-net skip in `include/wengine/logic/optimize_exact.hpp` (`classify` marks shared nets variable) + O2/O2-min loud refusal in `src/cli/commands/logic/optimize.cpp` (exit 1, workspace untouched, O0/O1 available)
- [X] T029 [US5] Extras-aware machinery fixes: `recount_fanout` counts extra edges, `follow_drivers`/`compute_feeds_seq` traverse extras, `remove_gate` sweeps extras as-driver, `dedup_key_for` never keys frozen gates, `netlists_equal` test oracle compares marks + extras (files: `include/wengine/logic/optimize_passes.hpp`, `include/wengine/logic/netlist.hpp`, `include/wengine/logic/topo.hpp`)
- [X] T030 [US5] Sync completion + registry pins in `tests/test_cli_args.cpp`: `logic wire --` completes `--shared`, `logic unsh` completes `unshare`, `logic unshare --` completes `--help`/`-h`, no `--opt=` value forms added
- [X] T031 [US5] Living-spec docs: `README.md` verb sentence + runnable example + test list entry, `docs/logic.md` §9 single-driver opt-in + §10 verb grammar (`unshare`, `wire --shared`) + §11 drop multi-driver non-goal + §12 Drivers slot shipped authoring with O2 follow-on, `docs/tests/logic.md` new-binary page (catalog audit stays zero-missing/zero-ghost)

**Checkpoint**: Front ends coherent, optimizer sound, docs ship with the feature

---

## Phase 8: Polish & Cross-Cutting Concerns

**Purpose**: Full-suite validation, measured fast-path claim, quickstart proof

- [X] T032 Run full suite `ctest --test-dir build --output-on-failure` in repo root + validate `specs/011-multi-driver-nets/quickstart.md` scenarios 1–7 end-to-end
- [X] T033 [P] Re-run `./build-bench/bench_logic 8 8 2000` and compare to T001 baseline in `benchmarks/bench_logic.cpp` context: accept within run-to-run noise; if measurable, apply research D11 contingency (pack mark/count into `driver_inv` spare bits) and re-measure
- [X] T034 [P] Run `docs/audit_catalogs.py` (zero missing / zero ghost) + docs build with Doxygen coverage for new public APIs (`share`/`unshare`/`SolveDiag` `@brief` comments)

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies — start immediately (T001, T002 parallel)
- **Foundational (Phase 2)**: Depends on Setup — BLOCKS all user stories (T003, T004 parallel; T005 after T003/T004 headers exist)
- **User Stories (Phase 3+)**: All depend on Foundational completion
  - US1 → US2 → US3 sequential preferred (authoring → resolution → counting builds naturally), but US2/US3 can start in parallel after Foundation if staffed (different files: `netlist.hpp` vs `block_interface.hpp` span vs `solve.cpp`/`step.cpp` readback)
  - US4, US5 depend on Foundation only; US5 optimizer work needs US1 authoring + US2 resolution headers stable
- **Polish (Phase 8)**: Depends on all desired user stories being complete

### User Story Dependencies

- **User Story 1 (P1)**: Can start after Foundational — no dependencies on other stories
- **User Story 2 (P1)**: Needs Foundational storage; integrates with US1 attachments but testable via direct engine seam
- **User Story 3 (P1)**: Needs US2 span assembly (counting hooks into it) + session singletons
- **User Story 4 (P2)**: Needs US1 marks/attachments shape; independently testable via YAML round-trip
- **User Story 5 (P2)**: Needs US1 verbs + US2 resolution + extras machinery; optimizer + docs close last

### Within Each User Story

- Tests (T006/T007, T013, T017, T022, T026) written FIRST and FAIL before implementation
- Storage before seam functions before verbs before readback
- Core implementation before integration
- Story complete before moving to next priority

### Parallel Opportunities

- T001 + T002 (baseline bench + baseline ctest, no shared files)
- T003 + T004 (different headers: `types.hpp` vs `block_interface.hpp`)
- T006 + T007 (different test files: `test_logic_shared.cpp` vs `test_cli_args.cpp`)
- T013 can run parallel with US1 implementation (different files)
- T017 + T022 + T026 (three different test files, all after Foundation)
- T033 + T034 (bench re-run vs docs audit, no shared files)

---

## Parallel Example: User Story 1

```bash
# Launch authoring-gate tests together (different files, no dependencies):
Task: "Engine authoring-gate tests in tests/test_logic_shared.cpp" (T006)
Task: "CLI authoring goldens in tests/test_cli_args.cpp" (T007)

# Implementation sequence (same files chain — NOT parallel):
# T008 (netlist.hpp) → T009 (netlist.hpp share/unshare) → T010 (wire.cpp) → T011 (unshare.cpp + logic_registry.hpp) → T012 (list.cpp)
```

## Parallel Example: User Story 5 (optimizer + docs)

```bash
# Launch after US1/US2 headers stable (different files):
Task: "Optimizer soundness tests in tests/test_logic_shared.cpp" (T026)
Task: "Completion pins in tests/test_cli_args.cpp" (T030)
Task: "Living-spec docs: README.md + docs/logic.md + docs/tests/logic.md" (T031)
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1: Setup (T001–T002 baseline)
2. Complete Phase 2: Foundational (T003–T005 storage + channel + test skeleton)
3. Complete Phase 3: User Story 1 (T006–T012 authoring gate)
4. **STOP and VALIDATE**: quickstart Scenario 1 (unmarked refusal byte-identical) + Scenario 4 (unshare lifecycle)
5. Deploy/demo if ready — the safety gate alone has value (explicit opt-in vocabulary)

### Incremental Delivery

1. Complete Setup + Foundational → additive storage, no behavior change
2. Add US1 → authoring gate testable (mark/wire/unshare)
3. Add US2 → wired consensus/conflict resolves via shipped table (MVP expressive value)
4. Add US3 → contention loud in status (full P1 slice: gate + resolve + count)
5. Add US4 → persistence (designs survive save/load, v1 untouched)
6. Add US5 → optimizer soundness + docs (coherence, shippable)
7. Polish → bench + audit + quickstart green

### Parallel Team Strategy

With multiple developers:

1. Team completes Setup + Foundational together
2. Once Foundational is done:
   - Developer A: US1 (netlist.hpp + wire/unshare verbs)
   - Developer B: US2 (block_interface.hpp span + word refusal)
   - Developer C: US4 (yaml_store.cpp schema — needs only US1 shape, mockable)
3. US3 counting + US5 optimizer after A/B headers stabilize
4. Stories integrate via the pinned `shared_meta`/`extra_drivers` layout

---

## Notes

- [P] tasks = different files, no dependencies
- [Story] label maps task to specific user story for traceability
- Each user story independently completable and testable
- Data-model field constraints quoted verbatim in T003/T008 (bit layout, density, cap) — not left to implementation discretion
- Reference anchoring: T013 verifies against specs/010 `resolve-net.md` table, never implementation output (constitution I/III)
- Registry-size `static_assert` sync enumerated in T007/T030 (22→23 + completion vectors)
- Benchmark never links assertion policy; bench tasks (T001/T033) are measurement-only
- Stop at any checkpoint to validate story independently
- Avoid: vague tasks, same-file conflicts, cross-story dependencies that break independence

---

## Phase 9: Convergence

**Purpose**: Close verification gaps found by the converge assessment (all P1–P2 behavior already implemented; these tasks pin counting on the step path, session lifecycle, CLI readback shapes, sequential authoring, and extended span coverage).

- [X] T035 [US3] Engine test stepping a shared bus with `SolveDiag` in `tests/test_logic_shared.cpp`: agreeing step succeeds with count 0; contended comb-fed bus fails with count 1 (phase-1 site); contended flop-D-fed shared input fails with count 1 (snapshot site) per FR-007 (partial)
- [X] T036 [US3] CLI test that `reset` clears contention counts in `tests/test_cli_args.cpp`: contend a shared bus (count>0 visible), `reset`, rebuild, agreeing solve shows `total 0` per FR-009 (missing)
- [X] T037 [US3] CLI goldens for the `contention:` line shape in `tests/test_cli_args.cpp`: agreeing solve shows zero line, contended solve shows the line before the loud failure, agreeing `step` on a sequential shared workspace shows the line, marked single-driver `truth` streams (no refusal) per FR-008 (missing)
- [X] T038 [US1] End-to-end shared-wire test on a sequential netlist in `tests/test_cli_args.cpp`: `--shared` append through the tentative-probe path succeeds, and a cycle closing through an extra is refused loudly with the netlist untouched per FR-004 (partial)
- [X] T039 [US2] Extend through-seam span tests in `tests/test_logic_shared.cpp` to 3- and 4-attachment multisets (all-agree, mixed, inverted extras) plus reversed-attachment-order count equality per SC-001 (partial)
- [X] T040 [US3] Emit the contention line on the `step` no-op path in `src/cli/commands/logic/step.cpp` when marked inputs exist per FR-008 (partial)
