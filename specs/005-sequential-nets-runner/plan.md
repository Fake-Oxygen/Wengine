# Implementation Plan: Sequential Nets Runner

**Branch**: `005-sequential-nets-runner` | **Date**: 2026-11-04 | **Spec**: `spec.md`

**Input**: Feature specification from `/specs/005-sequential-nets-runner/spec.md`

## Summary

Sequential nets (flops + latches) move from CLI-only session stepping onto the
shared deterministic fixed-step path: a new header-only `SequentialSolver`
(`kName="logic"`) behind `core::ISolver`, a nested `logic::SequentialRunConfig`
section (topology + stimulus + multi-clock member list) aggregated into
`core::RunConfig`, singleton netlist + clocked-state pools in `world::State`,
extended `Snapshot` rows + `fill_snapshot`, format-v6 checkpoints with fallback
loaders, one `link::Frame` adapter + registry entry, one tidy CSV sink, and
`sim run --solver logic` with `--seq-file/--seq-clocks/--seq-from` options.
Interactive verbs, file schema, and existing solvers are unchanged (additive).

## Technical Context

**Language/Version**: C++20, GCC 13.3 baseline, CMake ≥ 3.20, out-of-source builds only (`dev`/`test-opt`/`bench` presets preferred).

**Primary Dependencies**: none new. Reuses `wengine_core` (`core::Runner`/`ISolver`/`RunConfig`), `wengine/logic` engine headers (`netlist`, `seq_state`, `seq_step`, `yaml_store`), `wengine_yaml` (existing TU, CLI side only), `wengine_cli` (`sim run`).

**Storage**: `world::State` pools (in-memory) + versioned binary checkpoints (format v6, fallback loaders for v1–v5) + versioned YAML workspaces (unchanged schema v1/v2, authoring format only).

**Testing**: plain-`assert` binaries printing `test_x: OK`, `add_executable` + `target_link_libraries` + `add_test`; every `test_*` target links `wengine_test_assert`. New tests: `test_logic_seq_runner` (solver + driver goldens vs hand-derived references), extended `test_run_config`, `test_solver_registry` (2→3), `test_frame_source` (2→3), `test_snapshot*`/serialize round-trip + v5 fallback, `test_cli_args` (seq options run/usage/stray/completion), CSV re-read `==`. Narrowest-first, then full `ctest --output-on-failure`.

**Target Platform**: Linux headless (CLI); GUI libraries untouched (only the `kFrameSources` registry entry, views unchanged).

**Project Type**: header-only engine library + CLI (`wengine_cli` STATIC) — same shape as the signal domain.

**Performance Goals**: no perf claims (Principle IV). Step cost is one `seq_step_filtered` call (stack arrays, no allocation, `noexcept`); emit reuses warmed snapshot capacities. Optional: extend `bench_logic` with a Runner-driven case only if a hot path is suspected — not required.

**Constraints**: zero warnings (`-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wnon-virtual-dtor` on `wengine_cli`); header-only engine (`INTERFACE`) except existing TUs; hot path: no exceptions/strings/iostream/allocations; `core::Status` errors; `==` for exact values, justified `near()` only for inexact FP (none expected — bit logic is exact).

**Scale/Scope**: `kMaxGates` = 1024 slots; snapshot + checkpoint carry dense slot-order arrays; Frame cap 64 channels (≤63 live flops for frame transports, loud refusal above); CSV unbounded.

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

- [x] **I. Correctness**: excitation tables, control priority, level rules reused verbatim from `seq_step.hpp` (no reimplementation); goldens hand-derived, never from the implementation; `==` on bits/round-trips.
- [x] **II. Determinism**: fixed-step `Runner` + `State`; slot-order snapshot/commit/serials; end-of-step value-at-T+dt convention; static stimulus; versioned checkpoints with fallbacks; CSV `max_digits10` replay (never restart).
- [x] **III. Testing**: five-boundary verification (reference → solver → CLI → artifact → re-read); registry-size asserts updated (solver 2→3, frame-source 2→3); no TTY behavior added (headless only).
- [x] **IV. Measured performance**: no claims, no speculative optimization; benchmark reuse optional.
- [x] **V. Simplicity/discipline**: smallest coherent diff — one solver header, one section header, pool/snapshot/serialize/adapter/sink/CLI extensions, no new commands, no new views, no dependency reversals (`cli → solvers/core`, `world` standalone, sinks at the edge). One documented deviation: singleton (non-entity-attached) pool, justified in research R1.
- [x] **Workflow**: README (Run examples, module table, test list), no git actions, Plane tracking out of band.

*Post-design re-check (Phase 1 complete): no new violations introduced by
data-model/contracts — deferred items (GUI panels, typed blocks, per-step
stimulus programs) are named follow-ups, not drift.*

## Project Structure

### Documentation (this feature)

```text
specs/005-sequential-nets-runner/
├── plan.md              # This file (/speckit.plan command output)
├── research.md          # Phase 0 output (10 decisions, all resolved)
├── data-model.md        # Phase 1 output (section, pool, snapshot, checkpoint)
├── quickstart.md        # Phase 1 output (4 validation scenarios)
├── contracts/           # Phase 1 output
│   ├── seq-cli.md       # sim run --seq-* schema + exit codes + completion
│   └── seq-snapshot.md  # snapshot arrays + CSV shape + Frame channels
└── tasks.md             # Phase 2 output (/speckit.tasks command — NOT created here)
```

### Source Code (repository root)

```text
include/wengine/logic/seq_config.hpp        # SequentialRunConfig (new, beside the module)
include/wengine/solvers/sequential.hpp      # SequentialSolver : ISolver (new, header-only)
include/wengine/solvers/solver_registry.hpp # + SequentialSolver (Registry 2 → 3)
include/wengine/core/run_config.hpp         # + logic::SequentialRunConfig seq member
include/wengine/world/state.hpp             # + netlist/seq/inputs/members/outputs pools + fill_snapshot rows
include/wengine/world/serialize.hpp         # format v6 + fallback loaders (v1–v5)
include/wengine/link/frame.hpp              # + frame_from_logic_snapshot
include/wengine/emit/logic_sink.hpp         # + LogicCsvSink (new, mirrors signal_sink.hpp)
include/wengine/logic/types.hpp             # + defaulted == on Gate/Netlist (+ SeqState in seq_state.hpp)
src/cli/commands/sim.cpp                    # --seq-file/--seq-clocks/--seq-from + logic CSV/SHM dispatch
apps/gui/views/frame_source.hpp             # + {"logic", ...} registry entry (2 → 3)

tests/test_logic_seq_runner.cpp             # new: solver + driver + determinism goldens
tests/test_run_config.cpp                   # extended: section/solver scoping goldens
tests/test_solver_registry.cpp              # size 2 → 3
tests/test_signal.cpp                       # SolverRegistry size 2 → 3 (signal file owns one assert)
tests/test_frame_source.cpp                 # size 2 → 3 + logic adapter goldens
tests/test_cli_args.cpp                     # seq options run/usage/stray/completion
tests/test_<snapshot|world|sim>.cpp         # snapshot rows, v6 round-trip, v5 fallback, CSV re-read
CMakeLists.txt                              # test_logic_seq_runner target + registration
README.md                                   # Run examples, module table, test list
```

**Structure Decision**: single-project layout (no Option 2/3) — engine headers
stay header-only `INTERFACE`; the only new TU-linked surface is the existing
`wengine_cli` `sim.cpp` plus the new test binary. GUI gets one registry line,
no panel/view changes.

## Complexity Tracking

No constitution violations to justify. The one structural deviation
(singleton pool instead of entity-attached) is a documented design choice with
rationale in `research.md` R1, not a gate violation.
