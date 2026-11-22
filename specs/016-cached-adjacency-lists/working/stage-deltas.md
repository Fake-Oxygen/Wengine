# Working bench rows (016) — raw data for US4 consolidation

## T010 — compile-rebuild (US1), motif 0

After-invocation: `bench_logic_scale 0 1024 16384 4 1 120 3`
(dev build bench preset `-O3 -DNDEBUG`; steps=4 sizes 1024/2580/6502/16384;
words=1, reps=3 — NOT the committed invocation; US4 re-runs
`0 1024 65536 4 5 21600 5` for the record.)

Before-numbers: committed 015 ladder (`docs/benchmarks/logic-scale.md`,
motif 0, `0 1024 65536 4 5 21600 5`).

| size | before (ns/gate) | after (ns/gate) | ratio |
| --- | --- | --- | --- |
| 1024 | 1506.8 | 159.9 | 9.4x |
| 2580 | — (ladder has 4096: 4039891.3) | 167.6 | — |
| 6502 | — | 163.0 | — |
| 16384 | 4296491.9 | 168.1 | ~25500x |

After-curve is flat per-gate across 16x growth (160→168 ns/gate):
the per-pop pool scan is gone; cost is out-degree proportional.
(The 015 verdicts predicted exactly this shape change: the baseline
row was capacity-proportional — "quadratic in disguise" along the
deployment diagonal.)

Footprint note (SC-005): fp= rises with the pool-sized edge arrays
(1024: 392104 B incl. ~330 KB edge pool; 16384: 6259624 B incl.
~5.2 MB). Accounted, not hidden.

## T013 — growth-wire (US2), motif 0

Same after-invocation as T010 (`0 1024 16384 4 1 120 3`).

| size | after (ns/wire) | T010-row (pre-US2, ns/wire) | ratio |
| --- | --- | --- | --- |
| 1024 | 3051.2 | 19621.5 | 6.4x |
| 2580 | 8116.8 | 47960.7 | 5.9x |
| 6502 | 18561.9 | 115356.0 | 6.2x |
| 16384 | 42766.7 | 280879.9 | 6.6x |

Per-wire cost still rises with size on forward-built layered nets —
the honest cone cost (reachable cones grow with net depth; FR-004
contracts cone-proportionality, not constant time). Committed-ladder
row-for-row comparison lands in US4.

## T017 — growth-add and remove (US5), motif 0

Same after-invocation (`0 1024 16384 4 1 120 3`). Before-rows are the
T010 after-rows (pre-US5: free-slot scan + full-pool sink sweep).

growth-add per-gate (ns/gate):

| size | pre-US5 | post-US5 | ratio |
| --- | --- | --- | --- |
| 1024 | 4332.3 | 2427.0 | 1.8x |
| 2580 | 8210.5 | 3385.1 | 2.4x |
| 6502 | 14980.0 | 2724.8 | 5.5x |
| 16384 | 33802.1 | 2338.5 | 14.5x |

Curve is flat-to-falling across 16x growth (2427→3385→2725→2339:
variance, not growth) — the ~3.9x-per-4x slope from the 015 verdicts
table is gone; allocation is O(log n) heap work per add.

remove per-gate (ns/gate):

| size | pre-US5 | post-US5 | ratio |
| --- | --- | --- | --- |
| 1024 | 16633.4 | 550.3 | 30x |
| 2580 | 43762.6 | 597.3 | 73x |
| 6502 | 100886.1 | 601.0 | 168x |
| 16384 | 236165.3 | 609.2 | 388x |

Curve flat per-gate (550→609 ns): teardown is fanout-proportional.

## T021 — tie enumeration (US3), member micro-probe

Probe: N-gate pool of 2-pin tie classes; one `tie_class_members`
call on a fixed 2-member class, chain (new) vs scan (oracle).
`tie_pins` build reuses the same seam in both (identical tables).

| pool N | chain (ns/call) | scan (ns/call) | ratio |
| --- | --- | --- | --- |
| 4000 | ~1 | 6329 | ~6300x |
| 16000 | ~1 | 30114 | ~30000x |
| 64000 | ~1 | 110563 | ~110000x |

Chain cost is class-proportional (constant here); scan cost tracks
pool size. The remove-stage delta attribution lands in US4's
committed ladder (detach/untie survivor collection rides the same
index).

## T025/T027 — optimize and dedup (US6), motif 0

Same after-invocation (`0 1024 16384 4 1 120 3`). Before-rows are the
T010-run rows (pre-US6: all-pairs key rebuilds per pair).

optimize per-gate (ns/gate):

| size | pre-US6 | post-US6 | ratio |
| --- | --- | --- | --- |
| 1024 | 29665.8 | 5640.1 | 5.3x |
| 2580 | 73148.0 | 8116.8 | 9.0x |
| 6502 | 159450.9 | 6637.7 | 24x |
| 16384 | 377123.9 | 5015.4 | 75x |

Curve flat across 16x growth (variance only): the fixpoint's Kahns are
linear now (US1), the per-iteration whole-net guard scan is hoisted
once per invocation (T026), and recounts were already
boundary-only (audited, not moved: the exact pass reads fanout between
const-fold and exact-simplify).

dedup per-gate (ns/gate):

| size | pre-US6 | post-US6 (in-bucket) | ratio |
| --- | --- | --- | --- |
| 1024 | 34343.0 | 479.2 | 72x |
| 2580 | 76597.8 | 323.4 | 237x |
| 6502 | 196529.5 | 303.6 | 647x |
| 16384 | 505765.3 | 305.4 | 1656x |

Visits dropped from all-pairs to in-bucket pairs with identical merge
order (global ascending (s, t) preserved; cross-bucket pairs are
proven rejects; selective rehash after every merge keeps buckets
exact). Checksums identical to the pre-change runs (same nets, same
merges). Committed-ladder slope evidence lands in US4.
