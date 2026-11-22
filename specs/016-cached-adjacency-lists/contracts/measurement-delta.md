# Contract: Measurement Delta Protocol (measurement-delta.md)

**Feature**: [spec.md](../spec.md) FR-014/022, SC-001..008 | **Research**: D10 | Baseline: [docs/benchmarks/logic-scale.md](../../../docs/benchmarks/logic-scale.md)

How the before/after claim is recorded so Constitution IV ("measured
performance only") is satisfied by evidence, not assertion. Mirrors the
015 baseline conventions (environment pin, variance policy,
UNMEASURED-AT-SIZE loudness).

## 1. Before numbers

The committed 015 baseline is the before-record: verdicts table + kill
list + ladder tables in `docs/benchmarks/logic-scale.md`. No re-run is
required for "before" — the baseline is pinned with its environment.

## 2. After run

2.1. One full `bench_logic_scale` ladder re-run on the after-feature
tree at the same sizes and motifs as the baseline tables, same preset
(`--preset bench`, `-O3 -DNDEBUG` flags echoed at configure), ≥5 reps on
steady-state stages, warmup separated, opaque inputs, checksummed
outputs, per-size peak footprint (`fp=` field).

2.2. Environment re-pinned (compiler version, host, load policy) in
the delta section header; deviations from the baseline environment are
declared, never hidden (baseline variance policy applies verbatim).

## 3. The delta record

3.1. A dated `### Delta 016 — cached adjacency & maintained indexes`
section appended to `docs/benchmarks/logic-scale.md` containing:

- per-stage ratio tables (before → after, per ladder size) for the six
  targeted stages: growth-add, growth-wire, compile-rebuild, remove,
  optimize, dedup;
- slope evidence: per-gate cost across the ladder showing the
  targeted stages no longer carry the measured growth ratios (e.g.
  growth-add's ≈3.9× per-gate per 4× n flattening toward constant);
- no-regression check: solve/word/shared-solve/step/reset rows within
  the baseline variance policy (SC-005);
- footprint delta including the new cache members (SC-005: reported
  and accounted);
- citations row-for-row to the baseline tables (the baseline honesty
  rule: verdicts cite measured ratios, never asymptotics alone).

3.2. UNMEASURED-AT-SIZE rows stay loud — an unmeasured stage at a size
is reported as such, never extrapolated.

## 4. Claims discipline

4.1. No performance claim in README/docs/tests cites an unmeasured
number; every claim cites a delta-table row.

4.2. The deferred rows (research D11/D12 — Kahn frequency,
full-capacity init) are re-ranked by the after-run's numbers; any
follow-on optimization proposal must cite the post-feature row it
attacks (the standing measured-hotspot gate).

4.3. The delta section is committed with the feature; re-running the
ladder is reproducible from the preset + the harness's documented
invocation grammar (baseline-report contract of 015).
