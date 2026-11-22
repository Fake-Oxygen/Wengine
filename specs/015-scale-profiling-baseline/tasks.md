# Tasks: Measured Scale-Profiling Baseline for One-Million-Gate Logic Nets (015)

**Input**: Design documents from `/specs/015-scale-profiling-baseline/`
**Prerequisites**: plan.md (required), spec.md (required), research.md, data-model.md, quickstart.md, contracts/
**Tests**: INCLUDED — explicitly required by FR-010/SC-001 (scale-instantiation parity in `test_logic_scale`) and FR-009/SC-004 (pre-feature suite as regression pin). `test_logic_scale` links `wengine_test_assert` via the `test_` prefix; benchmarks never link the assertion policy.

**Organization**: Tasks grouped by user story. US1+US2 are P1 (the deliverable), US3+US4 are P2, US5 is P3.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (US1–US5)
- Include exact file paths in descriptions

## Path Conventions

- Engine: `include/wengine/logic/*.hpp` (header-only `INTERFACE` libs, no new TUs)
- Harness: `benchmarks/bench_logic_scale.cpp` (links `wengine_core` only)
- Tests: `tests/test_logic_scale.cpp` (links `wengine_core` + `wengine_test_assert` via `test_` prefix)
- Docs: `docs/benchmarks/logic-scale.md`, `docs/tests/logic.md`, `README.md`, `CMakeLists.txt` at repo root

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Pre-change baselines, idiom study, build configuration — no production code changes.

- [X] T001 Inspect logic touchpoint inventory in `include/wengine/logic/*.hpp` (26 headers, ~366 `std::uint16_t` slot mentions per research) and record pre-change `sizeof(Gate)=40`, `sizeof(Netlist)=159840`, `kNoDriver=0xFFFF` constants as the before-numbers baseline (working notes only, no code change)
- [X] T002 [P] Study bench idiom in `benchmarks/bench_logic.cpp` and `bench_signal_scale` source (clamped positional args, argc-seeded xorshift, warmup separation, `do_not_optimize`, checksums, labeled scenario lines) and note the 2-line bench CMake block pattern in `CMakeLists.txt` (no code change)
- [X] T003 [P] Verify zero `wengine/logic` includes in `apps/gui/` and `src/tui/` via grep and confirm monomorphic consumer list compiles against aliases (`include/wengine/logic/yaml_store.hpp` + store TUs, `world/serialize.hpp`, `world/state.hpp`, `solvers/sequential.hpp`, CLI verb TUs) (no code change)
- [X] T004 Configure dev build (`cmake -S . -B build -DBUILD_TESTS=ON -DBUILD_BENCHMARKS=ON`) and bench preset (`cmake --preset bench`), capturing the bench preset's echoed `-O3 -DNDEBUG` flags for the later environment pin
- [X] T005 Run pre-feature baseline and save outputs: full `ctest --test-dir build --output-on-failure`, reference `bench_logic` lines, and `python3 docs/audit_catalogs.py` (zero missing / zero ghost)

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Capacity-templating root + build/docs registration that ALL user stories depend on.

**⚠️ CRITICAL**: No user story work can begin until this phase is complete.

- [X] T006 Template `include/wengine/logic/types.hpp` (TEMPLATE ROOT): `NetlistT<kCap>`/`PinRefT<kCap>`, dependent `slot_t` width ("u16 while addressable, else u32" per research D2), capacity-derived sentinels (`kNoDriver`, `kNoLeader = kCap*kMaxInputs`, `kBitScratchWords = kCap`), aliases `Netlist`/`PinRef`/`kMaxGates=1024` pinned type-identical, `kMaxInputs=8` and `kMaxSharedExtra=3` fixed, Doxygen `@brief`/params/`@file` role paragraph
- [X] T007 [P] Establish scratch-bundle convention used by all templated cores in `include/wengine/logic/*.hpp`: caller-owned non-owning spans sized by the data-model formulas (Topo: kCap×5; Eval scalar: kCap×kMaxInputs + kCap; Word: input-position map + gate word scratch + block outputs; Step: kCap×kMaxInputs + kCap×4; Optimize: kCap×kMaxInputs 16 B each + kCap×n + one out-buffer netlist), validated with "loud `Status` when too small, before any work" (eval.hpp:227–229 precedent); cores never allocate, never throw
- [X] T008 Register build targets in `CMakeLists.txt`: `bench_logic_scale` 2-line bench block linking `wengine_core` only inside `if(BUILD_BENCHMARKS)` plus one-line comment naming the measured risks, and `test_logic_scale` via `add_executable` + `target_link_libraries(... wengine_core)` + `add_test` (inherits `wengine_test_assert` policy via `test_` prefix)
- [X] T009 Create `docs/benchmarks/logic-scale.md` skeleton with the literal `## bench_logic_scale` CS3 five-field anchor (Measured path / Why it matters / How to run / Interpretation / Pitfalls) so `docs/audit_catalogs.py benchmarks` stays clean from here on
- [X] T010 [P] Create `docs/tests/logic.md` `## test_logic_scale` CS1 six-field entry skeleton plus the `docs/tests/index.md` domain-map row

**Checkpoint**: Foundation ready — `types.hpp` root compiles with identical aliases, build targets registered, docs anchors pass audit. User story implementation can now begin.

---

## Phase 3: User Story 1 - A million-gate net exists on the measured path (Priority: P1) 🎯 MVP

**Goal**: The logic domain constructs, wires, compiles, and evaluates nets of ≥1,000,000 live gates on the measured path (scale instantiation of the same shipping source), with every standing contract intact (single-driver, flop-aware cycle rejection, four-value funnel, never-reused per-kind counters, loud overflow).

**Independent Test** (spec US1 + quickstart Scenarios 2/4): build the scale net, run one scalar solve + one word-evaluated slice + one sequential step, assert scalar/word bit-exact parity, bit-identical repeats, prefix-slice agreement, and loud refusal on over-capacity — no sweep/report needed. Concrete: `ctest --test-dir build -R logic_scale` → `test_logic_scale: OK`; optional `WENGINE_LOGIC_SCALE_GATES=262144 ctest --test-dir build -R logic_scale`.

### Tests for User Story 1 (REQUIRED by FR-010 — write first, ensure FAIL before implementation)

- [X] T011 [P] [US1] Write alias-identity pins in `tests/test_logic_scale.cpp`: `static_assert`s on struct sizes, sentinel values (`0xFFFF`, `8192`, `1024`), and width spellings proving `Netlist`/`PinRef`/`kMaxGates` are type-identical to pre-feature types (contracts/engine-seam.md Alias guarantees §1)
- [X] T012 [P] [US1] Write scale-instantiation parity tests in `tests/test_logic_scale.cpp` at default 65,536 gates (past everything the u16 alias's 1,024 slots / 8,192 flats can address) with `WENGINE_LOGIC_SCALE_GATES=<n>` env override (0/invalid ⇒ default): scalar/word bit-exact parity, prefix-slice agreement vs standalone small net, bit-identical repeat solves, loud-overflow + single-driver/cycle refusal parity at scale

### Implementation for User Story 1 (template the stage path in include order; FR-008: no optimization, smallest coherent change)

- [X] T013 [P] [US1] Template `include/wengine/logic/shared_net.hpp` (slot params/loops Cap-bounded, same seam) in `include/wengine/logic/shared_net.hpp`
- [X] T014 [P] [US1] Template `include/wengine/logic/tie_set.hpp` (`TieSnapshot` capacity-typed; `survivors[]` scratch moves to caller-owned spans) in `include/wengine/logic/tie_set.hpp`
- [X] T015 [US1] Template `include/wengine/logic/block_interface.hpp` (`SolveDiag` capacity-typed; per-solve full-cap init stays as-is — suspect #6 is measured, not silently fixed) in `include/wengine/logic/block_interface.hpp` (depends on T013, T014 seam)
- [X] T016 [US1] Template `include/wengine/logic/topo.hpp` (Kahn core: `kahn_pure`/`rebuild`/`validate`/`reaches` take scratch spans for indeg/queue/order/levels/seen; alias wrappers keep exact signatures + `noexcept` per test_logic.cpp:808/816) in `include/wengine/logic/topo.hpp`
- [X] T017 [US1] Template `include/wengine/logic/pins.hpp` (structurally coupled `PinRef`/`Netlist` refs; text adapters stay alias-monomorphic) in `include/wengine/logic/pins.hpp`
- [X] T018 [US1] Template `include/wengine/logic/netlist.hpp` (mutation core add/remove/wire/unwire/ties: free-slot scan, full-pool sweeps, reachability probe unchanged in algorithm; text adapters `wire_text`/`tie_text` alias-monomorphic) in `include/wengine/logic/netlist.hpp`
- [X] T019 [US1] Template `include/wengine/logic/eval.hpp` (`solve`/`eval_block_word` scratch-taking cores; alias wrappers keep signatures with internal fixed stack scratch at 1024; `resolved`/`gate_out`/`in_pos` full-cap inits unchanged) in `include/wengine/logic/eval.hpp`
- [X] T020 [US1] Template `include/wengine/logic/seq_state.hpp` (`SeqStateT<kCap>` beside `NetlistT<kCap>`) and `include/wengine/logic/seq_step.hpp` (scratch-taking cores + alias wrappers) in `include/wengine/logic/seq_state.hpp` and `include/wengine/logic/seq_step.hpp`
- [X] T021 [US1] Template optimize family in `include/wengine/logic/optimize_barrier.hpp`, `include/wengine/logic/optimize_passes.hpp`, `include/wengine/logic/optimize_exact.hpp`, `include/wengine/logic/optimize_o2_build.hpp`, `include/wengine/logic/optimize_o2_dedup.hpp`, `include/wengine/logic/optimize_o2_min.hpp`, `include/wengine/logic/optimize_o2_emit.hpp`, `include/wengine/logic/optimize.hpp` (by-value `Netlist` locals become caller-provided out-buffers in cores; alias wrappers keep by-value convenience semantics; exact/min caps unchanged n≤10/m≤16, n≤8/m≤8)
- [X] T022 [US1] Leave monomorphic on the alias with zero changes (verify compile): `include/wengine/logic/timing.hpp`, `include/wengine/logic/layout.hpp`, `include/wengine/logic/seq_config.hpp`, `include/wengine/logic/yaml_store.hpp`, `include/wengine/logic/clock_domains.hpp`, `include/wengine/logic/four_state.hpp`, `include/wengine/logic/seq_symbol.hpp`, `include/wengine/logic/optimize_types.hpp`, world serialization, all CLI verbs, GUI/TUI
- [X] T023 [US1] Build and run `test_logic_scale` (`cmake --build build && ctest --test-dir build -R logic_scale`): all T011–T012 assertions pass at default 65,536 gates; full `ctest --test-dir build` shows no other test changed output (US4 pre-check)

**Checkpoint**: US1 complete — scale instantiation carries more than the alias ever could (65,536 > 1,024 slots) with parity pinned; ≥1M reach is the bench's deliverable (US2). FR-002 honest measurement holds: only `bench_logic_scale` and `test_logic_scale` TUs may instantiate non-alias capacities; no runtime capacity knob, no new user-facing vocabulary.

---

## Phase 4: User Story 2 - The recorded growth baseline (Priority: P1)

**Goal**: One shipped sweep entry (`bench_logic_scale`) produces per-stage, per-size costs over a ≥3-orders-of-magnitude ladder to ≥1M gates, committed as the baseline page with pinned environment + variance policy; re-runs reproduce within stated variance.

**Independent Test** (spec US2 + quickstart Scenarios 3–5): run the shipped sweep on the pinned environment and compare per-stage numbers against the committed baseline within stated variance; any over-budget stage reads `UNMEASURED-AT-SIZE` loudly; identical args re-run ⇒ identical checksums.

### Implementation for User Story 2

- [X] T024 [P] [US2] Implement motif builders in `benchmarks/bench_logic_scale.cpp`: plain layered (primary) + wide + deep-chain + mixed motif (shared nets + pulls + sequential elements + delay annotations for 011–014 coverage), forward-layer construction keeping reachable sets bounded, argc-seeded xorshift (identical args ⇒ bit-identical nets/checksums), harness builds by slot directly (no text adapters)
- [X] T025 [US2] Implement size ladder + stage timing in `benchmarks/bench_logic_scale.cpp`: geometric ladder with alias anchor points (≤1024, shipped u16 instantiation) + scale-instantiation points to ≥1,000,000; stages `growth-add`, `growth-wire`, `compile-rebuild` (forced dirty), `solve`, `word`, `shared-solve`, `step`, `optimize`, `dedup` (isolated pass), `remove`, `reset`; warmup separated from steady state; first-compile never averaged into steady solve; per-stage per-size wall budgets from D11 arithmetic (bulk-add ≈10¹²/2 probes; forced compile ≈8×10¹² visits; dedup/O2 at 1M expected over budget) with exceed ⇒ `UNMEASURED-AT-SIZE` loudly, sweep continues
- [X] T026 [US2] Implement output grammar + honesty machinery in `benchmarks/bench_logic_scale.cpp`: `bench_logic_scale <stage>: <size> gates x <n> ops, <median> ns/<unit> (min=<m>, fp=<footprint>, checksum=<c>)` lines, `do_not_optimize` opaque outputs, peak footprint per size, exit 1 on setup/run failure (`bench_logic_scale: setup failed (<reason>)`), arg contract `bench_logic_scale [motif] [min_size] [max_size] [steps] [words] [budget_s] [reps]` with clamped/zero-fallback defaults (quick default ladder = minutes)
- [X] T027 [US2] Run the quick default ladder (`./build-bench/bench_logic_scale`) plus the committed full-ladder invocation including the ≥1M point and the 1M mixed-motif reach (construct + solve + word + one budgeted step), and fill the per-stage result tables in `docs/benchmarks/logic-scale.md` (columns: size, median, min, samples, peak footprint, budget state ∈ {measured, unmeasured-at-size}); constraint quoted verbatim: "no extrapolated or estimated values may exist in a StageResult" (FR-006)
- [X] T028 [US2] Write the Environment + Variance-policy blocks in `docs/benchmarks/logic-scale.md`: host class (WSL2 baseline), GCC 13.3, bench preset + echoed flags, date, free-memory budget; steady-state stages median+min of R≥5, one-shot mutation stages single measurement with ladder spread noted, every row carries its sample count

**Checkpoint**: US1+US2 deliver the full P1 contract — reach plus the recorded, reproducible baseline (SC-001, SC-002).

---

## Phase 5: User Story 3 - Named attribution and the measured-hotspot gate (Priority: P2)

**Goal**: Every FR-005 suspect carries a measured verdict from its own size-doubling ratios; 1M-end costs ranked mutation/compile/eval-side; the gate paragraph makes hotspot-less SIMD/threading/GPU/rework proposals rejectable by citation alone.

**Independent Test** (spec US3 + quickstart Scenario 6): read `docs/benchmarks/logic-scale.md` and assert each named suspect carries a verdict grounded in its own measured ratios, the ranking follows those numbers, and the gate's citation requirement is stated operationally.

### Implementation for User Story 3

- [X] T029 [US3] Write the verdict table in `docs/benchmarks/logic-scale.md` for all six FR-005 suspects (Kahn per-pop full-pool successor sweep topo.hpp:159–193; per-add free-slot scan netlist.hpp:81–86; per-remove full-pool sweep netlist.hpp:185–204; per-wire reachability probe netlist.hpp:504→topo.hpp:446–496; all-pairs structural dedup optimize_o2_dedup.hpp:84–169; full-capacity init per solve/word/step eval.hpp:141–149, eval.hpp:234–235, block_interface.hpp:350–352): worst-case class from research + measured size-doubling ratios + verdict ∈ {confirmed-super-linear, cleared-within-variance}, citing StageResult rows only; cleared verdicts are legitimate outcomes, worst-case class stated beside measured actual per D6
- [X] T030 [US3] Write the 1M-end ranking in `docs/benchmarks/logic-scale.md`: top costs in ranked order split mutation-side / compile-side / evaluation-side (the kill-list order for follow-ons), derived from the committed tables, never from code reading
- [X] T031 [US3] Write the operational gate paragraph + fidelity note in `docs/benchmarks/logic-scale.md`: near-verbatim gate text ("any vectorization, threading, GPU, or structural-rework proposal for the logic domain must cite the measured number of the hotspot it attacks from this page, and must re-run this sweep as its before/after evidence; a proposal naming no measured hotspot is out of order until re-baselined") plus the D1 fidelity statement (u32 instantiation measured, u16 alias shipped, alias anchor points keep one foot in the shipped instantiation)

**Checkpoint**: US3 complete — the baseline is attributable and enforceable (SC-003 first half).

---

## Phase 6: User Story 4 - Nothing that ships today moves (Priority: P2)

**Goal**: Every pre-feature golden, replay, refusal, bench measurement, and save/load round-trip at the 1024 alias passes unchanged; docs/catalog duties ship in the same change.

**Independent Test** (spec US4 + quickstart Scenarios 1/7/8): full `ctest` byte-identical; `bench_logic` within stated noise; workspace round-trips identical; `audit_catalogs.py` zero missing / zero ghost.

### Implementation for User Story 4

- [X] T032 [US4] Run the full regression pin: `ctest --test-dir build --output-on-failure` passes with byte-identical outputs at the alias (goldens, example replays, refusal messages, save/load round-trips), and `./build-bench/bench_logic 64 16 20000` ns/gate-word + shared/rebuild lines match pre-feature T005 numbers within stated noise — proving FR-008 (no optimization) and FR-009 (nothing moves)
- [X] T033 [P] [US4] Update `README.md` in the same change: run list (L54–64 region), bench catalog paragraph (L839–855 region), benchmarks brace list + test-list line (L992 region) for `bench_logic_scale` / `test_logic_scale`
- [X] T034 [P] [US4] Update `docs/benchmarks/index.md`: Pages bullet + toctree entry for `logic-scale` plus the one-sentence carve-out ("committed numbers live only on the pinned-environment baseline page, which states its own reproduction terms") amending the never-commit-numbers habit per research D7
- [X] T035 [US4] Verify catalogs: `python3 docs/audit_catalogs.py` reports `benchmarks: N/N covered` and `tests: N/N covered` (zero missing, zero ghost) including the `## bench_logic_scale` and `## test_logic_scale` anchors

**Checkpoint**: US4 complete — the baseline's before/after meaning is preserved at the current scale (SC-004).

---

## Phase 7: User Story 5 - The harness outlives the baseline (Priority: P3)

**Goal**: The sweep stays the standing before/after instrument: the report documents a sufficient re-run procedure, and a no-change re-run lands within stated variance.

**Independent Test** (spec US5): re-run the sweep with no intervening change using only the documented procedure and assert every curve lands within the stated variance.

### Implementation for User Story 5

- [X] T036 [US5] Document the re-run procedure in `docs/benchmarks/logic-scale.md`: verbatim committed full-ladder invocation, bench preset + environment pinning, repetition/variance policy, expectation (within variance; unmeasured entries stay unmeasured), beside-not-in-place rule for foreign hosts/compilers
- [X] T037 [US5] Demonstrate reproduction once in the report's own reproduction section in `docs/benchmarks/logic-scale.md`: no-change re-run results vs committed numbers, every measured stage within stated variance, budget-exceeded entries reproducing as budget-exceeded — the follow-on before/after acceptance shape stated verbatim (targeted stage's curve changed, no other stage regressed beyond noise)

**Checkpoint**: All user stories independently functional — full feature contract delivered (SC-001…SC-004).

---

## Phase 8: Polish & Cross-Cutting Concerns

**Purpose**: Final verification and house-rule compliance.

- [X] T038 Run the complete quickstart validation in order (Scenarios 1–8 in `specs/015-scale-profiling-baseline/quickstart.md`) and fix any deviation
- [X] T039 [P] Doxygen + docs build: verify `@brief`/params/`@file` role paragraphs ship on all templated forms and scratch-taking overloads, then `docs/build.sh` passes with zero Doxygen warnings and no API coverage gaps
- [X] T040 Full suite + zero warnings: `ctest --test-dir build --output-on-failure` green, bench preset build shows no new compiler warnings (`-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wnon-virtual-dtor` on including TUs), `python3 docs/audit_catalogs.py` clean, working tree builds from scratch in both `build` and `build-bench`

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies — start immediately. Produces before-numbers (T005) that US4 compares against.
- **Foundational (Phase 2)**: Depends on Setup — BLOCKS all user stories. `types.hpp` root (T006) is the compile basis for every US1 header task.
- **User Stories (Phase 3+)**: All depend on Foundational completion.
  - US1 (P1) → US2 (P1) → US3 (P2): strict data order (substrate → numbers → verdicts). US2's bench cannot run before US1's headers compile; US3's verdicts cannot be written before US2's tables exist.
  - US4 (P2): validation of everything; runs after US1+US2 code is in place (needs final binaries), documents in T033–T035 are parallelizable earlier.
  - US5 (P3): depends on US2/US3 report content; T036 procedure text can draft in parallel with US2, T037 demonstration needs final numbers.
- **Polish (Phase 8)**: Depends on all desired user stories being complete.

### Within Each User Story

- US1: T011–T012 tests written FIRST and FAIL before T013–T021 implementation; headers in include order (T013→T015 chain, then T016–T021); T022 monomorphic check + T023 full run last.
- US2: motifs (T024) → ladder/stages (T025) → grammar (T026) → measured runs + tables (T027) → environment/variance blocks (T028).
- US3: tables must exist (US2) → verdicts (T029) → ranking (T030) → gate + fidelity (T031).
- US4/US5: evidence-gathering last (T032, T037).

### Parallel Opportunities

- Phase 1: T002, T003 parallel (read-only studies); T004+T005 sequential (build then measure).
- Phase 2: T007, T010 parallel with anything; T006 → T008 order for compile check.
- US1: T011 + T012 parallel (same file, different sections — coordinate to avoid edit conflicts; safer sequential); T013 + T014 parallel (disjoint headers); T016–T021 parallel across disjoint headers after the seam headers (T013–T015) compile.
- US4 docs: T033, T034 parallel (different files).
- Polish: T039 parallel with T038 analysis.

---

## Parallel Example: User Story 1

```bash
# Disjoint header templating (different files, no shared edits):
Task: "Template shared_net.hpp (slot params/loops) in include/wengine/logic/shared_net.hpp"   # T013
Task: "Template tie_set.hpp (TieSnapshot + survivors spans) in include/wengine/logic/tie_set.hpp" # T014

# After seam compiles, fan out across the remaining stage path:
Task: "Template topo.hpp (scratch cores + noexcept alias wrappers)"      # T016
Task: "Template eval.hpp (scratch cores + alias wrappers)"                # T019
Task: "Template seq_state.hpp + seq_step.hpp"                             # T020
Task: "Template optimize family (out-buffer cores)"                       # T021
```

## Parallel Example: User Story 2

```bash
Task: "Implement motif builders in benchmarks/bench_logic_scale.cpp"     # T024
# Then, once motifs build:
Task: "Write Environment + Variance-policy blocks in docs/benchmarks/logic-scale.md"  # T028 (prose, no code dep)
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1: Setup (before-numbers saved)
2. Complete Phase 2: Foundational (template root + targets + anchors)
3. Complete Phase 3: User Story 1 (reach + parity tests green at 65,536 gates)
4. **STOP and VALIDATE**: `ctest -R logic_scale` → OK; full suite byte-identical — scale substrate proven with no sweep/report involved
5. Smoke-check reach beyond ctest via an early `bench_logic_scale` ladder stub if desired, then proceed

### Incremental Delivery

1. Setup + Foundational → template root ready
2. + US1 → reach proven (MVP!) — 65,536-gate parity in ctest
3. + US2 → baseline committed (core value!) — full ladder to ≥1M with tables + environment pin
4. + US3 → attribution + gate operational — verdicts, ranking, cite-or-out-of-order rule
5. + US4 → regression pin — byte-identical alias, audit clean
6. + US5 → instrument longevity — re-run procedure + demonstrated reproduction

### Parallel Team Strategy

With multiple developers (after Foundational completes):

- Developer A: US1 headers (T013–T021) + scale tests (T011–T012, T023)
- Developer B: bench harness shape (T024–T026, stubbed against small capacities until A's headers land)
- Developer C: docs prose tracks (T009/T010 skeletons → T033/T034/T036 procedure drafts)

US2 measurement (T027) and US3 verdicts (T029–T031) serialize after A's headers compile; US4 validation (T032/T035) and US5 demonstration (T037) come last.

---

## Notes

- [P] tasks = different files, no dependencies; same-file tasks (T011/T012, T033–T035 are different files so OK) coordinated to avoid edit conflicts
- [Story] label maps task to user story for traceability (US1–US5); Setup/Foundational/Polish carry none
- FR-008 is a hard constraint on every implementation task: no vectorization, threading, GPU, or algorithmic rework — only the enablement substrate strictly required to reach capacity
- FR-006 is a hard constraint on every report task: zero extrapolated/estimated/projected numbers; over-budget stages are loud `UNMEASURED-AT-SIZE` gaps
- Determinism: slot-order iteration, lowest-free-slot policy, per-kind counters, and snapshot/eval order are capacity-blind by construction — verify, don't re-test, at each capacity
- Commit granularity: after each task or logical group; stop at any checkpoint to validate the story independently

---

## Phase 9: Convergence

**Purpose**: Close the assessed gaps between the spec/plan/tasks intent and the
current tree (converge run 2026-11-22). All 40 prior tasks stay untouched;
only the items below are remaining work. T041/T042/T044 are blocked on the kill-list
optimizations FR-008 defers to follow-ons — they are recorded here for
traceability, not for in-feature implementation.

- [X] T041 Demonstrate the 1M-gate measured-path reach (build, wire, compile, scalar-solve, word-evaluated slice, sequential step with scalar/word bit-exact parity and bit-identical repeats) per FR-001/SC-001/US1-AC1 — closed 2026-11-22 on the 016 tree: standing reach invocation `bench_logic_scale <motif> 65536 1048576 3 5 21600 2` fully measured on motifs 0+3 (zero UNMEASURED), parity/repeats pinned by `WENGINE_LOGIC_SCALE_GATES=1048576 ./build/test_logic_scale` → OK; record in `docs/benchmarks/logic-scale.md #### 1M reach on the 016 tree`
- [X] T042 Fill the measured 1048576 cells for compile-rebuild, solve, word, shared-solve, step, optimize, dedup, remove, and reset in `docs/benchmarks/logic-scale.md` per FR-003/SC-002/US2-AC1 — closed 2026-11-22: full 1M rows for motifs 0+3 (plus the 262144 guard-history rows) committed beside the baseline in `#### 1M reach on the 016 tree`; baseline UNMEASURED cells stand as the pre-optimization record
- [X] T043 Extend the `tests/test_logic_scale.cpp` parity/prefix-slice pins to the 1M scale instantiation per FR-010/US1-AC6 (partial)
- [X] T044 Re-ground the 1M-end kill-list ranking (mutation-side / compile-side / evaluation-side splits) in 1M measured numbers in `docs/benchmarks/logic-scale.md` per FR-005/SC-003/US3-AC2 — closed 2026-11-22: ranked wall-total table + side totals (mutation 2219/1954 s, compile 3533/3877 s, evaluation 4394/3127 s, motifs 0/3) in `#### 1M reach on the 016 tree`; leader is the optimize fixpoint (~50–59 min)
- [X] T045 Remove the hour-projection estimates from the 1M note in `docs/benchmarks/logic-scale.md`, keeping only the budget-guard decline record per FR-006/US2-AC4 (contradicts)
- [X] T046 Validate the full `docs/build.sh` (Sphinx + Doxygen) build once the Python toolchain is available per plan/T039 (partial)
