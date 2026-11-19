# Benchmarks

The benchmark catalog: one entry per binary in `benchmarks/`, each naming
the measured path, why that path matters, how to run it, how to read its
output, and the pitfalls that make numbers lie. Measured numbers are
machine-dependent: use the interpretation fields to decide what to compare
(ns/iter across commits and configs), never treat absolute times as targets.

Build and run via the reproducible `bench` preset:

```sh
cmake --preset bench
cmake --build build-bench -j$(nproc)
./build-bench/bench_ohm 1000000
```

Every benchmark takes an optional count argument (iterations, steps, or
frames depending on the binary); the per-page entries document a
representative invocation each, all verified against the current tree.

Two rules hold for the whole suite. First, the constant-folding discipline:
a `constexpr` solver called only with literal arguments can be optimized
away before running and therefore measures nothing. Every benchmark defeats
this with opaque inputs — runtime-seeded xorshift vectors, `argc`-derived
seeds, or `volatile` guards — plus a checksum/`sink` accumulation behind an
`asm volatile` barrier so the timed loop cannot be discarded. When adding a
benchmark, copy that machinery; the per-page Pitfalls fields name the exact
mechanism each binary relies on. Second, the assertion policy: benchmarks
never link `wengine_test_assert`. Their `-O3 -DNDEBUG` behavior (GCC Release
defaults from the `bench` preset) is their own, so timing reflects shipped
optimization, not test instrumentation.

Pages:

- [Solver benchmarks](solvers.md): `bench_ohm`, `bench_s0`, `bench_i2c`,
  `bench_projectile`.
- [Transport and signal benchmarks](transport-signal.md): `bench_link`,
  `bench_link_sockets`, `bench_sample_block`, `bench_tcp_worker`,
  `bench_signal`, `bench_signal_scale`, `bench_snapshot`.
- [Logic benchmarks](logic.md): `bench_logic`.

```{toctree}
:maxdepth: 1

solvers
transport-signal
logic
```
