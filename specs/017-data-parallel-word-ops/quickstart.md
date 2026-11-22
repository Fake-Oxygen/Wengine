# Quickstart: Validating Data-Parallel Word Ops

**Date**: 2026-11-22 | **Spec**: [spec.md](spec.md) | **API**: [contracts/word-batch-api.md](contracts/word-batch-api.md) | **Parity**: [contracts/parity-matrix.md](contracts/parity-matrix.md)

## Prerequisites

- Baseline compiler (GCC 13.3), out-of-source builds only.
- Standard presets: `cmake -S . -B build -DBUILD_TESTS=ON
  -DBUILD_BENCHMARKS=ON` for tests; `cmake --preset bench` for
  measurement (Release `-O3 -DNDEBUG`, flags echoed at configure).

## 1. Parity (correctness — runs everywhere)

```sh
cmake -S . -B build -DBUILD_TESTS=ON -DBUILD_BENCHMARKS=ON
cmake --build build
ctest --test-dir build --output-on-failure
./build/test_logic_word_batch   # batch-vs-single on every slice + scalar agreement + refusals
```

Expected: `test_logic_word_batch: OK`, full suite green
(SC-001/SC-003). The new test covers the parity matrix corpus;
tail paths are exercised by nets whose slice counts are not
multiples of 4.

## 2. CLI end-to-end (truth bytes unchanged)

```sh
printf 'logic truth --help\nexit\n' | ./build/PhysicsSimApp
# compare truth output on a sample net before/after (see plan R6):
# bytes must be identical; only timing differs
```

Expected: truth tables byte-identical to pre-change output
(spec US3 acceptance scenario 5).

## 3. Measurement (the speedup claim)

```sh
cmake --preset bench
cmake --build build-bench
./build-bench/bench_logic [width depth words]   # word loop + batch section, opaque inputs, checksums
```

Expected: per-gate-word cost lower than the committed baseline by
the factor recorded in `docs/benchmarks/logic-scale.md`; no other
sweep stage regressed beyond stated noise (spec SC-002/SC-005).
Record environment (compiler, echoed flags, host), repetitions,
and variance beside the numbers — never edit the baseline in
place.

## 4. Auto-vectorization sanity (before claiming the win)

Inspect the generated code for the batch inner loop (e.g.
`objdump -d` / `-fopt-info-vec` on the `eval.hpp` instantiation)
and confirm packed 64-bit lane ops are emitted at `-O3`. If the
loop did not vectorize, the win rests on span-assembly
amortization alone — report which effect dominates rather than
assuming.

## References (no duplication)

- Sizing/validation rules: [contracts/word-batch-api.md](contracts/word-batch-api.md)
- Corpus axes and checks: [contracts/parity-matrix.md](contracts/parity-matrix.md)
- Design rationale (width, layout, guard placement): [research.md](research.md)
