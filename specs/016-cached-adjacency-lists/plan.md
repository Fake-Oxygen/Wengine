# Implementation Plan: Cached Adjacency Lists and Maintained Indexes for Logic-Domain Hot Paths

**Branch**: `016-cached-adjacency-lists` | **Date**: 2026-11-22 | **Spec**: [spec.md](spec.md)

**Input**: Feature specification from `/specs/016-cached-adjacency-lists/spec.md`

## Summary

Replace the logic domain's full-pool scans with incrementally maintained
derived indexes, one per measured kill-list family, with every observable
output bit-identical to the scans they replace:

- a successor adjacency (per-driver, slot-ordered, primary + extra edges)
  behind the Kahn compile, the wire-time cycle probe, gate removal, and
  driver rewiring;
- a tie-class member index (per-leader, slot-ordered) behind tie
  enumeration/untie/detach/reindex;
- a lowest-free-slot index behind gate addition (identical slot
  assignment);
- a shared-feeder counter behind the 011 freeze predicate;
- grouped structural-key comparison behind the all-pairs dedup;
- pass-boundary (not per-gate) whole-net re-derivations.

The 015 baseline (docs/benchmarks/logic-scale.md) is the measured
authority: kill-list rows cite it; before/after numbers are committed
beside it; nothing is optimized without a row.

## Technical Context

**Language/Version**: C++20, GCC 13.3 baseline (zero-warning target on
`wengine_cli`-adjacent TUs; logic headers compile clean under
`-Wall -Wextra -Wpedantic -Wconversion`).

**Primary Dependencies**: none new. All work lives in the header-only
logic domain (`wengine_core` INTERFACE lib): `types.hpp` (pool members +
`ensure_workspace`), `netlist.hpp` (mutation seam), `topo.hpp`,
`tie_set.hpp`, `shared_net.hpp`, `optimize_barrier.hpp`,
`optimize_passes.hpp`, `optimize_o2_dedup.hpp`. Consumers (eval,
seq_step, timing, layout, optimize pipeline, CLI verbs, YAML store,
tests) include these already — no dependency direction changes.

**Storage**: in-memory derived state only. No serialization format
changes (caches never serialized; checkpoint/YAML round-trips stay
byte-identical; loaders mark caches stale for lazy rebuild).

**Testing**: plain-`assert` binaries printing `test_x: OK`, registered
`add_executable` + `target_link_libraries` + `add_test`, assertion
policy inherited via the `test_` prefix (`wengine_test_assert`). New:
`tests/test_logic_adjacency.cpp` (parity + invalidation + allocation
corpus), `tests/logic_scan_reference.hpp` (test-only scan reference
implementations extracted pre-change). Existing `test_logic*` suite
must pass unmodified. Measurement via `bench_logic_scale` (bench
preset, opaque inputs, ≥5 reps, checksummed, per-size footprint).

**Target Platform**: Linux (project baseline; WSL2 host known-good for
ASAN, TSAN-via-Clang — not needed here).

**Project Type**: high-performance physics/logic simulation engine —
header-only engine modules plus CLI consumers.

**Performance Goals** (from the 015 kill list at 262144 gates, per-gate
medians): compile-rebuild 4.4ms/gate, optimize O0 9.9ms/gate (≈5 Kahns
per fixpoint iteration), dedup 6.2ms/gate, remove 3.2ms/gate,
growth-wire 4.4ms/wire (kCap-floor dominated), growth-add
0.49ms/gate with the steepest measured slope (≈3.9× per-gate per 4× n).
Targets: mutation-proportional cache maintenance; compile/probe/tie/
rewire costs proportional to edges/cone/class/fanout actually touched;
before/after tables committed to `docs/benchmarks/logic-scale.md`.

**Constraints**: bit-identical observable outputs everywhere (FR-003/
005/007/017/021/023); identical slot assignment (FR-017); hot paths
allocation-free after workspace warmup; loud `core::Status` refusals,
never partials; no public signature changes (FR-013); engine headers
stay header-only, exception-free, platform-free.

**Scale/Scope**: nets ≥1,000,000 live gates on the measured path (015
substrate). Scope is exactly the six index families above plus their
maintenance in the mutation seam; frequency-of-Kahn reductions
(step-path recompiles, O0 fixpoint rebuild count) are explicitly
deferred pending re-measurement after each Kahn becomes linear
(research D11).

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Gate | Verdict |
|---|---|---|
| I. Physical/Numerical Correctness | Independent reference for every changed result | PASS — the scan implementations themselves become the test-only reference ([contracts/parity-harness.md](contracts/parity-harness.md)); no numerical behavior changes |
| II. Determinism & Reproducibility | Bit-identical outputs, stable identities, pinned enumeration order | PASS — FR-003/010/012/017/021/023; all cache enumerations pinned to the standing slot order; identical slot assignment pinned by replay |
| III. Independent-Reference Testing | Multi-boundary verification; suite runs without optional deps | PASS — parity corpus (reference → engine → CLI verbs → YAML round-trip), new `test_`-prefixed binaries, no optional dependencies involved |
| IV. Measured Performance Only | Benchmark before optimizing; smallest change; re-measure | PASS — every targeted path is a committed kill-list row (docs/benchmarks/logic-scale.md); before/after recorded per [contracts/measurement-delta.md](contracts/measurement-delta.md); nothing without a row is touched (D11/D12 deferrals) |
| V. Simplicity, Minimal Scope, Dependency Discipline | Smallest coherent diff; no dependency reversals; header-only discipline; loud Status | PASS WITH NOTE — added derived state is the smallest change that removes measured quadratic rows (alternative: leaving the scans fails the scale goal); all writes funnel through the existing mutation seam; no new TUs, no new dependencies, no API breaks |

Pre-Phase-0 verdict: no violations. The V-note (new cache state) is
justified above and re-checked post-design below.

## Project Structure

### Documentation (this feature)

```text
specs/016-cached-adjacency-lists/
├── plan.md              # This file (/speckit.plan command output)
├── research.md          # Phase 0 output (/speckit.plan command)
├── data-model.md        # Phase 1 output (/speckit.plan command)
├── quickstart.md        # Phase 1 output (/speckit.plan command)
├── contracts/           # Phase 1 output (/speckit.plan command)
│   ├── adjacency-contract.md
│   ├── parity-harness.md
│   └── measurement-delta.md
└── tasks.md             # Phase 2 output (/speckit.tasks command - NOT created by /speckit.plan)
```

### Source Code (repository root)

```text
include/wengine/logic/
├── types.hpp              # Netlist gains derived cache members + ensure family (D1)
├── netlist.hpp             # mutation seam: sole cache writer (add/remove/wire/unwire/
│                          #   mark/unshare/retag/reindex paths)
├── topo.hpp                # Kahn drain + reaches consume successor adjacency
├── tie_set.hpp             # enumeration/untie/detach/reindex consume member index
├── shared_net.hpp          # extra-edge vocabulary shared with adjacency maintenance
├── optimize_barrier.hpp    # drives_shared consults feeder counters; roots/bypass fan out
├── optimize_passes.hpp     # rewire_users fanout-proportional; boundary recounts only
└── optimize_o2_dedup.hpp   # grouped-key compare replaces all-pairs loop

tests/
├── logic_scan_reference.hpp  # test-only: the pre-change scan implementations
└── test_logic_adjacency.cpp  # parity + invalidation + slot-allocation replay corpus

benchmarks/
└── bench_logic_scale.cpp     # unchanged harness; before/after recorded in docs

docs/
├── benchmarks/logic-scale.md  # after-numbers appended beside the baseline tables
└── logic.md                   # domain guide: derived-index contract note
```

**Structure Decision**: no new directories, no new TUs. The caches are
derived pool members beside the existing `ws_*` workspace (same
header-only discipline, same growth conventions); the mutation seam in
`netlist.hpp` remains the single writer exactly as the TieSet/SharedNet
seams already model. The parity reference lives test-side so the shipped
headers never carry two implementations.

## Complexity Tracking

> **Fill ONLY if Constitution Check has violations that must be justified**

| Violation | Why Needed | Simpler Alternative Rejected Because |
|-----------|------------|-------------------------------------|
| (none — re-check below) | | |

Post-Phase-1 re-check: still no violations. The one candidate (new
derived state in `Netlist`) is not a constitution violation — it
follows the existing derived-cache precedent (`topo_order`/`topo_level`
+ `dirty` are already derived pool state with the same lifecycle), now
extended per the measured kill list. Constitution compliance is
re-asserted for Phase 2: gates re-run per [contracts/parity-harness.md]
(tests) and [contracts/measurement-delta.md] (measurement) before any
task may claim Done.
