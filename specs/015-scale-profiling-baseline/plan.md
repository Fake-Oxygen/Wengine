# Implementation Plan: Measured Scale-Profiling Baseline for One-Million-Gate Logic Nets (015)

**Branch**: `015-scale-profiling-baseline` | **Date**: 2026-11-21 | **Spec**: [spec.md](spec.md)

**Input**: Feature specification from `/specs/015-scale-profiling-baseline/spec.md`

## Summary

The logic domain becomes capacity-parameterized: the fixed 1024-slot
`Netlist` (u16 slot ids, ~160 KB by value, stack scratch everywhere)
generalizes to a templated capacity with a dependent slot width, while
production keeps compiling against layout- and behavior-identical
aliases (`Netlist`, `PinRef`, `kMaxGates = 1024`) — zero churn in
world/solvers/CLI/yaml/serialize/GUI/TUI, byte-identical goldens. A new
`bench_logic_scale` harness instantiates the same shipping source at
≥1,000,000 gates (u32 slots, ~230 MB netlist + caller-owned heap
scratch), sweeps a growing size ladder (three-plus orders of magnitude)
with per-stage attribution — growth adds, wire bursts, forced topology
recompiles, steady scalar solve, bit-parallel word eval, shared-net
path, sequential stepping, optimizer passes (dedup isolated), remove
bursts, reset — under loud per-stage budgets, and the measured curves
are committed as the baseline page `docs/benchmarks/logic-scale.md`
(environment pinned, variance policy stated, zero extrapolated
numbers). Every super-linear suspect from the spec's FR-005 list gets a
measured verdict from its own size-doubling ratios; the page states the
operational measured-hotspot gate (no SIMD/threading/GPU/rework proposal
without citing these numbers) and the verbatim re-run procedure. A new
`test_logic_scale` pins scale-instantiation correctness (scalar/word
parity, bit-identical repeats, refusals, prefix-slice agreement) at a
ctest-practical size plus alias layout-identity `static_assert`s. No
optimization lands: the u16 alias measures unchanged everywhere. All
decisions are anchored in [research.md](research.md) (D1–D12).

## Technical Context

**Language/Version**: C++20, GCC 13.3 baseline (zero-warning duty via
the `wengine_cli` TUs that include the templated headers).

**Primary Dependencies**: existing only — header-only engine
`INTERFACE` libraries (`wengine_core`, `wengine_logic`); no new
external or vendored dependency; no new engine TU (templated cores stay
header-only; the YAML store TUs stay monomorphic on the alias).

**Storage**: in-memory `NetlistT<Cap>` instantiations; the committed
baseline is a documentation page (`docs/benchmarks/logic-scale.md`) with
pinned-environment numbers. No schema change anywhere: YAML workspace
(v1) and world checkpoint (v6) stay fixed-stride over the 1024 alias —
capacity-generic serialization is explicitly out of scope (research
D10).

**Testing**: plain-`assert` binaries via ctest; new
`tests/test_logic_scale.cpp` (links `wengine_core` alone, inherits the
`test_` assertion policy): alias layout-identity `static_assert`s,
scale-instantiation scalar/word parity, prefix-slice agreement,
bit-identical repeat solves, loud-overflow/refusal parity, at a
ctest-practical default size with an env-var override for manual larger
runs (research D9). Existing suite untouched (the alias is
type-identical). `docs/audit_catalogs.py` stays zero-missing/zero-ghost.

**Target Platform**: Linux (WSL2 baseline host for the committed
numbers), headless bench + ctest.

**Project Type**: C++20 physics/logic simulation engine + benchmark
harness.

**Performance Goals**: the deliverable is measurement, not speed — no
performance is claimed or improved. Reach targets (feasibility
arithmetic in research D11): a ≥1M-gate net builds, compiles, evaluates
in one session (expected wall cost minutes-to-~1 h at the 1M ladder
point, dominated by the very O(N²) costs the baseline exists to pin);
quick default ladder stays minutes-scale. Steady-state stages at 1M are
sub-second; mutation/compile stages are the measured bottleneck
candidates.

**Constraints**: FR-002 honest measurement (the harness drives the same
shipping source, instantiated at a wider capacity; the u32-vs-u16
instantiation fidelity note is stated in the report, never hidden);
loud budgets (unmeasured-at-size entries, never extrapolation);
deterministic harness construction (seeded, bit-identical shapes);
byte-identical alias behavior pinned by goldens + `static_assert`s;
peak footprint ≤ a stated memory budget with loud breach; benchmarks
never link the assertion policy; no SIMD/threading/GPU.

**Scale/Scope**: ≥1,000,000 live gates on the measured path; ~26
engine headers templated on capacity (research D4 maps the exact set);
one new bench binary, one new test binary, one new docs page; zero
changes to CLI verbs, serialization, world/solvers, GUI/TUI
(research-verified: no GUI/TUI file includes `wengine/logic`).

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Pre-Phase-0 | Post-Phase-1 (design) |
|---|---|---|
| I. Physical/numerical correctness | No numerical behavior may change at any capacity; scale results must be pinned against independent references | PASS — the templated cores are the same code paths (alias instantiation pinned type-identical by `static_assert`s + unchanged goldens); scale correctness pinned by scalar-vs-word cross-discipline parity and prefix-slice agreement, never self-graded readback (contracts/engine-seam.md) |
| II. Determinism & reproducibility | Slot-order iteration, free-slot policy, per-kind counters, snapshot/eval order must be identical at every capacity; harness must be reproducible | PASS — iteration order, lowest-free-slot policy, and counter rules are capacity-independent by construction (same loops, `Cap` only bounds them); harness nets are seeded xorshift → bit-identical shapes/checksums; committed numbers carry the pinned environment + variance policy; serialization untouched (alias) |
| III. Independent-reference testing | Multi-boundary verification required | PASS — boundaries: scale-instantiation parity tests (two evaluation disciplines + prefix slices) → `test_logic_scale` ctest → `bench_logic_scale` checksummed harness → committed report reproduction procedure; catalog audits (`audit_catalogs.py` benchmarks + tests) zero-missing/zero-ghost |
| IV. Measured performance only | This feature is the constitution's mechanism: opaque inputs, bench preset, no assertion policy, no optimization disguised as enablement | PASS — `bench_logic_scale` follows the `bench_signal_scale` discipline (argc-seeded, warmup-separated, `do_not_optimize`, checksums, per-stage budgets, no extrapolation); the instantiation-width fidelity caveat is documented in the report (research D1); FR-008 forbids optimization in this change |
| V. Simplicity, minimal scope, dependency discipline | Header-only engine modules; smallest coherent diff; no behavior fork | PASS-with-justification — capacity templating touches ~26 headers (the concrete need: FR-001 reach without reshaping the production struct, which docs/logic.md §12 pins as "Netlist never reshapes"); rejected simpler-looking alternatives recorded in research D1/D4; no new dependency, no new engine TU, no include-cycle risk (verified acyclic header graph) |
| Logic-domain contract (append-only kinds, single-driver, flop-aware cycles, per-kind counters, loud YAML refusals) | All contracts must hold identically at every capacity | PASS — the templated code is the same rule set with `Cap` as a bound; `GateKind` untouched; refusals byte-shaped as today on the alias; YAML store and checkpoint untouched (loud refusal policy unchanged) |
| Docs duties (README living spec, bench catalog CS3, `audit_catalogs.py`, Doxygen on new public APIs) | New bench/test/page must fit the catalog and audit regime | PASS — `## bench_logic_scale` + `## test_logic_scale` anchors with the mandated field schemas; README duty lines enumerated (research D7); the committed-numbers carve-out for the baseline page is explicit and minimal (index wording + page-internal pinning); Doxygen comments ship with the templated forms |

**Gate result**: no violations; no Complexity Tracking entries
required. The one deliberate design weight — capacity templating — is
justified against its rejected alternatives in research.md D1/D4
(engine-wide lift, macro knob, vector pool, shadow harness), and its
behavior-preservation burden is discharged structurally (aliases pinned
type-identical) rather than by testing hope.

## Project Structure

### Documentation (this feature)

```text
specs/015-scale-profiling-baseline/
├── spec.md              # The feature specification (/speckit.specify output)
├── plan.md              # This file (/speckit.plan output)
├── research.md          # Phase 0: decisions D1-D12, all clarifications resolved
├── data-model.md        # Phase 1: sweep/report entities, capacity/scratch model
├── quickstart.md        # Phase 1: runnable validation scenarios
├── contracts/
│   ├── engine-seam.md       # templated capacity + scratch API, alias guarantees
│   ├── bench-invocation.md  # bench_logic_scale/test_logic_scale CLI contracts,
│   │                        # output grammar, committed invocation
│   └── baseline-report.md    # the committed report's required content schema
└── checklists/
    └── requirements.md  # spec quality validation (complete, all pass)
```

### Source Code (repository root)

```text
include/wengine/logic/
├── types.hpp            # TEMPLATE ROOT: NetlistT<Cap>/PinRefT<Cap>, dependent
|                        #   slot_t (+level/fanout/count widths), capacity-derived
|                        #   sentinels (kNoDriver, kNoLeader, kBitScratchWords);
|                        #   aliases `Netlist`/`PinRef`/`kMaxGates`=1024 pinned
|                        #   type-identical (static_asserts; serialization-safe)
├── shared_net.hpp       # templated (slot params/loops) — same seam, Cap-bounded
├── tie_set.hpp          # templated; TieSnapshot becomes capacity-typed; the
|                        #   survivors[] scratch moves to caller-owned spans
├── block_interface.hpp  # templated; SolveDiag capacity-typed; per-solve full-cap
|                        #   init stays as-is (it is suspect #6 — measured, not
|                        #   silently fixed)
├── topo.hpp             # templated Kahn core; kahn_pure/rebuild/validate/reaches
|                        #   take scratch spans (indeg/queue/order/levels/seen);
|                        #   alias wrappers keep today's exact signatures, internal
|                        #   fixed stack scratch, noexcept pins (test_logic:808/816)
├── pins.hpp             # structurally coupled templating (PinRef/Netlist refs);
|                        #   text adapters stay alias-monomorphic
├── netlist.hpp          # templated mutation core (add/remove/wire/unwire/ties);
|                        #   text adapters (wire_text/tie_text) alias-monomorphic
├── eval.hpp             # templated solve/eval_block_word with scratch-taking cores;
|                        #   alias wrappers keep signatures (resolved/gate_out/
|                        #   in_pos stay internal stack scratch at 1024)
├── seq_state.hpp        # templated (SeqStateT<Cap> beside NetlistT<Cap>)
├── seq_step.hpp         # templated; scratch-taking cores + alias wrappers
├── optimize_barrier.hpp / optimize_passes.hpp / optimize_exact.hpp /
|   optimize_o2_{build,dedup,min,emit}.hpp / optimize.hpp
|                        # templated pipeline; Netlist by-value locals become
|                        #   caller-owned out-buffers in the cores; alias wrappers
|                        #   keep by-value semantics; exact/min caps unchanged
|                        #   (n≤10/m≤16, n≤8/m≤8 — capacity-free by design)
├── timing.hpp           # UNTOUCHED (014's wheel is CLI-invocation-only, not a
|                        #   sweep stage; its caller-owned TimingScratch pattern
|                        #   is the migration precedent, not a target)
├── layout.hpp           # UNTOUCHED (draw stage, not a sweep stage, alias consumer)
├── seq_config.hpp       # UNTOUCHED (embeds the alias Netlist; engine run path
|                        #   stays 1024 with loud caps — research D10)
└── yaml_store.hpp       # UNTOUCHED (v1 workspace stays fixed-stride on the alias;
                          #   capacity-generic serialization is a named follow-on)
include/wengine/{world/solvers,cli}/…  # UNTOUCHED — compile against the aliases
benchmarks/
└── bench_logic_scale.cpp # NEW harness: motif builders (plain layered, wide,
|                        #   deep-chain, mixed shared+pull+seq+delay), size ladder
|                        #   (alias anchor points + scale instantiation to ≥1M),
|                        #   per-stage timing with warmup separation, loud budgets,
|                        #   checksums + footprint reporting, committed-invocation
|                        #   mode; links wengine_core only
tests/
└── test_logic_scale.cpp  # NEW binary: alias identity static_asserts; scale parity
|                        #   (scalar/word, prefix slices, bit-identical repeats),
|                        #   refusal parity, default ctest size + env override
CMakeLists.txt            # bench_logic_scale (2-line bench block + comment) and
|                        #   test_logic_scale (add_executable + link + add_test)
|                        #   entries; nothing else
docs/
├── benchmarks/logic-scale.md  # NEW: ## bench_logic_scale (CS3 five fields) +
|                        #   the committed Baseline section (environment pin,
|                        #   variance policy, per-stage per-size tables, suspect
|                        #   verdicts + ratios, ranking, the measured-hotspot gate,
|                        #   re-run procedure, fidelity note)
├── benchmarks/index.md  # Pages bullet + toctree entry + one-sentence carve-out
|                        #   for the committed-numbers baseline page
└── tests/logic.md        # ## test_logic_scale entry (CS1 six fields)
README.md                 # L54-64 run list, L839-855 bench catalog paragraph,
|                        #   L992 brace list (+ test list line for the new test)
```

**Structure Decision**: single-project layout unchanged. The engine
stays header-only with the same acyclic include graph (verified in
research D4); templating introduces no new headers, no new TUs, no new
dependencies — the template parameters ride the existing function
signatures' types. The two new binaries follow the exact house
registration patterns; the one new docs page follows the exact CS3
catalog schema with one sanctioned, spec-mandated extension (committed
numbers, research D7). CLI verbs, serialization, world/solvers, GUI/TUI
are untouched — the user-facing capacity policy stays at 1024 loud until
the kill-list follow-ons land (research D10).

## Complexity Tracking

> **Fill ONLY if Constitution Check has violations that must be justified**

| Violation | Why Needed | Simpler Alternative Rejected Because |
|-----------|------------|-------------------------------------|
| — | — | — |

No constitution violations. The one deliberate design weight —
capacity templating across ~26 headers — is recorded and justified in
research.md D1/D4 (with the alias strategy discharging
behavior-preservation structurally), not here, because it is a weighed
design choice among recorded alternatives, not a violation needing a
waiver.
