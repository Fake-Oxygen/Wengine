# Implementation Plan: Versioned Workspace v1 (Flop Initial States + Clock-Domain Metadata)

**Branch**: `006-versioned-workspace-v1` | **Date**: 2026-11-04 | **Spec**: specs/006-versioned-workspace-v1/spec.md

**Input**: Feature specification from `/specs/006-versioned-workspace-v1/spec.md`

## Summary

Replace the logic workspace file schema with new-generation `version: 1` carrying per-sequential-element initial states (per-gate `init:` keys) and clock-domain metadata (top-level `clocks:` section pinning derived domain names). The loader accepts only the new schema — both legacy generations are refused loudly with no fallback — and load starts a new pool lifetime powered on to the file's inits. Save captures live pool Q as inits, keeping save/load/save byte-identical with no new verbs. Domain derivation gains a single engine-owned implementation shared by session stepping, `sim --seq-clocks`, and loader validation; the engine run path honors inits via a new nested-section field. Five shipped examples are converted and three advanced sequential references added; docs and version-pinned tests are updated, leaving zero legacy trace.

Approach (from research.md): bundle API beside the loader (`Workspace`, Netlist untouched), per-gate `init:` (ignore_nq precedent) + `clocks:` (ties precedent), saver-writes-live-Q, load-resets-pool, engine-owned derivation header, engine-run parity, extend-existing-tests strategy.

## Technical Context

**Language/Version**: C++20, GCC 13.3 baseline, CMake ≥ 3.20, out-of-source builds only.

**Primary Dependencies**: yaml-cpp (pinned submodule 0.8.0) behind the single-TU seam (`src/logic/yaml_store.cpp`) — no new dependencies.

**Storage**: YAML workspace files, new-generation `version: 1` schema (gates/wires/ties/clocks/meta); binary world checkpoints untouched.

**Testing**: Plain-`assert` binaries (`test_x: OK`) via `add_executable` + `add_test` + `ctest`; every `test_*` links `wengine_test_assert`; zero-warning build (`-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wnon-virtual-dtor` on `wengine_cli`).

**Target Platform**: Linux host, CLI + headless (GUI builds unaffected; no GUI changes).

**Project Type**: C++ simulation-engine library (`wengine_core` INTERFACE headers) + CLI (`wengine_cli`) + single app.

**Performance Goals**: No hot-path changes (evaluator/stepper untouched); load/save are O(gates) edge operations — no benchmark claims made or needed.

**Constraints**: Deterministic canonical bytes; atomic load (workspace + pool untouched on any failure); `Netlist` never reshapes; engine headers stay yaml-free except the sanctioned TU; one nested section per solver in `RunConfig`; no new commands/verbs/registries.

**Scale/Scope**: 1 new engine header, 1 extended TU, ~6 CLI TUs touched, 4 test files updated (no new target), 8 example files (5 converted + 3 new), 3 doc/usage-text updates.

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

- **I. Physical/numerical correctness** — PASS. Evaluation, excitation tables, control priority, and level rules are untouched; inits are binary values with loud non-binary rejection. No FP-order or iteration-order changes.
- **II. Determinism/reproducibility** — PASS. Reset+resync+apply is deterministic (serials restart 0-based in slot order); canonical bytes pinned; save-captures-Q is a pure function of session state (documented in spec assumptions). Repeated load+run is bit-identical by construction.
- **III. Independent-reference testing** — PASS. Verified at every boundary: canonical file bytes (goldens) → load → pool state (`peek`) → stepping trajectories → engine-run CSV. No registry-size `static_assert` changes (no new commands/verbs/sources). No TTY-behavior change (no PTY tests needed).
- **IV. Measured performance** — PASS. No performance claims made; loader/save/derivation are edge-path O(gates) code; hot paths (eval, step kernels) unmodified, so no benchmark obligation arises.
- **V. Simplicity/minimal scope/dependency discipline** — PASS. Smallest coherent diff: Netlist untouched (bundle rides beside, R7); engine stays header-only (one new header, R6 — justified by the file-pins-stepping drift hazard); dependency direction preserved (cli → solvers, yaml seam intact); no new third-party code.
- **Additional constraints** — PASS. C++20/GCC/CMake/zero-warnings unchanged; no unit-table changes (binary inits, no physical quantities); `RunConfig` gains a field *inside* the existing nested sequential section only; one-command-per-domain preserved (no new commands).
- **Workflow** — PASS (planned): narrowest tests first (`test_logic_yaml`, seq CLI tests), then full `ctest`; README + docs/logic.md + usage strings updated with the change; no git actions (not requested).

*Post-Phase-1 re-check (2026-11-04)*: research, data model, contract, and quickstart introduce no new violations. R6's new header is the single justified addition (single-implementation rule for derivation); the contract's loader-gate table and error taxonomy are strictness-preserving. No Complexity Tracking entries required.

## Project Structure

### Documentation (this feature)

```text
specs/006-versioned-workspace-v1/
├── plan.md              # This file (/speckit.plan command output)
├── research.md          # Phase 0 output (/speckit.plan command)
├── data-model.md        # Phase 1 output (/speckit.plan command)
├── quickstart.md        # Phase 1 output (/speckit.plan command)
├── contracts/           # Phase 1 output (/speckit.plan command)
│   └── workspace-v1.md  # File-format grammar, loader gate, CLI effects, reference files
└── tasks.md             # Phase 2 output (/speckit.tasks command - NOT created by /speckit.plan)
```

### Source Code (repository root)

```text
include/wengine/logic/
├── clock_domains.hpp    # NEW: pure engine-owned domain derivation (R6)
├── yaml_store.hpp       # EXTEND: Workspace bundle + load/save_workspace API (R7)
└── seq_config.hpp       # EXTEND: per-slot power-on init field (R8)

src/logic/
└── yaml_store.cpp       # EXTEND: v1 schema (init keys, clocks:), retired fallback (R1-R3,R9,R10)

src/cli/commands/logic/
├── common.{hpp,cpp}     # EXTEND: bundle wrappers, save with pool Q, adopt engine derivation
├── load.cpp             # EXTEND: new-lifetime pool handling + atomic restore (R5)
├── save.cpp / export.cpp# EXTEND: pass pool Q through (R4) — via common wrapper
└── step.cpp / peek.cpp  # ADOPT engine derivation (same behavior, one implementation)

src/cli/commands/sim.cpp# EXTEND: --seq-file loads bundle, fills section inits, adopts derivation (R6,R8)
include/wengine/cli/logic_registry.hpp  # UPDATE: save/load usage strings (versions)
include/wengine/solvers/configured_run.hpp  # EXTEND: apply section inits post-sync (R8)

tests/
├── test_logic_yaml.cpp          # EXTEND: v1 goldens, refusals, inits, clocks cases, 8-file examples
├── test_cli_args.cpp            # UPDATE: version-1 save goldens + refusal strings
├── test_logic_seq_controls.cpp  # UPDATE: v1 fixtures
├── test_logic_seq_cli_step.cpp  # UPDATE: v1 fixtures
└── test_logic_seq_runner.cpp    # EXTEND: engine-honors-inits via direct section

examples/logic/                 # 5 converted to v1 + 3 NEW advanced sequential references
docs/logic.md (§§9,12) + README.md  # UPDATE: v1 schema, retired legacy, no fallback
```

**Structure Decision**: Single-project layout retained (no new apps, targets, or registries). One new header-only engine header; one extended platform TU; edges adopt shared derivation. No new test target — existing suites own their seams.

## Complexity Tracking

> No violations. This section is intentionally empty — the Constitution Check passes without exception and R6's single new header is a justified derivation-ownership move documented in research, not a complexity violation.
