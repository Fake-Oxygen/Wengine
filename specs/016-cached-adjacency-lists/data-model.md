# Data Model: Cached Adjacency Lists and Maintained Indexes (016)

Entities, fields, validation rules, and relationships derived from
[spec.md](spec.md) (FR-001..023, Key Entities) and [research.md](research.md)
(D1..D13). Field types are described semantically; exact C++ spellings
are implementation detail owned by the tasks phase. All cache entities
share one lifecycle contract (see Validity below and
[contracts/adjacency-contract.md](contracts/adjacency-contract.md)).

## Lifecycle (all derived entities)

```text
                mutation (seam event)
   CURRENT ────────────────────────────►  STALE
      ▲                                      │
      │  maintain incrementally (edges/pins/ │  rebuild lazily
      │  slots the event actually touched)   │  (bulk paths: load/
      └──────────────────────────────────────┘  reset/load-adjacent)
```

- **CURRENT**: every query answer equals the scan it replaced.
- **STALE**: the next consumer refreshes before answering (FR-009).
  Single-edge/pin mutations maintain in place (FR-008); wholesale
  replacements (reset, checkpoint load, optimizer whole-net builds)
  mark stale and let the first consumer rebuild once.
- A STALE answer served to any consumer is a defect class pinned by
  the interleaved-sequence parity tests (spec FR-015).

---

## Engine-side entities (all members of `NetlistT<kCap>`, never serialized)

### SuccessorAdjacency — the one edge vocabulary (FR-001..005, D2/D3)

Per-driver, slot-ordered intrusive edge lists over flat arrays.

- **head** — per-slot index of the driver's first edge (none sentinel
  otherwise).
- **edge pool** — dense recycled entries (`next`, `sink slot`, `sink
  input`, `extra flag`, `polarity`), one entry per live driver edge:
  every primary wire and every shared-net extra attachment (the 011
  rule: extras are driver-graph edges exactly like primaries).
- **free-edge list** — recycled edge indices; inserts pop, removals
  push (no tombstones).
- **Ordering invariant** — each driver's chain enumerates sinks in
  ascending slot order (the bit-identity requirement behind the Kahn
  drain, DFS probe, and rewire walks).
- **Content invariant** — the edge multiset equals the netlist's
  live primary+extra edges exactly (no stale edge after despawn/
  rewire; no duplicate; dead slots appear on no chain).
- **Maintenance ownership** — the mutation seam (wire, extra append/
  erase/promote, unwire, remove, rewire, optimizer surgery paths) is
  the sole writer, mirroring the `tie_rep`/`shared_meta` single-writer
  seams.
- **Consumers** — Kahn drain (FR-002), wire-time cycle probe (FR-004),
  gate-removal sink release (FR-018), driver rewiring (FR-020).

### TieMemberIndex — class readout (FR-006/007, D4)

Per-leader, flat-ordered intrusive member chains.

- **head** — per-flat (leader pin) index of the class's first member.
- **member chains** — dense entries (`next`, member flat) linked in
  ascending flat order; enumeration reproduces `tie_class_members`
  output exactly.
- **Content invariant** — the class partition equals `tie_rep`'s
  star encoding (leaders singleton-implicit; every non-singleton flat
  appears on exactly its leader's chain).
- **Maintenance ownership** — the TieSet seam (`tie_pins` merge-
  splice, `untie_pin` unlink + re-head on leader removal,
  `detach_slot_ties`, `reindex_kept_ties`, dissolve).
- **Consumers** — member enumeration, group sizes, survivor
  collection, re-star reads, optimizer detach/dissolve paths.

### FreeSlotIndex — lowest-free in O(log N) (FR-017, D5)

- **heap** — array-backed binary min-heap of dead slot indices;
  pop-min answers gate addition, push records gate removal, empty
  falls through to pool growth (the pinned allocation-order comment
  stays true verbatim).
- **Answer invariant** — pop-min == the linear scan's lowest-free
  answer for every reachable state; slot-assignment replay parity is
  the defect pin.
- **Rebuild rule** — load/reset heapify once (deterministic
  bottom-up over ascending dead indices).

### SharedFeederCounters — the 011 freeze predicate in O(1) (FR-019, D6)

- **counters** — per-slot number of that slot's live edges landing on
  shared-marked inputs.
- **Answer invariant** — `count > 0` iff the `drives_shared` scan
  returns true, across mark/wire orderings (mark-then-wire, wire-then-
  mark), promotions, extra erasure, and slot death.
- **Maintenance ownership** — the same seam events that maintain
  `output_fanout`/`nq_fanout` today (each counter-moving event is a
  seam event).
- **Consumers** — barrier-root collection, bypass skips, dedup
  freeze checks.

### DedupKeyBuckets — grouped candidate matching (FR-021, D7)

Transient per-sweep workspace (rebuilt each sweep; not persistent).

- **keys** — the pass's existing sorted-key multiset per candidate
  (semantics unchanged).
- **buckets** — workspace chains over a fixed deterministic 64-bit
  hash of (kind, input count, sorted keys, merge mode); ascending
  slot order within buckets.
- **Decision invariant** — in-bucket exact compare fires exactly the
  merges the all-pairs scan fires, in the same (s, t) ascending visit
  order; collisions never merge on hash equality alone.

### ValidityEpoch — the staleness flag family (FR-009/011, D1)

- **per-cache stale markers** — piggyback the netlist's existing
  mutation-dirty discipline; no new invalidation vocabulary (spec
  FR-009).
- **loaders** — checkpoint/YAML loaders mark every cache stale; the
  first consumer rebuilds once; caches never appear in any artifact
  (byte-identity of round-trips by construction).

### Workspace extension (D13)

- **ensure family** — each cache gains an `ensure_*` grower following
  `ensure_workspace` (grow-on-demand, never shrink, geometric);
  steady-state consumption allocation-free after warmup; undersized
  scratch refuses loudly via the standing `core::Status` vocabulary
  (FR-010).

---

## Test-side entities

### ParityCorpus (FR-015, D9) — `tests/logic_scan_reference.hpp` + `tests/test_logic_adjacency.cpp`

- **ReferenceOracle** — the pre-change scan implementations extracted
  verbatim (Kahn drain scan, DFS successor scan, tie scans,
  `drives_shared` scan, all-pairs dedup, lowest-free scan); the
  trusted oracle behind every comparison.
- **Corpus nets** — layered combinational growth; shared nets with
  extras + NQ polarity; tie classes across merges, leader
  re-elections, two-member dissolves; sequential nets with flop
  feedback; dangling drivers; deliberate cycles incl. extra-closing;
  mark-then-wire vs wire-then-mark orders.
- **Replay sequences** — interleaved mutation/query scripts (add/
  wire/unwire/remove/tie/untie/reset/load) asserting every query
  matches the oracle (the stale-answer defect pin).
- **Comparison levels** — full netlist state (slots, kinds, drivers,
  extras, ties, marks, pulls, fanout counters, per-kind counters),
  every verdict, emitted order/level arrays, slot assignment per add,
  per-sweep dedup state (not just fixpoint).

### Relationships

```text
Netlist ──owns──► SuccessorAdjacency ──answers──► Kahn drain / probe / remove / rewire
       ├─owns──► TieMemberIndex     ──answers──► tie verbs / optimizer detach+dissolve
       ├─owns──► FreeSlotIndex       ──answers──► add_gate
       ├─owns──► SharedFeederCounters ─answers─► barrier roots / bypass / dedup freezes
       └─owns──► ValidityEpoch       ──gates────► every consumer answers CURRENT-only
ParityCorpus ──compares──► (engine answers) vs ReferenceOracle
```

No entity is serialized; no entity is user-visible; no public
signature changes (FR-013). Serialization versions are untouched.
