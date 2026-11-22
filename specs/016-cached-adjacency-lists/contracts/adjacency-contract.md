# Contract: Derived-Index Invariants (adjacency-contract.md)

**Feature**: [spec.md](../spec.md) FR-001..013/017..023 | **Research**: [research.md](../research.md) D1..D8 | **Model**: [data-model.md](../data-model.md)

The binding contract between the maintained indexes and every consumer.
Any implementation (tasks phase and beyond) that violates a clause here
fails the feature regardless of benchmark numbers.

## 1. Single source of truth

1.1. The netlist's explicit tables (`drivers[]`, `extra_drivers[]`,
`shared_meta`, `tie_rep`, pull config, per-kind counters) remain the
ONLY serialized, user-visible truth. Every index in this feature is
derived state: rebuildable from the tables alone, never serialized
(FR-011), never exposed through any public API (FR-013).

1.2. Single-writer rule: each index is written only by its owning seam
— successor adjacency by the mutation seam (`netlist.hpp` family),
tie index by the TieSet seam, feeder counters by the same events that
maintain `output_fanout`/`nq_fanout`, free-slot heap by add/remove.
No consumer mutates an index. No optimizer pass bypasses the seam
(spec edge case: the optimizer is a consumer and a mutator through
the seam, never a cache-maintenance bypass).

## 2. Answer identity (the equality contract)

2.1. Every consumer-visible answer equals, exactly, the answer of the
scan implementation it replaces, on every corpus net and every replay
sequence: compile orders/levels/verdicts (FR-003), cycle-probe verdicts
(FR-005), tie tables and verdicts (FR-007), slot assignment per add
(FR-017), removal release effects (FR-018), feeder verdicts (FR-019),
rewire effects (FR-020), dedup merge decisions/directions/final
netlists (FR-021/023).

2.2. Enumeration order is pinned: successor chains enumerate sinks in
ascending slot order; tie chains enumerate members in ascending flat
order. Bit-identity of queue pushes, level assignment, and member
readout depends on this order (Constitution II).

2.3. The reference oracle is the pre-change scan code extracted
verbatim to `tests/logic_scan_reference.hpp` — not a re-derivation.
Parity is proven by running both on the corpus (see
[parity-harness.md](parity-harness.md)).

## 3. Staleness and maintenance

3.1. Any seam event that changes indexed content either maintains the
affected index entries in place (cost proportional to the event's own
edges/pins — FR-008) or, on wholesale replacement (reset, checkpoint
load, optimizer whole-net rebuild), marks the indexes stale; a
consumer NEVER answers from stale state (FR-009). A stale answer
served anywhere in the interleaved corpus is a failing defect.

3.2. Per-mutation full rebuilds are forbidden on single-edge/single-
pin paths (wire, unwire, tie, untie, add, remove of one item). Bulk
rebuilds are confined to: load/reset (once), first use after
staleness (once), and the optimizer's whole-net rebuild paths. Bulk
writers that QUERY mid-build (the YAML replay's per-wire probe)
maintain incrementally instead — a query must never observe a
half-built index.

3.3. Loaders mark all indexes stale; the first consumer rebuilds once
(O(N+E)); repeated identical query sequences produce bit-identical
outputs (FR-010/012).

## 4. Cost bounds (the reason this feature exists)

| Operation | Bound | Source row (docs/benchmarks/logic-scale.md) |
|---|---|---|
| One Kahn compile | O(N + E) total; per pop O(out-degree) | compile-rebuild 4.4ms/gate; verdict CONFIRMED |
| One wire cycle probe | O(reachable cone × out-degree) | growth-wire 4.4ms/wire (kCap floor), kill item 3 |
| One gate removal | O(removed fanout + own inputs) | remove 3.2ms/gate, kill item 4 |
| One gate addition | O(log N) amortized | growth-add steepest slope, kill item 5 |
| One tie operation | O(affected class size) | remove-stage tie detach scans |
| Feeder predicate | O(1) | drives_shared full-pool per call |
| One rewire | O(replaced driver's out-degree) | bypass/dedup rewire_users scans |
| One dedup sweep | O(N + in-bucket pairs) expected | dedup 6.2ms/gate rising slope, kill item 6 |
| Pass-boundary re-derivations | ≤ 1 per pass, never per gate/pair | recount_fanout + guard rescan rows |

4.1. Bounds are contractual for bench-verified sizes; the committed
delta record (see [measurement-delta.md](measurement-delta.md)) is
the acceptance evidence, not asymptotics alone (baseline honesty
rule: cite table rows).

## 5. Edge-set semantics

5.1. Extra attachments are adjacency edges exactly like primaries
(011): cycles closing through extras are caught by the probe and the
compile; removal releases extras polarity-aware; rewiring respects the
shared-span freezes exactly as the scans do.

5.2. Edge filters stay at call sites: one adjacency serves both
`kAll` and `kCombOnly` consumers; the sequential-kind test is applied
while iterating (research D3). Flop-aware cycle semantics are
unchanged (FR-005).

## 6. Failure vocabulary

6.1. Undersized scratch refuses loudly through the standing
`core::Status` vocabulary — never partial results, never silent
truncation (FR-010). Hot paths throw nothing, allocate nothing after
warmup, and build no strings.

## 7. Guard rails for the tasks phase

7.1. No new top-level commands, no CLI surface changes, no
serialization version bumps, no GUI/TUI changes.

7.2. The Kahn-frequency and full-capacity-init rows stay untouched
(research D11/D12): any task proposing to reduce step-path recompiles
or fixpoint rebuild counts is out of scope until the post-feature
re-measurement re-ranks them.

7.3. `sizeof(Netlist)` is allowed to change (new derived members);
the shipped identity pins (widths/sentinels/caps) must remain true
(research D13).
