# Contract: Bench and Test Invocations — `bench_logic_scale` / `test_logic_scale`

Anchored in [research.md](../research.md) D5, D6, D8, D9, D11 and the
house bench conventions (`bench_signal_scale` idiom, `bench` preset,
catalog schema CS3).

## `bench_logic_scale` — argument contract

Positional, zero/clamp-fallback defaults, one line per
(scenario, stage, size) — the `bench_logic` / `bench_signal_scale`
grammar:

```text
bench_logic_scale [motif] [min_size] [max_size] [steps] [words] [budget_s] [reps]
```

- **motif** — 0 plain layered (default) | 1 wide | 2 deep-chain |
  3 mixed (shared nets + pulls + sequential + delay annotations,
  the 011–014 coverage motif).
- **min_size / max_size / steps** — geometric size ladder over the
  scale instantiation (default quick ladder tops well below 1M;
  the committed invocation's ladder is recorded verbatim in
  `docs/benchmarks/logic-scale.md` and includes ≥1,000,000).
- **words** — steady-state sample count for solve/word stages.
- **budget_s** — per-stage per-size wall budget; exceed ⇒ the stage
  reports **unmeasured-at-size** loudly and the sweep continues.
- **reps** — steady-state repetitions (≥5; median + min reported).

Construction is seeded from argc (xorshift, the house scheme):
identical arguments ⇒ bit-identical nets and checksums; timings
vary, shapes never do. Outputs are opaque to the compiler
(`do_not_optimize`, checksummed) — a folded-away stage is a harness
defect, not a fast measurement.

## Output grammar (stdout lines)

```text
bench_logic_scale <stage>: <size> gates x <n> ops, <median> ns/<unit> (min=<m>, fp=<footprint>, checksum=<c>)
bench_logic_scale <stage>: <size> gates UNMEASURED-AT-SIZE (budget <budget_s>s exceeded)
bench_logic_scale: setup failed (<reason>)        # exit 1
```

Stages (exactly the spec FR-003 set plus the FR-005 mutation homes):
`growth-add`, `growth-wire`, `compile-rebuild`, `solve`, `word`,
`shared-solve`, `step`, `optimize`, `dedup` (isolated pass),
`remove`, `reset`. Warmup is separated from every steady-state
timing; first-compile cost is never averaged into steady solve.

Exit codes: 0 success; 1 setup/run failure (house bench convention).

## The committed invocation

The full-ladder command (including the ≥1M point and the mixed-motif
reach: construct + solve + word + one budgeted step) is pinned
verbatim in `docs/benchmarks/logic-scale.md`'s Baseline section.
Re-running it on the pinned environment must land within the stated
variance (spec FR-004); budget-exceeded entries reproduce as
budget-exceeded. Default (no-args) invocation is the quick smoke
ladder — minutes, documented as such.

## `test_logic_scale` — ctest contract

- Registered `test_`-prefixed: `add_executable` +
  `target_link_libraries(... PRIVATE wengine_core)` + `add_test`;
  inherits the assertion policy automatically.
- Default size **65,536 gates** — past everything the u16 alias
  (1,024 slots / 8,192 flats) can address, proving the widened path
  carries more than the shipped instantiation ever could; suite cost
  a few seconds (research D9 arithmetic).
- Env override `WENGINE_LOGIC_SCALE_GATES=<n>` for manual larger
  runs (0/invalid ⇒ default).
- Asserts: alias identity pins (`static_assert`s: sizes, sentinels),
  scalar/word bit-exact parity at the scale instantiation,
  prefix-slice agreement, bit-identical repeat solves, loud-overflow
  and single-driver/cycle refusal parity at scale.
- Docs duty: CS1 six-field entry in `docs/tests/logic.md` +
  `docs/tests/index.md` domain-map row; README test-list line.

## Catalog duties (zero-missing / zero-ghost)

- `docs/benchmarks/logic-scale.md` gains the literal `##
  bench_logic_scale` anchor (CS3 five fields) — `docs/audit_catalogs.py
  benchmarks` must report N/N covered.
- `docs/tests/logic.md` gains the literal `## test_logic_scale`
  anchor — `docs/audit_catalogs.py tests` N/N covered.
- README: run list (L54–64 region), bench catalog paragraph
  (L839–855 region), benchmarks brace list (L992 region).
- Benchmarks never link `wengine_test_assert`; they run under the
  `bench` preset (Release `-O3 -DNDEBUG`, flags echoed at configure).
