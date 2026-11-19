---
description: "Task list for feature implementation"
---

# Tasks: Comprehensive Code Documentation

**Input**: Design documents from `/specs/009-comprehensive-code-documentation/`

**Prerequisites**: plan.md (required), spec.md (required for user stories), research.md, data-model.md, contracts/

**Tests**: No TDD test suite is requested. Verification is audit-based per plan.md — catalog completeness (`docs/audit_catalogs.py`), Doxygen coverage warnings, command-accuracy execution, and pre/post `ctest` parity. Those verification tasks are included below; no new engine test targets are created.

**Organization**: Tasks grouped by user story (US1–US7 from spec.md). Story tasks carry `[US#]`; Setup/Foundational/Polish tasks do not. Format: `- [ ] [ID] [P?] [Story?] Description with file path`.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies on incomplete tasks)
- **[Story]**: Which user story this task belongs to (US1…US7)
- Include exact file paths in descriptions

## Path Conventions

Single project, documentation feature — no `src/` layout changes. New files live under `docs/` (Sphinx source root, plan.md Structure Decision); comment-only edits spread across `include/`, `src/`, `apps/`, `tests/`, `benchmarks/`, `examples/`. Generated output is out-of-tree at `build/docs/`.

**Global rules for every task** (from AGENTS.md/constitution, apply throughout):
- Source edits are comment-only in JavaDoc form `/** ... */` with `@`-commands (`contracts/doxygen-comment-conventions.md` DC6: never reflow code, never touch code tokens, `assert()`-macro comma caution; anything suspicious becomes a Finding, never an in-pass fix).
- No commits/pushes/branches (git discipline) unless the operator explicitly requests them.
- Engine builds must stay warning-clean and all `ctest` results identical to the Phase 2 baseline at every story checkpoint.

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Bootstrap the Sphinx + MyST + Doxygen toolchain per `contracts/docs-build.md`

- [X] T001 [P] Create `docs/requirements.txt` with pinned toolchain (BC1): `sphinx==9.1.1`, `myst-parser==5.1.0`, `breathe==5.1.0`, `furo==2025.12.19`, `sphinx-copybutton==0.5.2`; install with `pip install -r docs/requirements.txt` and record the resolved versions
- [X] T002 [P] Create `docs/Doxyfile` (research R4): `INPUT = include/wengine`, `RECURSIVE = YES`, `GENERATE_XML = YES`, `OUTPUT_DIRECTORY = build/docs/doxygen-xml`, `EXTRACT_ALL = YES`, `WARN_IF_UNDOCUMENTED = YES`, `WARN_NO_PARAMDOC = YES`, `JAVADOC_AUTOBRIEF = NO`, Markdown-in-comments enabled
- [X] T003 [P] Create `docs/conf.py`: extensions `myst_parser`, `breathe`, `sphinx_copybutton`; `html_theme = "furo"`; `breathe_projects = {"wengine": "../build/docs/doxygen-xml"}` with `breathe_default_project`; `primary_domain = "cpp"`; source suffix `.md`
- [X] T004 [P] Create `docs/build.sh` (BC2): check prerequisites (doxygen ≥ 1.9.2, sphinx-build, python modules) naming anything missing → `doxygen docs/Doxyfile` → `sphinx-build -b html docs build/docs/html`; loud non-zero failures; output strictly out-of-tree (BC3)

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: A buildable walking-skeleton site + the audit/baseline machinery every story needs

**⚠️ CRITICAL**: No user story work can begin until this phase is complete.

- [X] T005 Create `docs/index.md` walking skeleton (NC1/NC3): five audience portals as stub links (Using / Developing / API Reference / Quality — Tests & Benchmarks / Examples); join the seven existing topic guides at their NC3 slots from their current paths — `signals.md`, `delivery.md`, `ros2.md`, `zephyr-hil.md` under Developing; `tui.md`, `logic.md`, `domain-outputs.md` under Using; add stub `user/`, `dev/`, `api/`, `tests/`, `benchmarks/`, `examples/` index pages; then run `docs/build.sh` and verify a warning-clean (minus recorded allow-list) HTML build with all seven guides reachable (quickstart scenario 1)
- [X] T006 [P] Create `docs/audit_catalogs.py` (CS5 semantics, stdlib-only Python): inventory = CTest registration parsed from `CMakeLists.txt` `add_test` entries (74 today) + `benchmarks/*` + `examples/*` file listings; catalog = `## test_*` / `## bench_*` / `## <file>` anchors under `docs/tests|benchmarks|examples/`; exit 0 listing "N/N covered", exit non-zero listing every missing entry and every ghost anchor
- [X] T007 [P] Record verification baselines: configure/build the engine (zero warnings) and run the full `ctest --test-dir build --output-on-failure`; save the pass/fail inventory as the SC-007 pre-comment-pass baseline that every later checkpoint must match exactly

**Checkpoint**: Foundation ready — `docs/build.sh` produces a navigable HTML site containing the existing corpus; audit tooling + baselines exist. User story implementation can now begin in parallel.

---

## Phase 3: User Story 1 - Developer guide: how the engine works (Priority: P1) 🎯 MVP

**Goal**: A developer answers architecture, step-lifecycle, determinism, and extension-point questions from `docs/dev/` alone (FR-001, SC-005).

**Independent Test**: Quickstart scenario 2 (developer leg) — from `build/docs/html/index.html`, reach "where does a new solver plug in" in ≤ 3 clicks; ≥ 9/10 sampled architecture questions answered from the guide only.

- [X] T008 [P] [US1] Create `docs/dev/architecture.md`: module map for all 12 `include/wengine/` directories + `apps/gui` layering (backend/controllers/models/panels/views) + `src/` platform TUs; dependency-direction rules; end-to-end life of a simulation (configuration → state → stepping → snapshot → sinks → outputs); per-module collaborators, deep-linking to corpus guides and `api/` pages instead of restating them (NC5)
- [X] T009 [P] [US1] Create `docs/dev/determinism.md`: fixed-step sequencing via `core::Runner` + `world::State`, stable signal serials, two-pass evaluation order (generators then `kSum`), snapshot timing, the units discipline (`units::Value<Dim>`, SI base), serialization versioning with fallback loaders, and the four-clocks separation (links to `docs/delivery.md` and `docs/signals.md`, never restating)
- [X] T010 [P] [US1] Create `docs/dev/extension-points.md`: the "where do I add X" playbook — new closed-form solver, new stepped solver (registry + registry-size `static_assert`s), new CLI command (`commands.hpp` → `command_registry.hpp` → `src/cli/commands/` + `tests/test_cli_args.cpp`), new IC chip, new signal quantity, new sink, new GUI panel/view (frame-source registry) — each with the file path to touch first and the test(s) to update per AGENTS.md §9
- [X] T011 [US1] Wire `dev/` pages into the toctree (NC2), rebuild, and run the independent test above; fix pages until it passes

**Checkpoint**: User Story 1 independently valuable: a searchable, navigable HTML site with corpus + developer guide. MVP deliverable.

---

## Phase 4: User Story 2 - End-user guide: run the engine, docs only (Priority: P1)

**Goal**: A first-time user goes zero → build → running simulation → interpreted output using only `docs/user/` (FR-002, SC-002).

**Independent Test**: Quickstart scenarios 1–3 (user leg): every command in the five pages executes exactly as written; the getting-started path completes on a clean machine meeting stated prerequisites in ≤ 30 minutes.

- [X] T012 [P] [US2] Create `docs/user/getting-started.md`: prerequisites, build (standard + `dev`/`test-opt` presets), first headless run (`./build/PhysicsSimApp sim run --steps 100 --particles 1`), REPL smoke (`printf 'ohm 12V 3A\nexit\n' | ./build/PhysicsSimApp`); execute every command while writing (FR-013)
- [X] T013 [P] [US2] Create `docs/user/cli-commands.md`: every top-level command family and what each is for (`sim` runs any registered stepped solver, `sig` owns signal vocabulary, `ic` owns chip calculators, `logic` owns the logic domain) with `--help`-accurate usage, named `--key value` / `--key=value` arguments, completion, and exit codes 0/1/2/-1
- [X] T014 [P] [US2] Create `docs/user/outputs.md`: console, CSV, and signals-CSV output formats; `max_digits10` exact round-trip; snapshot CSV is output replay, never a restart checkpoint (checkpoints are `save_state`/`load_state`); exit-code diagnosis; links to `docs/domain-outputs.md` (corpus) without restating
- [X] T015 [P] [US2] Create `docs/user/gui.md`: graphical app walkthrough — solver panels, stepped-sim control, signal batch panel, graphs, 2D canvas, 3D viewport, source panel (in-process / SHM subscribe / CSV replay); verified by an actual windowed run of the built GUI (project's manual-verification method; note the `BUILD_GUI=ON` build prerequisite)
- [X] T016 [P] [US2] Create `docs/user/troubleshooting.md`: usage vs. runtime failures via exit codes, common argument errors (stray positionals, bad quantities like `2mA` as volts failing loudly), PTY/terminal requirements for interactive mode, where `core::Status` errors surface
- [X] T017 [US2] Wire `user/` pages into the toctree, then execute every command across all five pages verbatim (story-level accuracy pass, FR-013); fix pages until the independent test passes

**Checkpoint**: Users 1 AND 2 both independently functional: developer guide + end-user guide live on the same indexed site.

---

## Phase 5: User Story 3 - Test catalog: every test explained (Priority: P1)

**Goal**: Every test artifact — every registered test binary and every test-support script/probe — has an entry answering what it checks / purpose / guards against / how to run / prerequisites / boundaries (FR-004, SC-001).

**Independent Test**: Quickstart scenario 3 — pick any entry at random, all six CS1 fields present; `docs/audit_catalogs.py` reports zero missing/ghost test entries against the live CTest inventory.

Every entry follows `contracts/catalog-schemas.md` CS1/CS2: anchor `## test_<exact-target-name>`; fields **What it checks** (quote-level specific), **Purpose**, **Guards against** (the regression class), **How to run** (exact commands), **Prerequisites** (standard build; real PTY / ROS 2 / embedded hardware where applicable + behavior when absent), **Boundaries** (what it deliberately does not cover). Inventory: ~70 `test_*` binaries + `assert_fail_probe` + `assert_probe_check.sh`, `logic_pty_smoke.py`, `tui_pty_smoke.py`, `tests/test_logic_barrier_fixture.hpp` — the audit compares against the live registration, not this list.

- [X] T018 [P] [US3] Create `docs/tests/index.md`: domain map of the suite, how to run it (`ctest`, the `test-opt` preset, the assertion policy linking `wengine_test_assert`), and pointers to each domain page
- [X] T019 [P] [US3] Create `docs/tests/core-world.md`: entries for `test_runner`, `test_world`, `test_sim`, `test_run_config`, `test_determinism`, `test_snapshot_reuse`, `test_particle_identity`, `test_solver_registry`
- [X] T020 [P] [US3] Create `docs/tests/solvers.md`: entries for `test_math_units`, `test_ohm_law`, `test_s0`, `test_i2c`, `test_ic`, `test_ldo`, `test_led`, `test_ina219`, `test_ap63205`, `test_tmc22xx` (closed-form goldens from independent references; datasheet-anchored for IC parts)
- [X] T021 [P] [US3] Create `docs/tests/emit-routing.md`: entries for `test_emit`, `test_routing`, `test_output_health`, `test_output_queue`, `test_paced`
- [X] T022 [P] [US3] Create `docs/tests/signal-scripting.md`: entries for `test_signal`, `test_signal_control`, `test_lua`
- [X] T023 [P] [US3] Create `docs/tests/link-transport.md`: entries for `test_link_shm`, `test_link_protocol`, `test_link_compact`, `test_link_bridge`, `test_link_receiver`, `test_link_sample_block`, `test_link_sockets`, `test_tcp_worker`, `test_uart_codec`, `test_uart_pub` (seqlock/version/lag-accounting regression classes)
- [X] T024 [P] [US3] Create `docs/tests/logic.md`: entries for `test_logic`, `test_logic_layout`, `test_logic_yaml`, the eleven `test_logic_seq_*` targets, plus CS2 support entries for `tests/test_logic_barrier_fixture.hpp` and `tests/logic_pty_smoke.py` (real-PTY prerequisite: piped `script(1)` cannot reproduce this behavior)
- [X] T025 [P] [US3] Create `docs/tests/tui.md`: entries for `test_tui_async`, `test_tui_batches`, `test_tui_choice`, `test_tui_exec`, `test_tui_forms`, `test_tui_views`, plus `tests/tui_pty_smoke.py` (CS2, real-PTY)
- [X] T026 [P] [US3] Create `docs/tests/gui-models.md`: entries for `test_canvas2d`, `test_viewport3d`, `test_gui_sim_control`, `test_gui_stream_source`, `test_signal_panel`, `test_signal_panel_lua`, `test_closed_form_panels`, `test_frame_source` (engine-only model tests linking `wengine_core` alone; windowed visuals are manual per project practice)
- [X] T027 [P] [US3] Create `docs/tests/integrations-fabric.md`: entries for `test_ros2_control`, `test_ros2_lifecycle`, `test_ros2_mapping`, `test_ros2_publisher` (ROS 2 stack prerequisite), `test_control_server`, `test_cli_args`, `test_assert_policy`, plus CS2 support entries for `tests/assert_fail_probe` and `tests/assert_probe_check.sh` (the probe guards the assertion policy actually failing builds)
- [X] T028 [US3] Wire `tests/` pages into the toctree, run `docs/audit_catalogs.py`, and fix entries until the test inventory reports zero missing entries and zero ghost anchors (SC-001)

**Checkpoint**: All three P1 stories complete — the site now delivers its three primary audiences. Each is independently shippable.

---

## Phase 6: User Story 4 - API reference for engine consumers (Priority: P2)

**Goal**: Per-module API pages, Breathe-rendered from Doxygen XML, each with one minimal correct usage pattern (FR-003, SC-003 page side).

**Independent Test**: Quickstart scenario 4 — each module page renders entities with brief/params/returns; every usage-pattern snippet compiles and runs as documented.

Page shape (data-model Entity 5): module prose (responsibility, collaborators, rules obeyed) + one compile-grade usage pattern + Breathe directives for key entities + the module's auto-generated entity listing (nothing undocumented can hide). MyST directive form: `` ```{doxygenclass} wengine::X `` fences (research R1).

- [X] T029 [P] [US4] Create `docs/api/index.md`: API landing with the 12-module table, the per-module page shape, and one worked MyST/Breathe directive example the other pages copy
- [X] T030 [P] [US4] Create `docs/api/units.md` (`units::Value<Dim>`, `Dimension`, arithmetic, literals, `scales.hpp` single-table discipline)
- [X] T031 [P] [US4] Create `docs/api/core.md` (`Runner`, `ISolver`, `RunConfig` sections, `core::Status` vocabulary)
- [X] T032 [P] [US4] Create `docs/api/world.md` (`State`, pools, `Snapshot`, `take_snapshot` slot-order contract)
- [X] T033 [P] [US4] Create `docs/api/physics.md`
- [X] T034 [P] [US4] Create `docs/api/solvers.md` (closed-form + stepped contracts + `solver_registry.hpp`; datasheet-reference blocks on `solvers/ic/` pages)
- [X] T035 [P] [US4] Create `docs/api/signal.md` (kinds/params/quantities table, `StampedValue`, serials)
- [X] T036 [P] [US4] Create `docs/api/emit.md` (`ISink`, routing wrappers `SignalSubsetSink`/`StrideSink`, counter vocabulary)
- [X] T037 [P] [US4] Create `docs/api/link.md` (16 headers: `Frame`, seqlock bus, endpoints, wire versions)
- [X] T038 [P] [US4] Create `docs/api/logic.md` (23 headers: netlist/state/layout/pins/topo)
- [X] T039 [P] [US4] Create `docs/api/ros2.md` (bridge + message vocabulary; ROS 2 prerequisite stated)
- [X] T040 [P] [US4] Create `docs/api/tui.md` (terminal UI model headers)
- [X] T041 [P] [US4] Create `docs/api/cli.md` (`cli/args.hpp` parsing, command spec structure, `ic_dispatch.hpp`)
- [X] T042 [US4] Verify US4: run `docs/build.sh` (every module page resolves its Breathe directives — no missing-entity errors); compile and run every usage-pattern snippet in a scratch TU against the built tree; fix pages until both pass (FR-013 for API patterns)

**Checkpoint**: API consumers served. Note: pages render with whatever comments exist today; SC-003's "zero bare signatures" state goes fully green only after US6 (dependency noted below).

---

## Phase 7: User Story 5 - Benchmark & example catalogs (Priority: P2)

**Goal**: Every benchmark and every shipped example documented with purpose, run instructions, and interpretation/expected outcome (FR-005, FR-006, SC-004).

**Independent Test**: Audit reports zero gaps for `benchmarks/` and `examples/`; running any documented bench/example command verbatim succeeds (bench preset lane — benchmarks never link the assertion policy, AGENTS §3).

- [X] T043 [P] [US5] Create `docs/benchmarks/index.md`: how to build/run via the `bench` preset, the opaque-input/constant-folding discipline (a `constexpr` solver called with literals can measure nothing — FR-005 pitfall), and links to each bench page
- [X] T044 [P] [US5] Create `docs/benchmarks/solvers.md`: CS3 entries for `bench_ohm`, `bench_s0`, `bench_i2c`, `bench_projectile`
- [X] T045 [P] [US5] Create `docs/benchmarks/transport-signal.md`: CS3 entries for `bench_link`, `bench_link_sockets`, `bench_sample_block`, `bench_tcp_worker`, `bench_signal`, `bench_signal_scale`, `bench_snapshot`
- [X] T046 [P] [US5] Create `docs/benchmarks/logic.md`: CS3 entry for `bench_logic`
- [X] T047 [P] [US5] Create `docs/examples/index.md`: example inventory and run ground rules (transports/endpoints prerequisites when an example talks to a live bus)
- [X] T048 [P] [US5] Create `docs/examples/link-tools.md`: CS4 entries for `examples/cli_smoke.cpp`, `link_sample.cpp`, `link_bridge.cpp`, `link_fastread.cpp`, `link_fastread_stall_repro.py`, `link_receiver_accounting.py`, `link_socket_protocol.py`, `link_tcp_signal_receiver.py`, `link_tcp_signal_plot.py`, `link_udp_signal_receiver.py`, `link_udp_signal_plot.py`
- [X] T049 [P] [US5] Create `docs/examples/integrations.md`: CS4 entries for `examples/ros2_bridge.cpp`, the `examples/lua/` scripts, the `examples/logic/` samples, and the `examples/zephyr/` embedded sample (hardware prerequisite stated)
- [X] T050 [US5] Wire `benchmarks/` + `examples/` pages into the toctree; run every documented bench/example command verbatim; run `docs/audit_catalogs.py` and fix entries until benchmarks + examples report zero missing/ghost (SC-004)

**Checkpoint**: Quality portals complete — tests, benchmarks, examples all indexed and audited.

---

## Phase 8: User Story 6 - In-code contract comments (Priority: P2)

**Goal**: The code explains itself in place: every public header carries DC1/DC2 documentation; non-obvious logic gets site comments — comment-only, verified bit-identical (FR-008, SC-003, SC-007).

**Independent Test**: Quickstart scenario 4 + 6 — doxygen reports zero undocumented warnings for `include/wengine/**`; engine builds warning-clean; full `ctest` identical to the T007 baseline.

Convention for every task in this phase (`contracts/doxygen-comment-conventions.md`): JavaDoc blocks `/** ... */` with `@`-commands — `/** @file */` role paragraph per header; `@brief` one-liner, `@param[in] name` + meaning/units, `@return` naming real `core::Status` codes on failure paths (no `@throws`), `@note` for units/ownership/determinism contracts per entity. DC6 rules apply verbatim: no code reflow, no token edits, `assert()`-macro comma caution, defects become Findings.

- [X] T051 [P] [US6] Document `include/wengine/units/` (2 headers) + `include/wengine/physics/` (1) to full DC1/DC2 level
- [X] T052 [P] [US6] Document `include/wengine/core/` (4) + `include/wengine/world/` (5)
- [X] T053 [P] [US6] Document `include/wengine/solvers/` (18 incl. `solvers/ic/`; preserve existing datasheet reference blocks, add the comment grammar around them)
- [X] T054 [P] [US6] Document `include/wengine/signal/` (5)
- [X] T055 [P] [US6] Document `include/wengine/emit/` (6) + `include/wengine/link/` (16)
- [X] T056 [P] [US6] Document `include/wengine/logic/` (23)
- [X] T057 [P] [US6] Document `include/wengine/ros2/` (8)
- [X] T058 [P] [US6] Document `include/wengine/tui/` (22)
- [X] T059 [P] [US6] Document `include/wengine/cli/` (19)
- [X] T060 [P] [US6] Add DC5 free-form site comments at non-obvious logic in `src/` and `apps/`: seqlock re-read loops and SHM endpoint TUs, terminal raw-mode re-entry (`TCSANOW`/`TCSADRAIN` lesson), protocol framing, E12 next-decade rule, frame-to-step folding FP fragility — the *why* at the site, no Doxygen form needed here
- [X] T061 [P] [US6] Add brief file-header role comments to every file in `tests/` (~75 files), `benchmarks/` (12), `examples/` (C++/Python) — one short role paragraph each, stating what the artifact guards or demonstrates
- [X] T062 [US6] Verify US6: rebuild the engine (zero warnings), rerun the full `ctest` suite and diff against the T007 baseline — identical (SC-007); run doxygen — zero `WARN_IF_UNDOCUMENTED`/`WARN_NO_PARAMDOC` warnings for `include/wengine/**` (SC-003); spot-check DC6 compliance (no reflowed code anywhere in the diff)

**Checkpoint**: The rendered API pages now show zero bare signatures — US4's completeness listing goes fully green.

---

## Phase 9: User Story 7 - Index, consistency, regeneration (Priority: P3)

**Goal**: One polished entry point, no contradictions with the existing corpus, and a documented refresh path (FR-009, FR-010, FR-011, SC-008).

**Independent Test**: Quickstart scenarios 2 (all legs) + 7 + 8 — 3-click rule holds per audience; consistency audit finds zero contradictions; search finds cross-cutting terms.

- [X] T063 [US7] Finalize `docs/index.md` portals: full NC1 portal descriptions replacing stubs, prominent README.md + GLOSSARY.md repository links; verify the site search finds cross-cutting terms (`seqlock`, `emit-csv`) across user/dev/API/quality pages (scenario 8)
- [X] T064 [P] [US7] Create `docs/regeneration.md`: the FR-011 procedure per `contracts/docs-build.md` BC5 — rebuild via `docs/build.sh`, refresh catalog pages when inventories change, re-run audits, and the findings policy (contradictions resolved toward actual behavior, reported, never papered over)
- [X] T065 [US7] Consistency audit (FR-010/NC5): read `README.md`, `GLOSSARY.md`, and the seven topic guides against every new page; resolve contradictions toward actual behavior; record suspected defects as Findings; confirm no new page restates corpus content instead of linking it
- [X] T066 [P] [US7] Update `README.md`: add a Documentation section pointing to `docs/build.sh` and the HTML suite (AGENTS §12 — Run examples/module table/test list stay otherwise untouched; no unrelated rewording)

**Checkpoint**: All user stories complete and integrated.

---

## Phase 10: Polish & Cross-Cutting Concerns

**Purpose**: Suite-wide verification that no single story can prove alone

- [X] T067 Full accuracy pass (FR-013/SC-006): execute every command, example, and usage pattern across ALL pages verbatim; fix pages where reality differs (recording Findings for suspected defects); report zero broken instructions
- [X] T068 Full audit suite: `docs/audit_catalogs.py` zero gaps across tests + benchmarks + examples; doxygen zero undocumented warnings; no orphan-page warnings in the Sphinx build; 3-click rule verified for all three audiences (SC-001/SC-003/SC-004/SC-008)
- [X] T069 Run `specs/009-comprehensive-code-documentation/quickstart.md` scenarios 1–9 end-to-end; consolidate the Findings list (doc/code mismatches found anywhere during the feature); walk the AGENTS.md completion checklist and report remaining uncertainty explicitly

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies — T001–T004 run in parallel immediately.
- **Foundational (Phase 2)**: Depends on Phase 1 — T005 needs all four; T006/T007 are [P] alongside T005. **BLOCKS all user stories.**
- **User Stories (Phases 3–9)**: All depend on Phase 2 completion.
  - US1, US2, US3 (P1) — mutually independent; parallel-capable after Phase 2.
  - US4, US5, US6 (P2) — mutually independent; parallel-capable after Phase 2. US6's baseline is T007.
  - US4↔US6 soft coupling: API pages (US4) render with current comments and are structurally complete without US6, but SC-003's "zero bare signatures" only goes green once US6 lands (verified at T062, re-checked at T068). No hard ordering required.
  - US7 (P3) depends on all other stories being complete (it indexes, audits, and cross-links them).
- **Polish (Phase 10)**: Depends on all stories.

### Within Each User Story

- The `## test_*` / per-module / per-group authoring tasks are [P] — different files, no interdependencies.
- Each story's final task (T011, T017, T028, T042, T050, T062, T063/T065) is the story's wiring + verification gate and depends on that story's [P] tasks.

### Parallel Opportunities

- Phase 1: all four tasks in parallel.
- Phase 2: T006, T007 parallel with T005.
- US3: nine domain pages (T019–T027) in parallel — the largest single burst.
- US4: twelve module pages (T030–T041) in parallel.
- US6: nine header groups (T051–T059) in parallel, plus T060/T061.
- Across stories (multi-agent): US1 ∥ US2 ∥ US3 after Phase 2; US4 ∥ US5 ∥ US6 next; US7 last.

---

## Parallel Example: User Story 3

```bash
# Launch all nine domain pages together (different files, CS1 schema pinned in contracts):
Task: "T019 core-world.md"
Task: "T020 solvers.md"
Task: "T021 emit-routing.md"
Task: "T022 signal-scripting.md"
Task: "T023 link-transport.md"
Task: "T024 logic.md"
Task: "T025 tui.md"
Task: "T026 gui-models.md"
Task: "T027 integrations-fabric.md"
# Then the story gate (single-threaded):
Task: "T028 wire toctree + audit zero gaps"
```

---

## Implementation Strategy

### MVP First (Setup + Foundational + User Story 1)

1. Complete Phase 1 (Setup) + Phase 2 (Foundational) — CRITICAL, blocks everything.
2. Complete Phase 3 (US1 developer guide).
3. **STOP and VALIDATE**: quickstart scenario 2 developer leg + SC-005 question sample.
4. At this point the repo already has a searchable, logically indexed HTML site (corpus + developer guide) — the feature's first independently valuable delivery.

### Incremental Delivery

1. Setup + Foundational → foundation ready.
2. + US1 → validate → **MVP** (developer audience served).
3. + US2 → validate (run-through on clean machine) → end users served.
4. + US3 → validate (audit zero gaps) → reviewers served; **all P1 audiences done**.
5. + US4 → validate (usage patterns compile) → embedders served.
6. + US5 → validate (bench/example audit) → contributors served.
7. + US6 → validate (parity + zero warnings) → source self-documenting; API pages fully green.
8. + US7 → validate (consistency, search, 3-click) → suite integrated.
9. Phase 10 → full-suite gates (accuracy, audits, quickstart 1–9).

Each story adds value without breaking previous ones — later pages only link earlier ones, and US6's comment-only rule keeps behavior identical throughout.

### Parallel Team Strategy

With multiple agents after Phase 2: A→US1, B→US2, C→US3, then A→US4, B→US5, C→US6, converge on US7 + Phase 10. US6's per-module groups parallelize further (nine groups).

---

## Notes

- [P] tasks = different files, no dependencies on incomplete tasks.
- [Story] labels map to spec.md user stories for traceability.
- Source edits are comment-only (`/** */` + `@`-commands); DC6 forbids code reflow, and every checkpoint re-proves `ctest` parity against T007.
- Do not commit/push/branch unless the operator explicitly requests it (AGENTS §14) — checkpoint validation replaces the template's commit advice.
- Suspected defects found while documenting are recorded as Findings and reported; they are never fixed inside this feature (FR-010).
- Stop at any checkpoint to validate a story independently.
- Avoid: vague entries, same-file conflicts between [P] tasks, cross-story dependencies that break independence.
