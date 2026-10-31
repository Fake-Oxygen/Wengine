# Implementation Plan: Sequential Session Verbs

**Branch**: `001-seq-session-verbs` | **Date**: 2026-10-31 | **Spec**: [spec.md](./spec.md)

**Input**: Feature specification from `/specs/001-seq-session-verbs/spec.md`

**Note**: This template is filled in by the `/speckit.plan` command; its definition describes the execution workflow.

## Summary

Expose the existing sequential core (`logic::SeqState` pool + `seq_step`
global-edge stepper) through three headless session verbs — `step`
(advance named clock N cycles), `poke` (drive block inputs), `peek` (read
flop Q state) — working identically in REPL session, one-shot, and piped
invocations with zero GUI dependency. Sequential blocks use readable
named pins (`DFF1.D`, `DFF1.CLK`, `DFF1.Q`, plus `NQ` negated output;
`DLATCH` uses `EN`), with numeric spellings kept as accepted aliases.
Prerequisite: enable sequential gate creation (`add DFF`/`DLATCH`), which
`add_gate` still refuses (SEQ1 staging pin); all stepping/eval semantics
stay untouched.

## Technical Context

**Language/Version**: C++20, GCC 13.3 baseline (repo standard)

**Primary Dependencies**: Existing `wengine_logic` headers (`logic/types.hpp`,
`logic/netlist.hpp`, `logic/seq_state.hpp`, `logic/seq_step.hpp`,
`logic/eval.hpp`), `wengine_cli` (`cli/logic_registry.hpp`,
`cli/status.hpp`); no new dependencies

**Storage**: N/A — in-memory process-wide `Netlist` workspace (existing);
poke values and per-clock cycle counters live in CLI-edge session maps
beside the workspace (same lifetime rules as the workspace itself);
existing versioned YAML `save`/`load` unchanged (counters derived from
gate IDs on load, no schema field)

**Testing**: Plain-`assert` `test_*` binaries printing `test_x: OK`,
registered via `add_executable` + `add_test`, linked against
`wengine_test_assert`; CLI golden tests through `run` + usage/stray +
`complete()`; PTY smoke only if TTY behavior added (none here)

**Target Platform**: Linux headless CLI (interactive REPL, one-shot
argv, stdin pipes, script files)

**Project Type**: CLI — sub-shell session verbs over a header-only engine

**Performance Goals**: Stepping loop is N × `seq_step` (allocation-free,
`noexcept` hot path preserved); CLI verb overhead negligible; no new
benchmark required (no hot-path change, per Constitution IV)

**Constraints**: Zero warnings (`-Wall -Wextra -Wpedantic -Wconversion
-Wshadow -Wnon-virtual-dtor` on `wengine_cli`); out-of-source builds;
`Netlist` additive-only (no field removal/renumber); kind bytes
append-only; exit codes 0/1/2 (usage error = 2, changes no state);
`core::Status` in engine, never exceptions; engine headers stay
`iostream`-free

**Scale/Scope**: 3 new verbs (`step`, `poke`, `peek`) + sequential
creation enablement in `add` + named-pin alias layer + `NQ` second
output with per-sink polarity; `LogicRegistry` 19 → 22 entries with the
`static_assert` in `tests/test_cli_args.cpp` updated; 3–4 new
`test_logic_seq_cli*` binaries plus updates to `test_logic_seq_kinds`
(pinned refusal flips), pin/naming goldens, and docs (`docs/logic.md`,
README examples)

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

- **I. Correctness**: New verbs are thin adapters over the pinned
  `seq_step` two-phase semantics (slot-order snapshot/commit, X/Z loud
  rejection); goldens derived from existing engine tests
  (`test_logic_seq_step` counter/shift-register vectors), never from the
  new code. PASS.
- **II. Determinism**: Slot-order stepping, slot-order peek output,
  sticky poke maps applied identically each run; replayBYTE-identical
  outputs asserted in tests; no FP/RNG/timing involvement. PASS.
- **III. Testing**: Multi-boundary — engine goldens (existing) → new
  CLI verb tests (run/usage/stray/completion) → save/load round-trip
  with sequential gates → scripted pipe replay test. Registry-size
  `static_assert` updated. PASS.
- **IV. Performance**: No hot-path change (stepper untouched; loop is
  caller-side N×); no optimization claimed, none needed. PASS.
- **V. Simplicity/scope**: Three verbs + creation enablement + named pins
  + `NQ` only; no GUI, no transport, no Netlist reshape (counter storage
  and polarity/alias state extend additively; topology/serialization code
  paths reused), dependency direction preserved (`cli → solvers/logic`,
  never reverse). PASS — two justified scope notes: flipping the pinned
  `add_gate` refusal of DFF/DLATCH (asserted in
  `test_logic_seq_kinds.cpp:125`) is required to make the feature usable
  at all; named pins + `NQ` are explicit user requirements (readability +
  standard flop dual outputs), designed as one alias table and one
  read-time inversion so no new state, pool, or eval semantics are
  introduced; both recorded in Complexity Tracking with the test/docs
  updates they entail.

## Project Structure

### Documentation (this feature)

```text
specs/001-seq-session-verbs/
├── plan.md              # This file (/speckit.plan command output)
├── research.md          # Phase 0 output (/speckit.plan command)
├── data-model.md        # Phase 1 output (/speckit.plan command)
├── quickstart.md        # Phase 1 output (/speckit.plan command)
├── contracts/           # Phase 1 output (/speckit.plan command)
└── tasks.md             # Phase 2 output (/speckit.tasks command - NOT created by /speckit.plan)
```

### Source Code (repository root)

```text
include/wengine/
├── cli/logic_registry.hpp      # + LogicStepSpec/LogicPokeSpec/LogicPeekSpec, registry append
└── logic/
    ├── netlist.hpp             # additive: sequential creation path (counters
                                # for ord 16/17) + pin alias table (D/CLK/EN/Q/NQ)
                                # + PinRef/BlockPin output selector + per-sink
                                # polarity array + nq_fanout + per-NQ ignore flag
    ├── seq_state.hpp           # untouched (pool + serials; NQ is read-time NOT Q)
    └── seq_step.hpp            # + subset-step core (global = all flops; clock filter
                                # = subset); sink polarity applied at resolution sites

src/cli/commands/
├── logic.cpp                   # workspace definition; + poke/cycle session maps
└── logic/
    ├── common.hpp/.cpp         # shared helpers (+ clock-domain + poke-map helpers;
                                # pin_text shows named seq pins; collect_pins lists Q+NQ)
    ├── add.cpp                 # accept DFF/DLATCH (exactly 2 inputs)
    ├── step.cpp                # NEW verb (domains incl. NQ-driven nets)
    ├── poke.cpp                # NEW verb (named spelling; numeric aliases accepted)
    ├── peek.cpp                # NEW verb (Q + NQ per flop)
    └── ...                     # existing verbs untouched (wire/tie/ignore/list/solve/
                                # truth gain named pins + NQ free via parse_pin)

src/logic/yaml_store.cpp        # NQ wire sources persist as text (no schema bump);
                                # strict loader learns suffix via parse_pin + ignore-nq key

tests/
├── test_logic_seq_cli_step.cpp # NEW: step goldens (counter/shift vectors via verbs)
├── test_logic_seq_cli_poke.cpp # NEW: poke/peek + replay determinism + error cases
├── test_logic_seq_cli_pins.cpp # NEW: named-pin/NQ goldens (parse aliases, NQ inversion,
                                # fanout/interface counts, YAML round-trip with NQ)
└── test_logic_seq_kinds.cpp    # UPDATE: creation refusal flips to acceptance
```

**Structure Decision**: Single-project CLI layout (existing `src/cli/commands/logic/`
per-verb TUs + header specs in `logic_registry.hpp`); engine changes are
additive header-only functions beside `seq_step` — no new directories,
no GUI or transport code.

## Complexity Tracking

> **Fill ONLY if Constitution Check has violations that must be justified**

| Violation | Why Needed | Simpler Alternative Rejected Because |
|-----------|------------|-------------------------------------|
| Flip pinned `add_gate` refusal of DFF/DLATCH (test_logic_seq_kinds:125 asserts failure) | Feature is unusable/testable from REPL without creating flops in-session; spec stories assume session-built sequential circuits | Hand-placing gates is test-white-box only, not reachable from any verb; keeping the refusal forces a second creation verb, duplicating `add` for no gain |
| `Netlist::next_counter` growth (array 8 → cover ordinals 16/17, additive) | Per-kind never-reused counters (`DFF1…`) must persist like combinational ones | Storing seq counters outside `Netlist` splits one convention across two owners and breaks save-YAML ID derivation (IDs already encode kind+counter) |
| `NQ` second output + per-sink polarity array + `nq_fanout` + per-NQ ignore flag defaulting to ignored (all additive; fresh-state default hides dangling NQ until `use`d or wired) | Standard flop dual outputs are explicitly requested; NQ must be wireable, so sinks need inversion state somewhere | NQ-as-pseudo-gate breaks serial identity and doubles topology; second `gate_out` value touches every eval loop for a value computable at read; storing inverted values at wire time goes stale as Q steps |
