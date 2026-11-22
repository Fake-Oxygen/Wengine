# Contract: Batch-vs-Single Parity Matrix

**Date**: 2026-11-22 | **Spec**: [../spec.md](../spec.md)

The parity corpus proves FR-002/FR-012: every word the batched path
emits equals the single-slice path's word on the same slice, and both
agree with scalar `solve` row-by-row. Bit-exact `==` throughout — no
tolerances.

## Corpus axes (cross product where meaningful)

1. **Gate kinds**: all 8 combinational ops (`AND/NAND/OR/NOR/XOR/XNOR/NOT/BUF`).
2. **Fan-in**: 1 (NOT/BUF), 2..8 mixed within one net.
3. **Input counts**: 1..7 (partial-word masking live), 8 (exact
   multi-word), 9..12 (multi-batch + tail), plus one ≥31-input net
   (unbounded-slice rule, spot slices incl. large `word_base`).
4. **Dangling/ignored**: ignored dangling inputs (word 0 rule),
   pull-terminated dangling (up = all-ones, down = all-zero).
5. **Ties**: tied members sharing a leader column (leader ordinal
   for every member).
6. **Sequential fallthrough**: nets with DFF/DLATCH/JK/T/SRLATCH
   present — word outputs follow the standing zero/NQ convention.
7. **Refusal classes** (verdict parity, not words): shared nets with
   ≥2 attachments, live `kTbuf`, X/Z block inputs, undersized
   scratch, output-size mismatch, out-of-range slice — batched and
   single entries refuse identically.
8. **Degenerate**: empty netlist, single gate, no outputs.

## Checks per corpus net

- [ ] Full-slice sweep: `eval_block_words` batches + single-slice
  tail vs per-slice `eval_block_word` — every word `==`.
- [ ] Scalar agreement: `solve` per truth row vs word lanes —
  every row `==` (existing convention, extended to batch outputs).
- [ ] Repeat determinism: same batch twice — every word `==`.
- [ ] Portable-identity: build with batch disabled (single path
  only) vs batch build — truth output `==` (via corpus replay
  harness; no second binary required if the tail path covers it —
  planning note: assert at least `count % W != 0` nets so the tail
  path is exercised on every run).

## Evidence location

- New `tests/test_logic_word_batch.cpp` (`wengine_test_assert`
  linked); corpus nets built with runtime-seeded opaque construction
  (bench-rule: no constant-foldable builds).
- `docs/benchmarks/logic-scale.md` gains the before/after
  per-gate-word record per R7.
