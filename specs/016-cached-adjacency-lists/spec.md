# Feature Specification: Cached Adjacency Lists and Maintained Indexes for Logic-Domain Hot Paths

**Feature Branch**: `016-cached-adjacency-lists`

**Created**: 2026-11-22

**Status**: Draft

**Input**: User description: "What: Replace full-matrix scans in topology, cycle check, and tie enumeration with cached adjacency lists. Why: Current scans are O(N^2) per wire/topo; they dominate long before 1M gates and block every scale goal."

**Scope extension (same session)**: "Also investigate what other high cost paths are present in this module and append optimizing them." — a source-verified sweep of the logic module found four more full-pool scan families on the measured path (per-add slot allocation, per-remove sink sweep, per-gate shared-feeder predicate, optimizer pass scans including the all-pairs dedup); they are appended here as User Story 5 and User Story 6 so one feature kills every measured quadratic path in the domain.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Topology compile reads cached adjacency, not the whole pool (Priority: P1)

A developer authors a large logic net (gate adds and wires interleaved). Every
mutation dirties the topology and the next compile re-runs Kahn's algorithm.
Today each popped gate triggers a scan of every live slot and every input to
find its successors, so one compile costs roughly pool-size-squared — at the
015 baseline sizes this is the compile-rebuild stage's dominant cost. After
this feature, the compile consults a cached successor adjacency (each driver
slot's list of driven sinks, primaries and shared-net extras alike) so one
pop touches only its own recorded successors, and one compile costs what the
edge count actually is. The compile's observable outputs are unchanged by
construction: identical emission order, identical per-slot levels, identical
live counts, and identical verdicts (combinational cycles and dangling
drivers still fail loudly through the same path) on the same netlist.

**Why this priority**: The per-mutation compile runs on every authoring verb
and every stepper rebuild; it is the single widest multiplier on net
authoring at scale, and the 015 baseline explicitly pinned it as a scan to
kill.

**Independent Test**: Can be fully tested by generating the baseline corpus
of nets (layered combinational growth, shared nets, sequential elements,
dangling drivers, deliberate cycles), compiling each before and after the
change, and asserting bit-identical order/levels/verdicts — plus one
compile-rebuild benchmark run at the largest baseline size showing the
flattened cost curve. Delivers value alone: compile at scale without the
quadratic scan.

**Acceptance Scenarios**:

1. **Given** any netlist from the generated corpus (including shared-net
   extra edges, tie classes, and sequential gates), **When** the topology
   compile runs, **Then** emission order, per-slot levels, emitted count,
   and success/failure verdicts are bit-identical to the pre-change
   scan-based compile.
2. **Given** a net with a combinational cycle (including one that closes
   only through a shared-net extra attachment), **When** compiled or
   validated, **Then** the cycle fails loudly exactly as before — same
   verdict, no new vocabulary, no partial orders.
3. **Given** a net with a dangling primary or extra driver, **When**
   compiled under the combinational filter, **Then** the dangling defect
   fails loudly exactly as before.
4. **Given** the committed growth ladder, **When** the compile stage is
   re-measured at each size, **Then** the recorded cost grows with the
   edge count rather than the pool-size squared, with before/after
   numbers committed beside the baseline.
5. **Given** the same net compiled twice in a row (no mutation between),
   **When** the second compile runs, **Then** outputs are bit-identical —
   the cache adds no iteration-order variance.

---

### User Story 2 - Wire-time cycle probes follow the adjacency (Priority: P1)

A developer wires one more edge into a large net. The mutation seam's
incremental cycle check asks one reachability question — can the new edge's
sink reach the new edge's driver through combinational paths? Today,
answering that question costs roughly reachable-cone times pool-size,
because each step of the walk rescans every slot to discover successors.
After this feature, the probe follows the cached successor adjacency, so
its cost is proportional to the downstream cone actually reachable from
the sink — the honest authoring cost the 015 baseline set out to measure.
Accept and reject verdicts are identical for every case in the corpus:
flop-aware semantics (sequential gates break loops) are untouched, and a
cycle closing through an extra attachment is still caught.

**Why this priority**: The probe runs on every wire verb; per-wire cost
dominates the growth-wire stage at large sizes and directly gates the
million-gate authoring goal.

**Independent Test**: Can be fully tested by replaying the corpus of wire
sequences (legal wires, cycle-closing wires, flop-feedback wires,
extra-attachment cycles) before and after, asserting identical
accept/reject per wire, plus one growth-wire benchmark run showing
cone-proportional cost. Delivers value alone: wiring at scale.

**Acceptance Scenarios**:

1. **Given** the corpus of attempted wires (accepted and rejected alike,
   including edges into sequential gates, shared-net extras, and
   cycle-closing attempts), **When** each is attempted after the change,
   **Then** every verdict matches the pre-change result exactly.
2. **Given** feedback that passes through a flop (Q output breaks the
   loop), **When** wired, **Then** the wire is accepted exactly as before.
3. **Given** a wire that closes a pure combinational cycle, **When**
   attempted, **Then** it is refused loudly with the standing error, never
   silently accepted or partially applied.
4. **Given** the committed growth ladder, **When** the growth-wire stage
   is re-measured, **Then** per-wire cost tracks the downstream cone of
   the wired edge rather than total pool size, with before/after numbers
   committed.
5. **Given** a mutation sequence (add, wire, unwire, remove, re-wire)
   followed by a cycle probe, **When** the probe runs, **Then** it sees
   the adjacency as mutated — a stale answer served after any mutation
   is a defect caught by the pinned sequence tests.

---

### User Story 3 - Tie enumeration reads a cached member index (Priority: P2)

A developer ties, unties, inspects, or removes gates with tied input pins
on a large net. Today every one of those operations — member
enumeration, group-size queries, survivor collection on untie, class
re-star on tie/detach/reindex — scans the entire pool of slots and
inputs to find the affected class's members. After this feature, a
cached tie-class member index (each class leader's member pins, in the
standing slot-order enumeration) answers these questions by touching
only the affected class, and mutations maintain it at a cost proportional
to the pins they actually touch. The tie table's observable results are
identical: same star encodings, same leader election (lowest
slot/input), same idempotent re-tie, same two-member dissolve, same
verdicts.

**Why this priority**: Tie verbs and gate removal (which detaches slot
ties) are less frequent than wire/compile on the bulk authoring path,
but at scale each one still costs a full-pool scan, and the remove stage
of the baseline sweep is dominated by exactly these scans.

**Independent Test**: Can be fully tested by replaying a corpus of tie
sequences (tie merges across classes, leader removals with re-election,
non-leader untie, full-class dissolve via ignore, slot detach on
remove, input-drop compaction reindex) before and after, asserting
identical tie tables and verdicts after every step, plus a remove-stage
benchmark run. Delivers value alone: tie/untie/remove at scale.

**Acceptance Scenarios**:

1. **Given** the tie-operation corpus, **When** replayed after the
   change, **Then** the tie table after every step is identical to the
   pre-change result (star values, leaders, group counts, pair counts).
2. **Given** a tie merge of two classes on a large net, **When**
   performed, **Then** the merged class re-stars around the lowest
   member exactly as before.
3. **Given** removal of a class leader, **When** untied, **Then** the
   lowest surviving member is re-elected exactly as before, and the
   affected-class cost is proportional to class size, not pool size.
4. **Given** gate removal on a net whose removed slot holds tied pins,
   **When** the gate is removed, **Then** outside survivors re-elect
   exactly as before.
5. **Given** the committed growth ladder, **When** the remove stage is
   re-measured, **Then** its cost no longer grows with pool size per
   detach, with before/after numbers committed.

---

### User Story 4 - The cache lifecycle is contracted and measured (Priority: P2)

The caches are derived state: never serialized, never a second source of
truth, always rebuildable from the netlist alone. Their lifecycle follows
the standing discipline: mutations mark them stale, the next consumer
rebuilds or updates them before answering, undersized scratch refuses
loudly, growth is amortized through the pool workspace, and steady-state
consumption is allocation-free. One measured before/after report from the
committed baseline sweep (all stages, warmup separated, checksummed,
opaque inputs, per-size footprint) records the win on the targeted
stages and proves no other stage regressed beyond noise.

**Why this priority**: The invalidation contract is what makes the first
three stories trustworthy — a stale cache serving a wrong cycle verdict
would be worse than the quadratic scan it replaces — and the project's
performance rule requires the claim to be measured, never asserted.

**Independent Test**: Can be fully tested by a mutation interleaving
harness (mutate, query, mutate, query across add/wire/unwire/remove/
tie/untie/reset/load sequences) asserting every query matches the
scan-based reference, plus the committed before/after benchmark record.
Delivers value alone: the correctness contract and the evidence trail.

**Acceptance Scenarios**:

1. **Given** any sequence of mutations followed by any query (compile,
   validate, cycle probe, tie op), **When** the query runs, **Then** it
   reflects every preceding mutation — no stale answer anywhere in the
   interleaved corpus.
2. **Given** a cache consumer with undersized scratch, **When** work is
   requested, **Then** the refusal is loud through the standing failure
   vocabulary, never a partial or silent result.
3. **Given** a checkpoint save/load or YAML workspace round-trip,
   **When** the netlist is restored, **Then** serialized artifacts are
   byte-identical to pre-change (caches are derived and never
   serialized), and the restored netlist rebuilds its caches on first
   use.
4. **Given** the full baseline sweep re-run after the change, **When**
   stage costs are compared to the committed baseline, **Then** the
   targeted stages (growth-add, growth-wire, compile-rebuild, remove,
   optimize, dedup) show the flattened curve and no other stage
   regressed beyond measurement noise, with peak footprint per size
   reported.
5. **Given** repeated identical query sequences, **When** re-run, **Then**
   outputs are bit-identical (determinism contract intact).

---

### User Story 5 - Authoring verbs read maintained indexes, not the pool (Priority: P1)

A developer builds a large net one gate and one wire at a time, then
tears parts of it down. Today every single gate addition scans the
pool from slot zero for the lowest free slot, so merely constructing a
net of N gates costs pool-size-squared before a single wire — the
growth-add stage's dominant cost and the earliest blocker of every
scale goal. And every gate removal sweeps every live slot's inputs to
release the edges the removed gate drives, so teardown costs a full
pool scan per removed gate (the remove stage's cost, paid again per
dead gate inside optimizer passes). After this feature, slot
allocation consults a maintained free-slot index whose answers are
identical to today's lowest-free-slot rule (same slot for the same
operation sequence — the pinned allocation-order contract), and
removal releases the removed gate's driven edges by walking only the
successors it actually drives (the same adjacency User Story 1
builds). Counter conventions (never-reused per-kind counters, slot
recycling) are untouched.

**Why this priority**: The add scan is paid on every gate of every net
before any other cost — it dominates earliest and blocks the
million-gate construction goal outright; the remove sweep compounds
through the optimizer passes (dead-gate removal) and interactive
teardown.

**Independent Test**: Can be fully tested by replaying identical
operation sequences (mixed adds, removes, and re-adds that recycle
slots) before and after, asserting identical slot assignment at every
step, plus growth-add and remove stage benchmark runs. Delivers value
alone: construction and teardown at scale.

**Acceptance Scenarios**:

1. **Given** any add/remove/recycle operation sequence, **When**
   replayed after the change, **Then** every gate lands in the same
   slot as the pre-change implementation (lowest-free-slot rule
   preserved exactly, including recycled slots).
2. **Given** a large net and a gate removal, **When** the gate is
   removed, **Then** every input it drove (primary and extra
   attachments alike) is released with the same semantics as today
   (unwire promotion, polarity-aware fanout release, tie detach,
   shared-mark survival rules) at a cost proportional to the removed
   gate's fanout, not pool size.
3. **Given** a removal followed by a re-add into the recycled slot,
   **When** the re-add completes, **Then** no stale edge, mark, tie,
   or fanout count from the previous occupant survives (the existing
   slot-death invariants, now guaranteed by the index too).
4. **Given** the committed growth ladder, **When** growth-add and
   remove stages are re-measured, **Then** per-add and per-remove costs
   no longer grow with total pool size, with before/after numbers
   committed.

---

### User Story 6 - Optimizer pass scans are bounded by pass work (Priority: P2)

A developer runs the optimizer on a large net. Today several passes
hide full-pool scans inside per-gate work: the shared-feeder
predicate sweeps the whole pool once per queried gate (so root
collection alone is pool-size squared per pipeline run, and the dedup
pair loop stacks a third factor); the dedup pass compares every
gate pair (pool-size squared per sweep, re-run to fixpoint); every
rewire sweeps the whole pool for the users it moves; and each pass
re-derives whole-net state (fanout counts, sequential guards) from
scratch even when the mutation seam already maintains it. After this
feature, the shared-feeder predicate answers in constant or
fanout-proportional time from maintained state, rewires touch only
the replaced driver's recorded successors, the dedup sweep groups
candidates by structural key and compares within groups, and
whole-net re-derivations happen at most once per pass rather than per
gate. The optimizer's observable results are identical: same kept and
removed gates, same merges (higher slot into lower), same retags and
collapses, same final netlist (slots, kinds, wires, counters, ties,
marks) after each pass and at fixpoint.

**Why this priority**: The optimizer runs on demand rather than on
every authoring verb, but at scale each invocation costs
pool-size-squared or worse, and the dedup stage is an explicitly
measured all-pairs baseline line.

**Independent Test**: Can be fully tested by running the full
optimizer at every level over the generated corpus (combinational,
sequential, shared, pulled, tied nets) before and after, asserting
identical resulting netlists (slot-by-slot structural equality plus
counter and tie equality), plus optimize and dedup stage benchmark
runs. Delivers value alone: optimization at scale.

**Acceptance Scenarios**:

1. **Given** the generated corpus at every optimizer level,
   **When** each pipeline runs after the change, **Then** the
   resulting netlist is structurally identical to the pre-change
   result — same slots, kinds, drivers, extras, ties, marks, pulls,
   fanout counts, and per-kind counters.
2. **Given** a net with many structurally identical gates, **When**
   the dedup sweep runs, **Then** merge decisions and directions match
   the pre-change all-pairs result exactly (sinks preserved, higher
   slot merged into lower, sequential-feeders and shared feeders
   untouched) at a cost proportional to gate count plus within-group
   comparisons.
3. **Given** any pass that rewires a driver's users, **When** it
   runs, **Then** exactly the same inputs move to the same targets as
   today at fanout-proportional cost, with ignored-bit exposure on
   newly dangling inputs preserved.
4. **Given** dead-gate removal (DCE) on a large net, **When** it
   runs, **Then** removals produce the identical final netlist while
   per-removal cost stays proportional to the removed gate's fanout
   (no per-dead-gate full-pool sweeps).
5. **Given** the committed growth ladder, **When** optimize and dedup
   stages are re-measured, **Then** their cost no longer contains a
   pool-size-squared (or worse) term, with before/after numbers
   committed.
6. **Given** a sequential net, **When** optimized, **Then** the
   opaque-barrier rules hold exactly as today (no state element
   folded, rewired, or deleted; feeds-sequential gates untouched).

---

### Edge Cases

- What happens when a mutation despawns a slot that appears in the
  adjacency (as driver or sink) or in tie classes? Its edges/memberships
  must vanish from the caches; slot recycling must never resurrect a
  stale edge or membership.
- What happens when a driver is dangling (dead or out of range) at cache
  build or update time? The dangling verdicts must survive exactly —
  the cache must not launder a defect into a false pass.
- What happens when a cycle closes only through a shared-net extra
  attachment? The extra edges must be first-class adjacency entries
  (the 011 contract) so the probe and compile catch it exactly as
  before.
- What happens when both edge filters are needed (all-edges compile vs
  combinational-only ordering)? Each consumer must see its contracted
  edge set without duplicating storage or doubling maintenance cost
  beyond the pinned design.
- What happens when a single-edge mutation (one wire, one tie) occurs on
  a huge net? Cache maintenance must cost proportional to the mutation,
  never a full rebuild per single edge — otherwise the per-wire probe
  regression the baseline comment warns about simply moves.
- What happens when the pool grows mid-run? Workspace growth stays
  amortized and loud; the first query after growth may allocate (the
  standing workspace precedent), steady-state must not.
- What happens on an empty pool, a pool of one gate, or a two-member
  tie dissolve? All degenerate cases must answer identically to the
  scans they replace.
- What happens when the optimizer rewrites or removes gates (its own
  mutation path)? The caches must track those mutations too — the
  optimizer is a consumer and a mutator, never a cache-maintenance
  bypass.
- What happens when the free-slot index and the lowest-free-slot rule
  disagree (index stale after a removal or a load)? The index must
  answer identically to a fresh lowest-free scan; a divergent
  allocation order is a determinism defect pinned by replay tests.
- What happens when the shared-feeder predicate's maintained state and
  the mark tables disagree? The predicate must never clear a gate the
  marks still implicate (a wrong feeder verdict lets a pass fold
  through a shared span — the 011 freeze broken silently).
- What happens when the dedup key-bucketing collides (two gates with
  equal keys but different structure)? Collisions must resolve by
  exact structural compare within the bucket, never by hash equality
  alone, and candidate enumeration order must reproduce the all-pairs
  (ascending pair) visit order.
- What happens when passes mutate the net between cache reads (DCE
  removes, dedup merges, then a later pass queries)? The same
  staleness contract as the authoring verbs applies inside the
  pipeline: every pass reads fresh maintained state, and a fixpoint
  re-run produces the identical result as today.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The system MUST maintain a per-netlist cached successor
  adjacency covering every live driver edge (primary wires and
  shared-net extra attachments alike), derived from the netlist alone.
- **FR-002**: The topology compile (mutation rebuild, pure validation,
  and the stepper's combinational order) MUST consume the cached
  adjacency: per popped gate it touches only that gate's recorded
  successors, never the full slot pool.
- **FR-003**: Every observable compile output MUST be bit-identical to
  the pre-change scan-based core on the same netlist: emission order,
  per-slot levels, emitted count, live count, and all verdicts
  (combinational cycle, dangling driver, undersized scratch).
- **FR-004**: The wire-time incremental cycle probe MUST consume the
  cached adjacency so its cost is proportional to the combinational
  cone reachable from the probe's start, not to total pool size.
- **FR-005**: Every cycle-probe verdict MUST match the pre-change result
  exactly, including flop-aware acceptance (sequential gates break
  loops) and rejection of cycles closing through extra attachments.
- **FR-006**: Tie operations (member enumeration, group-size queries,
  untie survivor collection, tie/detach/reindex re-star) MUST read a
  cached per-class member index so per-operation cost is proportional
  to the affected class size, not pool size.
- **FR-007**: Every tie operation MUST leave the tie table and every
  verdict identical to the pre-change implementation (star encodings,
  lowest-member leader election, idempotent re-tie, two-member
  dissolve, validation refusals).
- **FR-008**: Cache maintenance on mutation MUST cost proportional to
  the mutation's own edge/member changes; a single-edge or single-pin
  mutation MUST NOT trigger a full-cache rebuild (full rebuilds are
  confined to bulk state replacement: reset, load, optimizer rewrite).
- **FR-009**: The caches MUST follow the standing invalidation
  discipline: every mutation path marks them stale, and the next
  consumer refreshes before answering; any query answered from stale
  cache state is a defect pinned by an interleaved mutation/query test.
- **FR-010**: Cache storage MUST live in the pool workspace following
  the existing growth/scratch conventions: undersized scratch refuses
  loudly through the standing failure vocabulary, growth is amortized,
  and steady-state consumption is allocation-free after warmup.
- **FR-011**: The caches MUST NOT be serialized: checkpoint and YAML
  artifacts stay byte-identical, and a restored netlist rebuilds its
  caches lazily on first use.
- **FR-012**: Enumeration order wherever the caches feed user-visible
  output (successor visits, tie member enumeration) MUST stay pinned to
  the standing slot-order convention so repeated runs are
  bit-identical.
- **FR-013**: Existing public entry points (topology compile,
  validation, reachability, tie verbs, mutation verbs) MUST keep their
  signatures and semantics; no consumer outside the logic domain
  changes.
- **FR-014**: The change MUST be measured: before/after numbers from the
  committed baseline sweep recorded for the targeted stages
  (growth-wire, compile-rebuild, remove) with warmup separated,
  checksummed outputs, opaque inputs, and per-size peak footprint, and
  no other stage regressed beyond measurement noise.
- **FR-015**: A parity test corpus MUST be added covering nets with
  shared-net extras, tie classes, sequential gates, dangling drivers,
  and deliberate cycles, asserting old-vs-new agreement for every
  affected operation (compiles, probes, tie operations, slot
  allocation replay, optimizer pipelines at every level), plus
  interleaved mutation/query sequences for invalidation pinning.
- **FR-016**: Documentation MUST ship with the change: the benchmark
  record updated with after-numbers, the logic domain guide updated
  where the cache contract changes documented behavior, and reference
  documentation for any new public helpers.
- **FR-017**: Gate addition MUST resolve its slot from a maintained
  free-slot index whose answer is identical to the lowest-free-slot
  rule for every operation sequence (including recycled slots); a
  divergent slot assignment is a determinism defect.
- **FR-018**: Gate removal MUST release the removed gate's driven
  edges (primary and extra) by consulting the successor adjacency at a
  cost proportional to the removed gate's fanout, with all standing
  release semantics preserved (unwire promotion, polarity-aware
  fanout release, tie detach, mark survival).
- **FR-019**: The shared-feeder predicate (does a gate drive any
  shared-marked input) MUST answer in constant or fanout-proportional
  time from maintained state, with verdicts identical to the
  full-pool scan on every net in the corpus.
- **FR-020**: Driver rewiring MUST touch only the replaced driver's
  recorded successors at fanout-proportional cost, producing identical
  wiring and ignored-bit exposure to the full-pool scan.
- **FR-021**: The structural dedup pass MUST replace the all-pairs
  compare with grouped candidate matching (structural key, exact
  within-group compare, collision-safe) whose merge decisions,
  directions, and final netlist are identical to the all-pairs result.
- **FR-022**: Whole-net re-derivations inside one optimizer pipeline
  invocation (fanout recounts, whole-net guard scans) MUST occur at
  most once per pass, never per gate or per pair, without trusting
  stale maintained state (re-derive once, then maintain).
- **FR-023**: The optimizer's observable results at every level MUST
  be identical to pre-change on the same netlist: kept/removed gates,
  merges, retags, collapses, final slots, kinds, drivers, extras,
  ties, marks, pulls, fanout counts, and per-kind counters, including
  the opaque-barrier and shared/pull freeze rules.

### Key Entities

- **Successor adjacency**: derived, per-netlist index mapping each live
  driver slot to the sink slots (and their input positions) it drives
  through primary wires and extra attachments; the one edge vocabulary
  shared by the topology compile and the cycle probe.
- **Tie-class member index**: derived, per-netlist index mapping each
  tie-class leader pin to its member pins in slot order; the readout
  behind tie enumeration, size queries, and re-star survivor
  collection.
- **Cache validity epoch**: the staleness state shared with the
  netlist's existing mutation-dirty discipline; a cache is either
  current or stale, and stale caches are refreshed before any consumer
  answers.
- **Free-slot allocation index**: derived, per-netlist index of
  recyclable slot positions answering "the lowest free slot" in
  constant or bounded time; maintained by gate addition and removal;
  never serialized (slot assignment is reproduced by operation replay
  and checkpoint load, not by storing the index).
- **Shared-feeder state**: maintained per-gate answer to "drives any
  shared-marked input", kept current by the marking/wiring/unwiring
  seam; the 011 driver-side freeze decisions consult it instead of
  scanning the pool.
- **Netlist pool workspace**: the existing pool-owned scratch home
  where the caches live, grow amortized, and refuse loudly when
  undersized.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: On the committed growth ladder, the per-mutation topology
  compile cost at the largest baseline size grows with the edge count
  rather than pool-size squared, with before/after numbers recorded in
  the benchmark record (compile-rebuild stage).
- **SC-002**: On the committed growth ladder, per-wire cycle-probe cost
  at the largest baseline size is proportional to the wired edge's
  downstream cone rather than total pool size, with before/after numbers
  recorded (growth-wire stage).
- **SC-003**: On the committed growth ladder, tie operations and
  slot-tie detach during gate removal at the largest baseline size cost
  proportional to the affected classes rather than pool size, with
  before/after numbers recorded (remove stage and tie corpus timings).
- **SC-004**: Every observable output is bit-identical to pre-change
  behavior: the full existing logic test suite passes unmodified, and
  the new parity corpus proves old-vs-new agreement on every affected
  operation (compiles, probes, tie operations, slot allocation,
  optimizer pipelines at every level).
- **SC-005**: No stage of the committed baseline sweep regresses beyond
  measurement noise, and peak memory footprint per size is reported
  with the cache cost accounted.
- **SC-006**: On the committed growth ladder, gate addition at the
  largest baseline size costs constant (amortized) time rather than a
  pool-size scan, and gate removal costs proportional to the removed
  gate's fanout — growth-add and remove stages show the flattened
  curve with before/after numbers recorded.
- **SC-007**: On the committed growth ladder, an optimizer pipeline
  run at the largest baseline size contains no pool-size-squared (or
  worse) term — optimize and dedup stages show the flattened curve
  with before/after numbers recorded, and no optimizer level's
  resulting netlist differs from pre-change on the corpus.
- **SC-008**: Every maintained index answers identically to the scan
  it replaces across the corpus and interleaved sequences: slot
  allocation, shared-feeder verdicts, rewire effects, and dedup
  merge decisions included.

## Assumptions

- The 015 scale-profiling baseline (bench_logic_scale, its committed
  numbers, and the growth-ladder conventions) exists and covers the
  targeted stages; the before-numbers cited by this spec come from that
  record.
- The caches are engine-internal derived state in the logic domain
  (header-only, following the existing workspace conventions); no CLI,
  GUI, TUI, wire-format, or serialization surface changes.
- Incremental maintenance is achievable within the existing mutation
  seam (all mutations already funnel through the netlist mutation
  vocabulary that marks state dirty); if planning finds a mutation path
  that bypasses the seam, that path is fixed to route through it rather
  than adding cache bookkeeping elsewhere.
- The exact cache representation (per-driver successor lists, per-leader
  member lists, or denser equivalents) is a planning decision; this
  spec contracts the cost bounds (mutation-proportional maintenance,
  class/cone-proportional queries, slot-order enumeration) rather than
  the data structure.
- Shared-net extra edges and tie memberships are bounded per pin by the
  existing caps (extras per input, inputs per gate), so per-entry cache
  costs are bounded by the same constants.
- Sequential gates remain opaque to the caches' consumers exactly as
  today: the caches store edges; the edge-filter semantics stay at
  their existing call sites.
- The appended hot paths (User Stories 5 and 6) were source-verified,
  not inferred: per-add lowest-free-slot scan in the mutation seam,
  per-remove full-pool sink sweep, per-gate shared-feeder full-pool
  predicate (called per gate by barrier-root collection, bypass, and
  per matching pair by the dedup loop), the all-pairs structural
  dedup, per-rewire full-pool user sweeps, per-removed-gate tie
  reindex/detach survivor scans, and whole-net recounts per pass. The
  exact maintained-state representation for each (free-slot cursor or
  recycle list, shared-feeder counter or adjacency check, dedup key
  buckets) is a planning decision; this spec contracts the cost
  bounds and the answer identity, never the structure.
- Paths investigated and deliberately ruled out of scope (linear-once
  or bounded, not per-operation scans): steady-state evaluation walks
  (inherently per-gate per-pass), per-step sequential phases,
  batch interface builds, schematic layout, the bounded exact
  pass (≤10 inputs / ≤16 outputs whole-net guards), delay-event
  scheduling (bounded pending list; its per-settle edge lists are
  existing adjacency-style precedent), reset/load wholesale rebuilds,
  and interactive name resolution. Fixing these without a measured
  hotspot is forbidden by the project's performance rule; they are
  named here so planning does not re-litigate the sweep.
- The dedup key's exact form (kind, input multiset, tie/merge mode)
  is pinned by the existing pass semantics; only the comparison
  strategy changes (grouped with exact within-group compare, collision
  never trusted).
