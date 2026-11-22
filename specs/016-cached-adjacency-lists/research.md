# Research: Cached Adjacency Lists and Maintained Indexes (016)

Phase 0 output. Every decision below resolves a design point the spec
deliberately deferred (spec Assumptions: "the spec contracts the cost
bounds and the answer identity, never the structure"), anchored to the
committed baseline rows in [docs/benchmarks/logic-scale.md](../../docs/benchmarks/logic-scale.md)
(verdicts + kill list). Format per decision: Decision / Rationale /
Alternatives considered.

Baseline rows this feature attacks (262144 gates, per-gate medians):
optimize O0 9.9ms, seq step 8.4ms (Kahn-frequency items, D11), dedup
6.2ms, compile-rebuild 4.4ms, growth-wire 4.4ms/wire (kCap-floor),
remove 3.2ms, growth-add 0.49ms with the steepest slope (≈3.9× per-gate
per 4× n).

---

## D1 — Where the caches live: derived pool members beside `ws_*`

**Decision**: the successor adjacency, tie member index, free-slot
index, and shared-feeder counters become `Netlist` members (types.hpp)
beside the existing derived topology cache (`topo_order`/`topo_level`/
`dirty` — the established in-struct precedent), each with its own
`ensure_*` growth function following the `ensure_workspace` conventions
(grow-on-demand, never shrink, no-op when sized). They are *persistent
derived state* (incrementally maintained), not per-call scratch.

**Rationale**: FR-008 forbids per-mutation full rebuilds, so the
adjacency must survive across operations — per-call `ws_*` scratch
semantics do not fit. The pool already owns derived topology state with
exactly this lifecycle, so the pattern is house-idiomatic. Mutation
seam writers in `netlist.hpp` become the sole maintainers (same
single-writer rule as `tie_rep` in tie_set.hpp and `shared_meta` in
shared_net.hpp).

**Alternatives considered**: (a) caller-owned scratch rebuilt per query
— violates FR-008 for per-wire probes; (b) a side structure owned by
the CLI/controller — splits truth from the netlist and breaks save/
load/reset paths; (c) `std::vector` per gate — per-gate allocation,
violates the allocation-free workspace discipline.

---

## D2 — Successor adjacency: per-driver slot-ordered intrusive edge lists

**Decision**: flat-array intrusive lists keyed by driver slot:
`succ_head[Slot]` plus dense edge arrays (`succ_next[EdgeIdx]`,
`succ_sink[EdgeIdx]`, `succ_input[EdgeIdx]`, `succ_extra[EdgeIdx]`,
`succ_inv[EdgeIdx]`). One entry per live edge (primary + every shared
extra), each carrying the sink slot, sink input, and whether it came
from an extra attachment (the 011 rule: extras are driver-graph edges
exactly like primaries). Edge indices recycle through a free-edge list.
Insertion places the new edge in ascending sink-slot position within
the driver's list (O(out-degree) per insert — degree-bounded);
removal unlinks after a degree-bounded find.

**Rationale**: three hard requirements select this shape together:
(i) FR-003 bit-identity — the Kahn drain and DFS probe must see sinks
in ascending slot order per pop, which ordered lists preserve exactly;
(ii) FR-008 — O(1) amortized insert/unlink per single-edge mutation;
(iii) hot-path discipline — flat arrays, no per-gate allocation,
degree-bounded ops that never touch the wider pool. The measured
growth-wire row is kCap-floor dominated (4.4ms/wire flat), so
degree-bounded insert cost (microseconds) is negligible against it.

**Alternatives considered**: (a) CSR rebuilt per compile — O(N+E) per
rebuild violates FR-008 on the per-wire probe path; (b) unordered
prepend + reverse-on-read — reverses multi-pop enumeration order and
risks bit-divergence in queue pushes; (c) per-driver sorted dynamic
vectors — same asymptotics, worse memory locality and per-gate
allocation; (d) tombstone-only removal — compaction complexity and a
second validity vocabulary for no measured gain.

**Sizing**: edge capacity is bounded by live gates × inputs ×
(1 + kMaxSharedExtra) in the worst case, but the free-edge recycle list
keeps storage at *current* edge count + slack; `ensure` grows
geometrically like every other pool member.

---

## D3 — Edge-filter views: one adjacency, filter at read

**Decision**: one adjacency stores every edge. `kCombOnly` consumers
(the stepper order, the wire probe, flop-aware validation) test
`is_sequential_kind(nl.slots[sink].kind)` while iterating — the same
per-edge test the scans perform today, now on degree-sized iteration
instead of pool-sized.

**Rationale**: a second comb-only adjacency would double maintenance
on every mutation for a check that costs one branch per visited edge;
the existing `EdgeFilter` call sites already encode the rule. One
representation, semantics at the call sites (spec Assumption: "the
caches store edges; the edge-filter semantics stay at their existing
call sites").

**Alternatives considered**: (a) two cached views (all + comb-only) —
maintenance doubling for no measured benefit; (b) storing a
precomputed "comb edge" bit per adjacency entry — the bit duplicates
`Gate::kind`, a second truth to maintain on retags.

---

## D4 — Tie member index: per-leader flat-ordered intrusive lists

**Decision**: `tie_head[leader_flat]` + member chains over flat pin
positions, members linked in ascending flat order (the standing
slot-order enumeration), maintained by `tie_pins`/`untie_pin`/
`detach_slot_ties`/`reindex_kept_ties` (the TieSet seam stays the sole
writer, exactly as today). Enumeration, group-size, survivor
collection, re-star reads walk the chain; class merges splice chains
(ordered merge of two ascending chains); leader re-election re-heads.

**Rationale**: every tie operation's cost bound (FR-006: proportional
to affected class size) falls out of chain walks; ascending order
reproduces `tie_class_members`'s enumeration exactly (FR-007
bit-identity); splicing two ordered chains costs the merged size — the
same asymptotic as the current full-pool re-star with the pool factor
removed.

**Alternatives considered**: (a) rebuild member buckets per query —
violates FR-008's spirit for untie storms; (b) per-class `std::vector`
— allocation per class, and classes merge/split constantly; (c) keeping
the scans but memoizing sizes only — leaves untie/detach/reindex at
full-pool cost, the remove-stage row.

---

## D5 — Free-slot index: a min-heap of freed slots

**Decision**: an array-backed binary min-heap of dead slot indices,
persistently maintained: `add_gate` pops the heap top when non-empty
(= the lowest free slot; identical answers to the linear scan),
appends at pool end when empty (the growth path — today's comment
"allocation order matches the old fixed pool" stays true verbatim);
`remove_gate` pushes the freed slot. Load/reset rebuilds the heap in
one O(N) pass (all dead slots, heapified deterministically — a
bottom-up heapify over ascending indices yields a canonical layout for
identical states).

**Rationale**: the verdict table calls growth-add the one genuinely
super-linear growth cost (≈3.9× per-gate per 4× n, "projects to overtake
wire near 1M — the mutation-side item to kill first"). A heap answers
lowest-free in O(log N) with identical results to the scan (FR-017 is
an equality contract, and heap-pop-lowest is exactly the scan's
answer by definition of a min-heap of free slots).

**Alternatives considered**: (a) bitset word-scan — O(N/64) per add,
still pool-proportional, exactly the term the kill list says to kill;
(b) sorted free-list with binary search — same O(log N) but O(N) worst
insert/remove bookkeeping; (c) cursor + "no holes below cursor"
invariant — breaks the moment a removal opens a hole below the cursor
(recycled slots MUST be reused lowest-first to match the scan).

**Determinism note**: binary heaps are layout-deterministic for
identical operation sequences (no tie-breaking ambiguity: slots are
unique integers). Replays compare slot-by-slot, so the heap itself
needs no canonical form beyond the pop-min answer.

---

## D6 — Shared-feeder state: a per-slot maintained counter

**Decision**: `feeds_shared[Slot]` counts, per driver slot, how many
of its live edges land on shared-marked inputs. Maintained at the same
seam points that already maintain `output_fanout`/`nq_fanout`:
wire/extra-append increments when the destination input carries the
mark; unwire/extra-erase/remove decrements; `mark_shared` on an already-
driven input back-increments the existing driver (pre-marking then
wiring is the other order and is symmetric); slot death zeroes the
slot's own counter and releases its outgoing edges' contributions.
`drives_shared` becomes `feeds_shared[s] > 0` (plus the existing
direct input checks where the predicate means "drives a shared
*input*" vs "has a shared input" — the two existing predicates
`drives_shared`/`has_marked_input` stay distinct; only the former
changes implementation).

**Rationale**: the verdict table shows `collect_barrier_roots` paying
a full-pool scan per root test (O(V²) per DCE/O2 pipeline, a third
factor stacked inside the dedup pair loop — the only cubic-shaped
corner in the domain). The mark table already changes only through the
mark/unshare/wire/unwire seam, so a maintained counter is exact by the
same argument that makes `output_fanout` exact: every counter-moving
event is a seam event. Verdict identity is equality of a boolean
(count > 0) with the scan — pinned by the parity corpus including
mark-then-wire, wire-then-mark, promote, and erase orders (contracts/
adjacency-contract.md §Feeder).

**Alternatives considered**: (a) answer from the successor adjacency by
walking the driver's edges and testing each sink's mark — fanout-
proportional (FR-019 allows it) but strictly worse than O(1) for a
predicate called per gate; (b) a shared-feeder bitset of slots — same
maintenance, no extra value over a counter; the counter also enables
future "how many" queries for free; (c) keep the scan, memoize per
pipeline — leaves per-bypass/per-pair calls at full-pool cost.

---

## D7 — Dedup grouping: deterministic key-hash buckets, exact in-bucket compare

**Decision**: the all-pairs loop is replaced by: build each candidate
gate's existing sorted-key multiset (the pass's current key semantics,
unchanged); hash the (kind, input-count, sorted-key-multiset,
merge-mode) tuple with a fixed deterministic 64-bit mixer (FNV-1a or
splitmix-style — constant seed, no runtime randomization); bucket
candidates via workspace chains (`bucket_head[HashMod]` + member
links) preserving ascending slot order within buckets; compare exact
keys within each bucket in ascending (s, t) pair order, firing merges
exactly where the all-pairs scan fires them (higher into lower, same
skips: sinks, sequential-feeders, shared feeders, ignored masks).

**Rationale**: FR-021 demands identical merge decisions/directions/
final netlist; ascending-order bucket chains reproduce the all-pairs
visit order for every pair that all-pairs would compare, and skip
(cheaply) every pair it would reject on the first key mismatch — the
baseline's rising dedup slope (1.69× at 262K, "the only evaluation-
adjacent scan with a rising slope") comes exactly from live universal-
gate pairs accumulating, which bucketing prunes. Hash quality affects
only speed, never correctness: collisions fall through to the exact
compare (spec edge case: "collisions must resolve by exact structural
compare, never by hash equality alone").

**Alternatives considered**: (a) keep all-pairs with an early-exit
sort order — the slope still rises with pair count; (b) full
structural hashing with canonical gate interning (DAG hashing) — a
larger redesign that changes merge semantics timing (fixpoint
interactions) for no additional contract gain; (c) sort-all-keys-then-
group — same cost class as buckets, more workspace churn.

**Fixpoint note**: the pass re-runs to fixpoint unchanged; each sweep
is O(N + in-bucket pairs) expected, and the exact-compare guarantee
makes every sweep's decisions identical to the corresponding all-pairs
sweep (parity corpus pins per-sweep state, not just fixpoint state).

---

## D8 — Whole-net re-derivations: once per pass boundary, never per gate

**Decision**: `recount_fanout` stays at pass entry/exit (its documented
purpose: avoid drift after direct driver surgery inside passes) — once
per pass, O(N·I), acceptable per FR-022 ("at most once per pass").
The repeated whole-net guards hoist instead: `has_sequential_gates(nl)`
is computed once per pipeline invocation and threaded to the per-level
branches that currently each rescan (optimize.hpp calls it ~5× per
run); `any_shared_mark`/`has_buffer_gates`/`any_pull` similarly
evaluate once per invocation, not once per pass where they are
invocation-invariant. Nothing trusts stale state across a mutation
boundary: any pass that mutates re-derives once, then maintains.

**Rationale**: FR-022's bound is "at most once per pass, never per
gate or per pair, without trusting stale maintained state". Boundary
recounts are the conservative reading of "without trusting stale" —
passes do direct surgery (bypass rewrites `drivers[i]` in place), so
boundary re-derivation is the correctness-preserving minimum; hoisting
invocation-invariant guards is free.

**Alternatives considered**: (a) trust seam-maintained counters across
pass surgery — requires rerouting every pass's direct driver writes
through the seam in this same change (scope creep beyond the measured
rows; the boundary recount already satisfies the FR); (b) drop
recounts entirely — the comments mark them as drift protection after
surgery; removing them changes observable behavior risk for zero
measured gain.

---

## D9 — Parity harness: pre-change scans preserved as a test-only reference

**Decision**: before any production header changes, the current scan
implementations (Kahn drain successor scan, `reaches` successor
discovery, tie member/group/re-star scans, `drives_shared` scan,
all-pairs dedup, lowest-free-slot scan) are extracted verbatim into
`tests/logic_scan_reference.hpp` (test-only, links `wengine_core`,
asserts enabled via the `test_`-prefix policy). The new
`test_logic_adjacency.cpp` builds the corpus (layered combinational,
shared nets with extras + NQ polarity, tie classes across merges/
re-elections/dissolves, sequential nets with flop feedback, dangling
drivers, deliberate cycles incl. extra-closing, mark-then-wire vs
wire-then-mark orders) and, for every operation replay, runs reference
vs new and compares: full netlist state (slots, kinds, drivers, extras,
ties, marks, pulls, fanout counters, per-kind counters), every
verdict, every emitted order/level array, and slot assignment at every
add. Interleaved mutation/query sequences pin invalidation (spec FR-
009's stale-answer defect class). An ASAN-instrumented ctest variant of
the same binary pins allocation-free steady-state (count allocations
after warmup == 0 via a counting new/Delete link trick is NOT used —
instead the existing project pattern: allocation-freedom is pinned by
the bench footprint rows, and the test asserts repeated-query
bit-identity which allocation churn would perturb only via addresses,
not values — so the test pins values and the bench pins footprint).

**Rationale**: constitution Principle I/III (independent reference,
multi-boundary). The scans are the trusted oracle — they are today's
shipped behavior with the golden suites behind them. Extracting them
verbatim (not rewriting "equivalent" logic) keeps the oracle honest.
The comparison levels mirror the 015 parity disciplines (scalar vs
word) that previously caught real bugs.

**Alternatives considered**: (a) frozen golden vectors only — loses
corpus generality and lets both implementations drift identically
wrong on un-covered shapes; (b) keeping the scans in shipped headers
behind a flag — two implementations in the hot headers, the exact
bloat the constitution's simplicity rule forbids; (c) property-based
random nets — the deterministic corpus is reproducible under ctest;
randomness would need its own determinism story (out of scope).

---

## D10 — Measurement protocol: the standing bench, extended recording

**Decision**: `bench_logic_scale` runs unchanged (same stage lines,
same preset `--preset bench`, warmup separated, opaque inputs, ≥5
reps, checksums, per-size footprint, UNMEASURED-AT-SIZE loudness).
Before/after is recorded by appending a dated delta section to
`docs/benchmarks/logic-scale.md` (environment re-pinned, ladder re-run
at the same sizes, per-stage ratio tables old→new, footprint delta
including the cache cost) per [contracts/measurement-delta.md](contracts/measurement-delta.md).
The pre-change numbers are already committed (015); the after-run
cites them row-for-row.

**Rationale**: constitution IV: benchmark → smallest change → verify →
re-run. The harness already isolates exactly the stages this feature
targets (growth-add, growth-wire, compile-rebuild, remove, optimize,
dedup) plus the must-not-regress stages (solve/word/shared-solve/step/
reset) — no new harness is justified.

**Alternatives considered**: (a) a new micro-bench per cache — more
catalog surface (docs/audit_catalogs.py duties) for numbers the
existing stage lines already produce; (b) measuring only at 262K —
loses the slope evidence that distinguishes "linear now" from "smaller
constant".

---

## D11 — Explicitly deferred: Kahn-frequency reductions (kill-list #1/#2)

**Decision**: out of scope for this feature, recorded as the next
candidates: (1) the step path's per-edge validate/compile (8.4ms/gate
row) and (2) the O0 fixpoint's ≈5 Kahns per iteration (9.9ms/gate row)
are *call-frequency* costs; this feature makes each Kahn linear (O(N+E)
via D2), which collapses their per-call quadratic term, but does not
reduce how often they run. Frequency reduction (incremental comb-
order maintenance, fixpoint rebuild avoidance) proceeds only if the
post-feature re-measurement still shows those rows dominating.

**Rationale**: constitution IV's smallest-change rule and the
measured-hotspot gate ("no vectorization/threading/GPU proposal
proceeds without citing one of these rows") cut both ways: after the
adjacency lands, the honest next baseline decides; pre-optimizing
frequency now would be optimizing an unmeasured post-change state.

**Alternatives considered**: (a) folding incremental-order maintenance
into this feature — triples the diff (touching seq_step's two-phase
contract) for rows that may already fall below others once linear;
rejected; (b) declaring them non-problems — dishonest; the kill list
ranks them #1/#2.

---

## D12 — Explicitly deferred: full-capacity-init floor

**Decision**: out of scope: the "full-capacity init per solve/word/
step" row (verdict: CONFIRMED present, capacity-proportional, "flattens,
never steepens") is a linear per-op constant, not a growth term. The
evaluation-side rows sit two orders of magnitude below compile/mutation
rows. No change without a post-feature re-measurement showing it
dominant.

**Rationale**: same constitution-IV argument as D11; also the spec's
Assumptions already name the ruled-out list to keep planning honest.

---

## D13 — Type/workspace impact and the alias-identity pins

**Decision**: new `Netlist` members extend the templated
`NetlistT<kCap>` (types.hpp) with capacity-derived bounds like every
other member; each gains an `ensure_*` grower following the
`ensure_workspace` pattern. The 015 identity pins are re-checked:
`tests/test_logic_scale.cpp` pins widths/sentinels (`sizeof(Slot)==4`,
`kNoDriver==0xFFFFFFFF`, `kMaxGates==1048576`) — none pin
`sizeof(Netlist)` bytes, so adding members breaks no shipped
`static_assert`; the pins' *intent* (alias identity to the production
instantiation) is untouched. Serialization: no cache appears in
checkpoint/YAML formats (FR-011); loaders mark all caches stale and
the first consumer rebuilds (O(N+E) once, the standing load-time
pattern).

**Rationale**: keeps the 015 capacity-templating contract intact (the
scale instantiation must see the same members); avoids inventing a
second workspace family when the house pattern (grow-on-demand spans)
exists; keeps wire formats byte-identical by construction.

**Alternatives considered**: (a) cache members on a side struct hung
off `Netlist` — a second truth container with its own lifetime story;
(b) `static`/thread-local caches — breaks multi-netlist determinism
and the workspace precedent; (c) serializing caches — rejected
outright by FR-011 (derived state, rebuildable, round-trip
byte-identity required).

---

## D14 — Research summary: NEEDS CLARIFICATION items resolved

The spec contained zero `[NEEDS CLARIFICATION]` markers (its open
points were planning decisions by design). All deferred decisions are
now resolved: D1 placement, D2 adjacency shape, D3 filter views, D4 tie
index shape, D5 free-slot index, D6 feeder state, D7 dedup grouping,
D8 re-derivation bounds, D9 parity oracle, D10 measurement protocol,
D11/D12 deferrals with re-measure gates, D13 typing/serialization. No
unknowns remain for Phase 1.
