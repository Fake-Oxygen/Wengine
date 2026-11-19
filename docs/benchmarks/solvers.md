# Solver benchmarks

Closed-form and stepped solver costs, isolated from transport and capture.

## bench_ohm

**Measured path**: `OhmLawSolver::solve_from_ui` over a 1024-entry table of
runtime-generated U/I pairs (1–24 V, 0.1–5 A), one divide plus power term
per iteration.

**Why it matters**: cheapest closed-form solver; floors per-eval overhead and
validates the benchmark harness itself (opaque loop + sink).

**How to run**:

```sh
cmake --preset bench && cmake --build build-bench -j$(nproc)
./build-bench/bench_ohm 100000
```

Optional count argument is iterations (default 1,000,000).

**Interpretation**: `bench_ohm: <iters> iters, <x> ns/iter (sink=<s>)`;
compare `ns/iter` across commits. `sink` is the accumulated R+P checksum.

**Pitfalls**: constant folding defeated by `argc`-seeded xorshift input
vectors cycled with `n % kVals`, consumed through an `asm volatile`
`do_not_optimize(sink)` barrier. Replacing inputs with literals would fold
the whole loop away.

## bench_s0

**Measured path**: `S0PulseSolver::solve_full` (the heaviest S0 entry: ~10
flops plus an infinity branch) over runtime-generated power/time/impulse
triples.

**Why it matters**: bounds closed-form pulse-solver cost; exercises the only
branchy path in the S0 family.

**How to run**:

```sh
./build-bench/bench_s0 100000
```

Optional count argument is iterations (default 1,000,000).

**Interpretation**: `bench_s0: <iters> iters, <x> ns/iter (sink=<s>)`;
compare `ns/iter`. `sink` accumulates pulses + frequency + energy.

**Pitfalls**: same opaque machinery as `bench_ohm` (xorshift `power_w` /
`time_s` / `imp_per_kwh` tables, `do_not_optimize` sink). Literal inputs
would let the `constexpr` solver fold; impulse values cycle four constants
to keep the branch live.

## bench_i2c

**Measured path**: `solve_pullup` + `solve_series` per bus point, including
the data-dependent `e12_nearest` decade loops, over runtime Vdd/Cb/mode
triples.

**Why it matters**: only closed-form solver with input-dependent loop
bounds; measures realistic branchy E-series selection, not flat flops.

**How to run**:

```sh
./build-bench/bench_i2c 20000
```

Optional count argument is iterations (default 200,000).

**Interpretation**: `bench_i2c: <iters> iters, <x> ns/iter (sink=<s>)`;
expect ~2 orders above `bench_ohm`. `sink` mixes Rp, rise/fall estimates.

**Pitfalls**: opaque Vdd (1.8/3.3/5 V), capacitance (50–550 pF), and mode
tables keep decade-loop trip counts runtime-varying; `do_not_optimize`
holds the sink. Fixed literals would collapse the branch profile.

## bench_projectile

**Measured path**: `Runner` + `ProjectileSolver` over N live particles for M
fixed steps at dt=0.01: the full stepped-solver loop, no sinks attached.

**Why it matters**: isolates integration cost from snapshot/transport; the
reference stepped-solver rate every transport benchmark compares against.

**How to run**:

```sh
./build-bench/bench_projectile 256 100
```

Positional args are particles then steps (defaults 1024 x 500).

**Interpretation**: `bench_projectile: <p> particles x <m> steps,
<x> ns/particle-step (checksum=<c>)`; compare `ns/particle-step`.
`checksum` sums the final snapshot positions/velocities.

**Pitfalls**: initial conditions come from an `argc`-seeded xorshift PRNG so
the integration loop cannot fold; the snapshot checksum behind
`do_not_optimize` pins the run against dead-code elimination. Failure
prints `bench_projectile: run failed` and exits 1.
