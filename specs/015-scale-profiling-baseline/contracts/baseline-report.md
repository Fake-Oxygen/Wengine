# Contract: The Committed Baseline Report — `docs/benchmarks/logic-scale.md`

The one documentation page that commits measured numbers, by explicit
spec mandate (FR-004/FR-005/FR-007) and the sanctioned carve-out of the
bench-docs "never commit machine-dependent numbers" habit (research
D7). Schema is fixed so follow-on tasks cite rows, not prose.

## Page structure

### 1. `## bench_logic_scale` — the CS3 catalog entry

The five mandated fields, exactly like every other bench entry:

- **Measured path** — the capacity-parameterized logic pipeline stages
  (growth, wire, compile, scalar solve, word eval, shared solve, step,
  optimize/dedup, remove, reset) at growing net sizes to ≥1M gates.
- **Why it matters** — the measured-hotspot gate: no SIMD/threading/
  GPU/structural-rework work on the logic domain proceeds without
  citing this page.
- **How to run** — sh block: bench preset build, the quick default
  invocation, and the committed full-ladder invocation (verbatim).
- **Interpretation** — per-stage line meanings, budget semantics,
  complexity verdicts from size-doubling ratios, and the fidelity
  note (u32 instantiation vs shipped u16 alias; alias anchor points).
- **Pitfalls** — opaque seeding (no constant folding), warmup
  separation, compare slopes/ratios not raw walls, foreign hosts
  record beside.

### 2. `### Baseline (pinned environment)` — the committed numbers

- **Environment block**: host class (WSL2 baseline), compiler
  (GCC 13.3), build (bench preset, echoed `-O3 -DNDEBUG` flags),
  date, free-memory budget observed.
- **Variance policy**: steady-state stages = median + min of ≥5
  repetitions; one-shot mutation stages = single measurement with
  observed ladder spread; reproduction claim = within stated
  variance on the pinned environment; budget-exceeded entries
  reproduce as budget-exceeded.
- **Stage tables** — one per stage; columns: size (gates),
  median, min, samples, peak footprint, budget state. A row may be
  `UNMEASURED-AT-SIZE (budget …)` — that loud gap is baseline
  evidence, never filled by estimation (FR-006).
- **Verdict table** — the spec FR-005 six suspects by name (Kahn
  per-pop full-pool sweep; per-add free-slot scan; per-remove
  full-pool sweep; per-wire reachability probe; all-pairs dedup;
  full-capacity init per solve/word/step): worst-case class,
  measured size-doubling ratios, verdict ∈ {confirmed-super-linear,
  cleared-within-variance}. Verdicts cite table rows only.
- **Ranking** — top costs at the 1M end, split mutation-side /
  compile-side / evaluation-side. This is the kill list's order.
- **Gate paragraph** (operational, near-verbatim):
  > From this baseline on, any vectorization, threading, GPU, or
  > structural-rework proposal for the logic domain must cite the
  measured number of the hotspot it attacks from this page, and
  must re-run this sweep as its before/after evidence. A proposal
  naming no measured hotspot is out of order until re-baselined.
- **Re-run procedure** — the verbatim committed invocation, the
  preset, and the expectation (within variance; unmeasured entries
  stay unmeasured).
- **Fidelity note** — the instantiation-width statement
  (contracts/engine-seam.md) in reader-facing words.
- **Beside-not-in-place rule** — other hosts/compilers re-run and
  record their numbers beside; the committed baseline is never
  edited to match a foreign environment.

### 3. Index integration

`docs/benchmarks/index.md`: Pages bullet + toctree entry for
`logic-scale`, plus the one-sentence carve-out in the intro
("committed numbers live only on the pinned-environment baseline
page, which states its own reproduction terms").

## Audit duties

- `python3 docs/audit_catalogs.py` (benchmarks + tests categories):
  zero missing, zero ghost — the `## bench_logic_scale` anchor must
  exist because the file stem exists.
- README lines per contracts/bench-invocation.md; Doxygen on the new
  engine forms ships in the same change.
