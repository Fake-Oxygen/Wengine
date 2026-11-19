# Core and world

The engine-foundation slice of the suite: the fixed-step `core::Runner`,
entity/component storage and snapshots in `world::State`, the CLI/GUI seam
(`RunConfig` + `run_headless`/`ConfiguredRun`), and the two
allocation-conscious companions (`fill_snapshot` reuse, particle identity).
These tests pin exact stepping, deterministic replay, failure latching, and
the registry contract everything else builds on.

## test_runner

**What it checks**: exact stepping (`run(10)` advances 10 steps and stamps
`sim_time`), shared-state visibility across solvers in registration order,
`emit_every` decimation (exact step indices, `0` disables, default emits
every step), the failure latch (a later solver's failure freezes
time/index, keeps the earlier solver's partial mutation, emits nothing, and
retries return the same failure without invoking solvers again until the
`Runner` is rebuilt), noise-RNG advancement under a failing expression
solver, non-finite `dt` rejection before any solver runs, clock-exhaustion
rejection (`step_index` overflow, non-finite next time, sub-resolution `dt`;
pure pre-checks, not latched), stop callbacks, error propagation
(`step_index`/`sim_time`/emission stop at the failed tick), accumulator
folding, `max_substeps` budgets, and bad-config rejection.

**Purpose**: the `Runner` is the single stepping contract every solver,
sink, and transport relies on; its clock, ordering, and failure semantics
must be exact and reproducible.

**Guards against**: timestep drift, out-of-order solver invocation, silent
emission on failed steps, retry re-execution after failure, and
non-finite/clock-overflow inputs reaching solvers.

**How to run**:

```sh
ctest --test-dir build -R runner
./build/test_runner
```

**Prerequisites**: standard build only. No PTY, ROS, or hardware needed.

**Boundaries**: does not cover solver numerics (see `test_sim`,
`test_determinism`), signal semantics (see `test_signal`), or transports
(see the link-transport page); failure recovery is rebuild-only by design.

## test_world

**What it checks**: entity registry slot reuse with bumped generations
(stale handles never alias), component opt-in attach/detach, snapshot slot
order with holes skipped, `fill_snapshot` value-identity with
`take_snapshot` for populated/empty states, retained owned copies surviving
live-State mutation and buffer reuse, serial stability across
despawn/respawn (`sig_serial` slot order, recycled slots get fresh
serials), `select_signals_into` capacity reuse and slot-order (not request
order), binary `save_state`/`load_state` round-trip including freelist
order, loud corruption handling (bad magic, bad version, truncation, empty
input clear the destination), and repeat-drive equality.

**Purpose**: `world::State` owns all live simulation state; its
spawn/despawn, snapshot, and serialization behavior must be exact so
sinks, checkpoints, and replay all observe the same truth.

**Guards against**: generation-aliasing bugs, snapshot/life-state
aliasing, serial reuse, serialization drift, and silent acceptance of
corrupt checkpoints.

**How to run**:

```sh
ctest --test-dir build -R world
./build/test_world
```

**Prerequisites**: standard build only.

**Boundaries**: does not cover stepping or emission (see `test_runner`),
signal evaluation (see `test_signal`), or the link wire encoding (see
`test_link_protocol`).

## test_sim

**What it checks**: the projectile semi-implicit Euler golden
(`x = 10`, `y = 5.04595`, `vy = 0.19` for the default 100-step drive,
hand-derived in the file), detached-slot skipping with frozen hole values,
non-finite `dt`/gravity rejection, the CLI three-level check
(solver → CLI text → CSV artifact with the exact header and slot-ordered
rows), solver selection (`--solver projectile|signal`, unknown names fail
with the registry listed), the full `sim run`/`sig run` usage-error matrix
(inf/nan options, stray positionals, orphans), SHM flag wiring with
`--emit-health` per-cause breakdown, per-output sampling
(`--emit-csv-stride`, `--emit-csv-select`, global/per-output intersection,
muted stride dropping loudly), UDP payload budgets, and ROS flag validation
with and without `WENGINE_HAS_RCLCPP` (rebuild hint when ROS is absent).

**Purpose**: proves the CLI batch path drives the engine faithfully:
goldens survive the solver → CLI → file pipeline byte-exactly, and every
flag combination either works or fails loudly.

**Guards against**: CLI/engine divergence, silent flag ignoring, golden
drift through the text/CSV layers, and unvalidated ROS/UDP options.

**How to run**:

```sh
ctest --test-dir build -R sim
./build/test_sim
```

**Prerequisites**: standard build; real loopback sockets for the UDP send
smoke (a closed port still sends the first datagram). No ROS install
needed — the ROS assertions degrade to the rebuild-hint path without it.

**Boundaries**: does not cover the TUI, paced wall-clock pacing (see
`test_paced`), or the SHM/socket wire formats themselves (see the
link-transport page).

## test_run_config

**What it checks**: the headless golden through `run_headless`
(`x = 10`, `y = 5.04595`, exact `vel_x`, default gravity), gravity flow
through `cfg.gravity`/`has_gravity` (`g = 1.62` golden `y = 0.99109`),
bad-config rejection (unknown/empty solver names, zero `dt`, zero steps,
gravity with a non-projectile solver, non-finite loop/plant values with
`out` untouched, inert zero-particle pass-through), `emit_every` through
the config, chunked `ConfiguredRun::run(37)+run(63)` bit-equality with a
one-shot batch for projectile and signal alike, and the explicit-Lua
requirement (an arbitrary extra solver never satisfies `kLua`).

**Purpose**: `RunConfig` is the shared CLI/GUI seam; frontends must be able
to build it, drive it headless or chunked, and get identical results.

**Guards against**: frontend/engine divergence, chunked-vs-oneshot drift,
and Lua scripts running without the real Lua runtime attached.

**How to run**:

```sh
ctest --test-dir build -R run_config
./build/test_run_config
```

**Prerequisites**: standard build only.

**Boundaries**: does not cover CLI parsing (see `test_sim`), Lua semantics
(see `test_lua`), or wall-clock pacing (see `test_paced`).

## test_determinism

**What it checks**: the fixed-`dt` golden with justified tolerances
(`O(n^2 * eps_machine)` drift ≈ 1e-11, so 1e-9 carries ~100x margin),
bit-identical repeat runs (full `State` and snapshot `==`, exact FP time
equality), decimated emission determinism (exact step indices), CSV
replay exactness (two identical runs produce byte-identical files;
`max_digits10` re-parses with `==`), and `dt` sensitivity (`dt = 0.005`
follows its own closed form and differs from the `dt = 0.01` golden by the
hand-derived integration-error gap `0.024525`, while repeating exactly).

**Purpose**: pins the project's core promise — identical inputs produce
identical outputs — and separates genuine determinism from expected
integration-error differences across `dt`.

**Guards against**: FP operation-order changes, iteration-order changes,
snapshot-timing drift, serialization formatting drift, and widened
tolerances hiding real divergence.

**How to run**:

```sh
ctest --test-dir build -R determinism
./build/test_determinism
```

**Prerequisites**: standard build only.

**Boundaries**: does not cover failure latching (see `test_runner`),
signal sources (see `test_signal`), or link transports.

## test_snapshot_reuse

**What it checks**: zero-allocation steady state by counting global
`operator new`: 1000 `fill_snapshot` calls on a fixed population allocate
nothing, a `take_snapshot` control proves the counter observes allocations
(non-vacuous), cold-fill growth is bounded (≤ 9 allocs), shrinking to zero
allocates nothing, a 200-step `Runner` fan-out to three pre-reserved sinks
allocates nothing with identical final `State` to a quiet run, retained
copies survive buffer reuse, and the subset wrapper's reused scratch keeps
steady traffic free with exact inner counters.

**Purpose**: the emit path must not allocate per tick at fixed population;
this is the performance contract behind the zero-alloc buffer-reuse
design.

**Guards against**: per-tick allocations in the snapshot/emission path,
growth of reused buffers at steady state, and retained-sink aliasing of
the reused buffer.

**How to run**:

```sh
ctest --test-dir build -R snapshot_reuse
./build/test_snapshot_reuse
```

**Prerequisites**: standard build only. Single-threaded counting; no
sanitizer or hardware needed.

**Boundaries**: does not cover threaded queues (see `test_output_queue`)
or numerical correctness of the snapshots themselves.

## test_particle_identity

**What it checks**: snapshots carry `(entity_index, entity_generation)`
identity vectors in dense slot order; `Frame` adapters preserve them
through SHM and compact round-trips with exact inverse equality; despawn
keeps survivor identity while dense positions shift; slot reuse yields a
new generation that never aliases the despawned one; checkpoints preserve
IDs and freelist order; legacy snapshots/frames without identity stay
unavailable (never fabricated from dense positions) through every codec;
and CSV round-trips preserve IDs while legacy 7-column logs parse as
unavailable.

**Purpose**: distinct simulated objects must stay distinct across despawn,
respawn, checkpoints, and every transport — dense slot positions alone
cannot identify an object.

**Guards against**: identity loss across despawn/recycle, fabricated
identity from slot positions, and codec paths dropping the identity
vectors.

**How to run**:

```sh
ctest --test-dir build -R particle_identity
./build/test_particle_identity
```

**Prerequisites**: standard build only.

**Boundaries**: does not cover signal serials (see `test_routing`) or
live-run determinism (see `test_determinism`).

## test_solver_registry

**What it checks**: compile-time registry facts (`find("projectile")`,
`find("signal")`, `find("logic")` non-null; `find("nope")` null;
`all().size() == 3`; non-empty descriptions; non-null `create`), plus the
factory-to-`Runner` path (registry `create()` drives 100 steps to
`step_index == 100` with constant-`vx` golden `x = 10`).

**Purpose**: the registry is the single obvious entry point for stepped
solvers; adding a solver without registering it (or breaking the catalog
path) must fail here first.

**Guards against**: unregistered solvers, registry-size drift, and
factory/runner wiring breaks.

**How to run**:

```sh
ctest --test-dir build -R solver_registry
./build/test_solver_registry
```

**Prerequisites**: standard build only.

**Boundaries**: does not cover CLI selection errors (see `test_sim`) or
individual solver numerics (see the solvers page).
