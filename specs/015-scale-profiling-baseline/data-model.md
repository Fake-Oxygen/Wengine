# Data Model: Scale-Profiling Baseline (015)

Entities, fields, validation rules, and relationships derived from
[spec.md](spec.md) FRs and [research.md](research.md) D1–D12. Field
types are described semantically; exact C++ spellings are
implementation detail owned by the tasks phase.

## Engine-side entities (the enablement substrate)

### Capacity instantiation — `NetlistT<kCap>` (and its siblings)

The templated form of today's fixed-capacity netlist. Siblings:
`PinRefT<kCap>` (slot reference), `SeqStateT<kCap>` (sequential pool
beside the netlist), capacity-typed `TieSnapshot`/`SolveDiag`.

- **kCap** — slot-pool capacity; bounds every pool-shaped member.
- **slot_t** — dependent slot/flat/level/fanout/count width
  (u16 while addressable, else u32 — research D2).
- **Members** — exactly today's fields, `kCap`-bounded:
  gates pool (per-kind counters, drivers, fanout counts, delay
  annotations), tie/leader tables, shared-net spans (mark + up to
  `kMaxSharedExtra` extras with polarity), pull configuration, and the
  derived block interface + topo cache (order/levels/size/dirty).
- **Aliases** — `Netlist`, `PinRef`, `kMaxGates = 1024` etc. pin the
  production instantiation; identity to today's types is enforced at
  compile time (size/sentinel `static_assert`s) and at runtime (the
  unchanged golden suite).

Validation rules (unchanged per FR-001/FR-009, at every capacity):
loud refusal past `kCap` ("netlist full"), single-driver rule with the
shared-mark opt-in, flop-aware combinational cycle rejection, X/Z
loudness at level consumers, per-kind counters monotonic and never
reused, zero-init invariant ("all-singleton state") for `kCap`-bounded
tables, tie/pull/shared disjointness per flat, and the
lowest-free-slot allocation policy (deterministic, capacity-blind).

### Scratch bundles (caller-owned, one per templated core)

Non-owning buffer sets the cores require; sized by `kCap`, owned at
the edge (research D3):

| Bundle | Required buffers | Size formula | Validation |
|---|---|---|---|
| Topo | indegree, queue, order, levels, seen | kCap; kCap; kCap; kCap; kCap | loud `Status` when too small, before any work |
| Eval (scalar) | resolved, gate_out | kCap×kMaxInputs; kCap | same |
| Word | input-position map, gate word scratch, block outputs | kCap×kMaxInputs; kCap; block_output_count | same (the `eval_block_word` precedent) |
| Step | resolved, comb order, gate_out, next_q, forced/force | kCap×kMaxInputs + kCap×4 | same |
| Optimize | pending wires, labels/roots/feeds/reach, emit map, **out-buffer netlist** (replaces by-value copies) | kCap×kMaxInputs (16 B each) + kCap×n + one netlist | same; out-buffer is caller-initialized fresh state |

Rule: scratch is a per-call non-owning view; the engine never
allocates; wrappers for the alias keep internal fixed stack scratch
and today's exact signatures (`noexcept` pinned).

## Harness-side entities (the measurement instrument)

### SweepSpec — one harness invocation

- **motif** — plain layered | wide | deep-chain | mixed
  (shared+pull+seq+delay; research D6).
- **size ladder** — ordered gate counts; must include alias anchor
  points (≤1024) and scale-instantiation points to ≥1M (FR-003's
  three-plus orders of magnitude).
- **stage set** — growth-add, growth-wire, compile-rebuild, scalar
  solve, word eval, shared solve, seq step, optimize (dedup isolated),
  remove burst, reset (FR-003 + FR-005 homes).
- **budgets** — per stage per size wall-time caps; exceeded →
  *unmeasured-at-size*, never a partial number (FR-006).
- **repetitions** — steady-state sample count R (≥5, median+min);
  one-shot stages fixed at 1 (research D8).
- **seed** — construction RNG seed (argc-derived, house idiom);
  identical spec ⇒ bit-identical nets and checksums (FR-003, edge
  case: shapes never vary, timings do).

### StageResult — one measured cell

Fields: motif, stage, size, samples, median ns (or ns/unit), min ns,
checksum, peak footprint, **budget state** ∈ {measured,
unmeasured-at-size}. Invariant: no extrapolated or estimated values
may exist in a StageResult (FR-006).

### Verdict — one suspect's measured truth

Fields: suspect (the spec FR-005 six, by name), worst-case class
(research table), measured size-doubling ratios (per stage/size),
verdict ∈ {confirmed-super-linear, cleared-within-variance}, evidence
= the StageResult rows it cites. Invariant: verdicts derive from
committed rows only; a suspect may be cleared (edge case honored).

### BaselineReport — the committed artifact

Fields: environment pin (host class, compiler, preset + echoed
flags, date), variance policy, per-stage result tables, verdict
table, 1M-end ranking (mutation/compile/eval-side split), the
measured-hotspot gate paragraph (operational FR-007 text), re-run
procedure (verbatim invocation), fidelity note (u32 instantiation vs
u16 alias, with alias anchor points). Invariant: reproduction within
stated variance on the pinned environment (FR-004); foreign hosts
record beside, never edit in place.

## Relationships

```text
SweepSpec ──runs──▶ NetlistT<kScaleCap> (via scratch bundles)
    │                        │
    ├─times──▶ StageResult ──cited-by──▶ Verdict ──ranked-in──▶ BaselineReport
    └─parity──▶ (scalar vs word vs prefix-slice references)     ▲
                                │                              │
tests (alias pins + scale parity) ──guards─── the whole chain ──┘
```

## State transitions

- **Budget state**: pending → measured | unmeasured-at-size (terminal
  per run; loud in output and report).
- **Compile cache** (`dirty`): mutation ⇒ dirty; next
  compile/validate drains it — timed as the *compile-rebuild* stage
  (forced dirty), never averaged into steady-state solve (FR-003
  warmup separation).
- **Per-kind counters / slot identity**: monotonic per pool; slots
  recycle — unchanged at every capacity (constitution stable-identity
  rule).
