# Implementation Plan: Opt-In Delay-Honoring Evaluator (014)

**Branch**: `014-delay-honoring-evaluator` | **Date**: 2026-11-21 | **Spec**: [spec.md](spec.md)

**Input**: Feature specification from `/specs/014-delay-honoring-evaluator/spec.md`

## Summary

An opt-in timing evaluation mode — selected per invocation with a
`--with-delays` flag on the existing `solve`/`step`/`truth` verbs —
runs an event-driven, transport-delay wheel that honors the per-gate
`propagation_delay_ps` annotations 013 shipped (whole picoseconds;
un-annotated gates take the mode's unit delay, one picosecond) and
reports ordered event traces: every gate-output transition stamped
`t=<N>ps` in a pinned total order, through transient X/Z values, to
the drained settled state. The zero-delay fast path is untouched
(`eval.hpp`/`seq_step.hpp` gain nothing; every default invocation is
byte-identical; `bench_logic` measures unchanged). The wheel never
mutates `Netlist`/`SeqState`: the unchanged stepper still commits
state, the unchanged `solve`/phase-2 machinery still validates and
prints settled values, and the wheel's end-state is pinned equal to
the zero-delay result by test — trajectory parity holds by
construction, so the mode observes ordering (glitches, D-vs-CLK race
offsets) without altering outcomes. `truth --with-delays` adds one
per-row drain-time column (the assignment's critical path). All
decisions are anchored in [research.md](research.md) (D1–D12).

## Technical Context

**Language/Version**: C++20, GCC 13.3 baseline (zero-warning target
with the `wengine_cli` flag set).

**Primary Dependencies**: existing only — engine header-only
`INTERFACE` libraries (`wengine_core`, `wengine_logic`), `wengine_cli`
(Shell + ArgCursor), the `Netlist`/`SeqState`/funnel seams
(`types.hpp`, `block_interface.hpp`, `seq_step.hpp`). No new external
or vendored dependency; no new TU (the timing wheel is header-only
like the rest of the engine).

**Storage**: in-memory `Netlist` — **no struct change**: the u64
field already lives on `Gate` (types.hpp:333), is user-writable via
013's verbs, persists via `delay_ps:` in the YAML store and the world
checkpoint, and resets under O2 per its documented contract. The
timing mode is **read-only over it**: the unit default for
un-annotated gates is a mode-level rule, never written back. The
trace itself is per-invocation scratch — never serialized, never part
of `RunConfig`/`Snapshot`/workspace state (no schema, no version
bump anywhere).

**Testing**: plain-`assert` binaries via ctest; new
`tests/test_logic_timing.cpp` (hand-derived trace goldens → pulse
widths/placement, drain times, race offsets, 4-state transient
windows, refusals → parity tests → determinism re-runs);
`tests/test_cli_args.cpp` gains `--with-delays` completion vectors
(registry size stays 27 — no new verbs); two example workspaces
(`glitch_chain.yaml`, `race_window.yaml`) pin the demos with
expected traces; `wengine_test_assert` policy inherited via the
`test_` prefix; `docs/audit_catalogs.py` stays zero-missing/
zero-ghost.

**Target Platform**: Linux (WSL2 baseline), headless CLI + TUI (no
TUI change — parity by construction, the TUI composes the same
one-shot lines).

**Project Type**: C++20 physics/logic simulation engine + CLI
modules.

**Performance Goals**: none claimed for the wheel — it is a cold,
per-invocation observation path (constitution IV is satisfied by the
absence of a claim plus the unchanged `bench_logic`, exactly like
013's D9). The feature's measured duty is no-regression on the
existing fast path.

**Constraints**: determinism (pinned event order: time, then slot
order; integer-only time arithmetic with loud overflow); unit floor
(every cause precedes its effect by ≥1 ps, so the wheel is
well-founded and cannot oscillate within an instant); transport
semantics (no inertial filtering); byte-identical defaults
(flag-off output matches pre-feature everywhere); zero warnings;
loud budget cap on pathological transport nets (refusal, never
silent truncation).

**Scale/Scope**: one new engine header (`logic/timing.hpp`); three
verb TUs touched (`solve.cpp`, `step.cpp`, `truth.cpp`) plus a shared
trace-print helper in `common.hpp`; `kUsage` strings gain the flag;
one new test binary; two new examples; registry size unchanged (27);
no new benchmark, no serializer, checkpoint, Runner, GUI, or TUI
change.

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Pre-Phase-0 | Post-Phase-1 (design) |
|---|---|---|
| I. Physical/numerical correctness | Delay semantics must be exact whole picoseconds with a total, independently referenceable gate-level timing model | PASS — transport model with pinned 4-state gate tables anchored to IEEE 1364 gate semantics (timing-semantics.md D5); goldens hand-derived from path-delay arithmetic (pulse width = path difference), never from implementation readback; integer-only time axis with checked adds |
| II. Determinism & reproducibility | Traces must be a pure function of (netlist, annotations, stimulus); zero-delay output must not move | PASS — event order pinned (time, then slot order; buckets cannot self-interact because the unit floor makes every effect ≥ cause+1, research D3); per-invocation selection, never sticky/serialized; default path untouched and pinned byte-identical; state commits still owned by the unchanged stepper (trajectory parity by construction + by test) |
| III. Independent-reference testing | Multi-boundary verification is the house norm | PASS — independent reference (1364 tables + hand-derived path arithmetic) → wheel trace → CLI representation → example-workspace replay; parity tests (wheel end-state == zero-delay result, step trajectories equal across modes); registry/completion pins enumerated (cli-verbs.md) |
| IV. Measured performance only | The feature must not pretend hot-path relevance and must not regress the fast path | PASS — no benchmark touched, no claim made for the cold wheel (research D10); `eval.hpp`/`seq_step.hpp` gain zero code; the no-regression duty is measured by the unchanged `bench_logic` run in the suite |
| V. Simplicity, minimal scope, dependency discipline | Header-only engine modules; one command domain; no new vocabulary surfaces beyond the tasked flag | PASS — one new header beside the existing evaluators (no new TU, no framework); three flags on existing verbs inside the logic domain (registry size unchanged); `logic` stays standalone (timing.hpp never includes CLI types); no struct/schema/checkpoint/Snapshot change |
| Logic-domain constraints (append-only kinds, loud failures, optimizer opacity, single-driver rule, pinned conventions) | All named in the constitution | PASS — no `GateKind` change; settled X/Z and settled S=R=1 refuse exactly as today by riding the unchanged validation path (research D6); transient invalid windows render X, never coerced; 013's zero-is-un-annotated convention preserved (unit floor documented in output); optimizer untouched (its documented O2 reset already covers timing's read side) |

**Gate result**: no violations; no Complexity Tracking entries
required.

## Project Structure

### Documentation (this feature)

```text
specs/014-delay-honoring-evaluator/
├── spec.md              # The feature specification (/speckit.specify output)
├── plan.md              # This file (/speckit.plan output)
├── research.md          # Phase 0: decisions D1-D12, all clarifications resolved
├── data-model.md        # Phase 1: event/trace/scratch entities, window lifecycle
├── quickstart.md        # Phase 1: runnable validation scenarios
├── contracts/
│   ├── timing-semantics.md  # the mode's physics: unit floor, transport, 4-state
│   │                        # tables, seeds/anchors, drain guarantee, budget
│   ├── engine-seam.md       # timing.hpp API, invariants, consumers
│   └── cli-verbs.md         # --with-delays grammar on solve/step/truth, trace
│                            # format, registry/completion pins, docs duties
└── checklists/
    └── requirements.md  # spec quality validation (complete, all pass)
```

### Source Code (repository root)

```text
include/wengine/logic/
├── timing.hpp          # NEW header-only wheel: run_timing_window + seed helpers
|                        #   + 4-state gate eval (dominant-value tables) + transient-
|                        #   tolerant input resolution (funnel reused, X/Z passed
|                        #   through); fixed-capacity trace/scratch (no heap, no
|                        #   exceptions, no CLI includes); reads propagation_delay_ps
|                        #   (unit floor 1ps) — never writes anything
├── eval.hpp            # UNTOUCHED — zero-delay scalar solve + word eval stay the
|                        #   only default path (fast-path guarantee, research D10)
├── seq_step.hpp         # UNTOUCHED — commits, level settle, async apply unchanged;
|                        #   the step adapter snapshots Q around the unchanged calls
├── types.hpp            # UNTOUCHED — Gate::propagation_delay_ps already exists (:333)
└── block_interface.hpp # UNTOUCHED — funnel (resolve_net) reused read-only; the
                         #   transient-tolerant wrapper lives in timing.hpp
include/wengine/cli/
├── commands.hpp        # LogicSpec::kUsage / Solve / Step / Truth kUsage strings gain
|                        #   --with-delays (text only; grammar changes in the TUs)
└── logic_registry.hpp  # NO registry size change (27 stays; test_cli_args.cpp:689)
src/cli/commands/logic/
├── common.hpp/.cpp     # +print_timing_trace helper (one printer, three verbs) + flag
|                        #   spelling constant
├── solve.cpp           # +--with-delays: zero-vector zero-delay solve for the pre-state,
|                        #   seed diff of the applied assignment, wheel, trace block,
|                        #   then the unchanged settled lines (byte-identical tail)
├── step.cpp            # +--with-delays: unchanged async/level settle + edge loop; per
|                        #   edge: Q-diff seeds (NQ shares the timestamp), one window,
|                        #   'edge k:' separator for --cycles N; settled tail unchanged
└── truth.cpp           # +--with-delays: per-row wheel drain time (all-zero pre-vector,
                         #   same row convention), settle column in pretty + --csv forms
tests/
├── test_logic_timing.cpp # NEW binary: goldens -> 4-state windows -> refusals (cap,
|                        #   overflow, settled X/Z, S=R=1) -> parity (wheel == zero-delay;
|                        #   trajectories equal) -> determinism re-runs
└── test_cli_args.cpp    # +--with-delays completion vectors on the three verbs; size-27
                         #   registry pin unchanged (:689)
examples/logic/
├── glitch_chain.yaml   # NEW — inverter chain vs direct path into one gate, annotated
|                        #   variant; header shows the expected pulse trace
└── race_window.yaml    # NEW — annotated clock-to-Q flop feeding a D path; header shows
                         #   the expected arrival offsets
CMakeLists.txt           # test_logic_timing target (add_executable + link + add_test;
                         #   test_assert policy inherited); wengine_logic stays INTERFACE
benchmarks/             # UNTOUCHED — bench_logic is the no-regression proof (research D10)
docs/logic.md            # §10 verb grammar gains the flag; §12 Timing slot ships (the
|                        #   named follow-on lands)
docs/tests/logic.md      # page for the new binary; catalog audit stays zero-missing/zero-ghost
README.md                # flag in the verb list, runnable glitch/race examples, test list,
                         #   module-table wording
```

**Structure Decision**: single-project layout unchanged. The wheel is
one new engine header following the `eval.hpp`/`seq_step.hpp` pattern
(header-only `INTERFACE`, fixed stack-sized scratch, `core::Status`
failures); the CLI side touches only existing verb TUs plus one
shared printer helper; one new test binary; two new example
workspaces; no new directories, no new TUs, no registry growth.

## Complexity Tracking

No constitution violations to justify — the table is intentionally
empty. The one deliberate design weight is the loud event budget
(research D9): transport propagation over reconvergent fan-out can
multiply (gate × arrival-time) pairs far beyond gate count, so the
mode refuses loudly past a constant cap instead of pretending linear
behavior; its rejected alternative (no cap, relying on the wire-time
cycle rejection alone) is recorded in research.md rather than here,
since it was rejected, not traded.
