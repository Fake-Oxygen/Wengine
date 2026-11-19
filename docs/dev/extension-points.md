# Extension points

Where a new capability plugs in: the file to touch first, the steps to
follow, and the tests and registries to update. Every entry below
grounds in one recipe; the recipes live here so contributors do not
have to rediscover them.

Related guides: [Architecture](architecture.md),
[Determinism](determinism.md), [Signal subsystem](../signals.md),
[Domain outputs](../domain-outputs.md).

## New closed-form solver

Pure, static calculation over SI quantities. Start here:
`include/wengine/solvers/<name>.hpp`.

Sequence:

1. Add the header with static pure functions, SI quantities across
   boundaries, and `constexpr`/`noexcept` where actually appropriate
   (never mechanically).
2. Quote the trusted reference (datasheet title, revision, section or
   equation numbers) in a comment where useful.
3. Add reference-anchored tests plus `static_assert` checks where the
   result is exactly representable.

Tests to update: a new `tests/test_<name>.cpp` (or an extension of the
matching suite) with solver goldens, registered with
`add_executable`/`add_test`.

## New stepped solver

A `core::ISolver` subclass that mutates `world::State` in place.
Start here: `include/wengine/solvers/<name>.hpp`, beside `projectile.hpp`.

Sequence:

1. Subclass `core::ISolver`: `name()` returns `kName`, `step()` mutates
   state, performs no I/O, and reports failures via `Status`.
2. Provide `static constexpr kName`, `static constexpr kDescription`,
   and `static create()` returning `unique_ptr<ISolver>`.
3. Validate `dt` on entry and skip detached or inactive slots.
4. Register the solver in `solver_registry.hpp` (one obvious entry) and
   maintain the registry-size `static_assert`.
5. If the solver carries state: pool it in `world::State` in SoA shape
   with `despawn_entity` detaching it, extend `Snapshot` and
   `take_snapshot` in slot order, bump the serialization version with
   a fallback loader for prior versions, add one `link::Frame` adapter
   plus one frame-source registry entry, and give `RunConfig` one
   nested section struct (never flat fields; `--gravity` is
   grandfathered).
6. Add tests against an independently derived closed form or reference
   model.

Tests to update: solver goldens, `test_solver_registry` (registry
size), serialization round-trip tests, and the frame-source tests.

## New CLI command

One command per solver domain: `sig` owns all signal vocabulary, `ic`
owns all IC vocabulary, and `sim run` takes no solver options beyond
the grandfathered `--gravity`. Start here:
`include/wengine/cli/commands.hpp` (the command specification).

Sequence:

1. Add the spec to `commands.hpp`: `kName`, `kDescription`, `kUsage`,
   `run`, and `complete`.
2. Register it in `command_registry.hpp` and maintain the
   registry-size `static_assert`.
3. Implement it in `src/cli/commands/<name>.cpp`: named `--key value`
   parameters parsed through `ArgCursor` and the existing `parse_*`
   helpers; bad arguments return usage errors (exit 2),
   runtime failures return exit 1, and stray positionals are errors,
   never silently ignored.
4. Cover `complete()` for modes, option names, and `--opt=` value forms.
5. Update `tests/test_cli_args.cpp` and check every other
   registry-size assertion.
6. Update README run examples, the module table, and the test list.

Tests to update: `test_cli_args`, plus the command's own run, usage,
stray-positional, and completion coverage.

## New IC-specific solver

Part-specific datasheet calculators (one real chip per solver) share
the single `ic` command so hundreds of parts never become hundreds of
commands. Start here: `include/wengine/solvers/ic/template.hpp` (copy
it, never include CLI types from a solver).

Sequence:

1. Add the solver at `include/wengine/solvers/ic/<chip>.hpp` with the
   datasheet reference block (title, revision, section/equation
   numbers) and reference-anchored tests.
2. Declare the CLI glue in `include/wengine/cli/ic/<chip>.hpp` (one
   spec: `kName` is the lowercase part number, plus `kDescription`,
   `kUsage`, `run`, and optional `complete`).
3. Implement it in `src/cli/commands/ic/<chip>.cpp`, parsing via
   `ArgCursor` plus `parse_*` helpers and reporting through
   `cli/status.hpp`. One chip may expose several subcommands
   (`ic <chip> <op>`, dispatched on its first positional like the
   `s0`/`i2c` modes); the top level never gains per-chip options.
4. Append the spec to `IcRegistry` in `ic_registry.hpp` (names stay
   unique via compile-time validation) and add the `.cpp` to the
   `wengine_cli` sources in `CMakeLists.txt`. Shared two-level
   plumbing lives in `ic_dispatch.hpp`; do not duplicate it per chip.
5. Add `tests/test_<chip>.cpp` (solver goldens plus CLI run, usage,
   stray, and completion coverage) or extend `tests/test_ic.cpp`,
   with `add_executable`/`add_test` entries.

Tests to update: the chip suite above; touch `test_cli_args` only if
top-level completion changes.

## New signal quantity

Quantities extend through one table, and compile-time checks pin the
pieces together. Start here: `include/wengine/signal/types.hpp`
(the `kQuantities` table).

Sequence:

1. Add the `Dimension` plus literal in `units/units.hpp` and one
   `scales_for` row in `units/scales.hpp`. Prove generic parsing with
   a test-only dimension so no other production change is needed for
   parsing.
2. Append one `kQuantities` row plus one `DimensionFor` line in
   `signal/types.hpp`, and extend the `StampedValue` variant plus
   `stamp()`. The `static_assert`s fail until all three agree.
3. Nothing else changes: pool, snapshot, serialization bound, `Frame`
   and CSV display, and quantity-aware parsing derive from the table.

Tests to update: quantity-table agreement tests, parse/display
round-trips for the new quantity, and the snapshot/serialization
tests that enumerate quantities. Full stamping design:
[Signal subsystem](../signals.md).

## New sink

An `emit::ISink` observer that reads snapshots and owns its output.
Start here: `include/wengine/emit/sink.hpp` (the contract).

Sequence:

1. Implement `on_snapshot`: copy or enqueue-owned before returning,
   never retain the reference, never reenter the driving Runner, and
   never throw into the loop. The snapshot reference is valid only
   for the duration of the call.
2. Keep the sink best-effort and non-throwing: a sink failure must not
   alter simulation state or timing. Make drops loud via counters,
   never silent.
3. Wire it at the app edge (caller-owned, passed to the driver or
   attached to the Runner); never add publishers to `RunConfig`.
4. For N:M routing, compose the existing edge wrappers in
   `emit/routing.hpp` (`SignalSubsetSink` for serial allowlists,
   `StrideSink` for step-aligned decimation) instead of adding
   filtering to the Runner.

Tests to update: sink unit tests (output exactness, replay
round-trip), sink-failure isolation tests, and, for transports, the
loopback/soak suites in `test_link_*`.

## New GUI panel or view

Engine logic first, rendering second, with manual windowed
verification as the project's method. Start here:
`apps/gui/models/` for the logic, `apps/gui/panels/` or
`apps/gui/views/` for the rendering.

Sequence:

1. Put engine-only logic (math, evaluation, adaptation) in `models/`
   or a `views/` math header with no ImGui includes, and add a unit
   test linking `wengine_core` alone.
2. Render in `panels/` (controls) or `views/` (canvas, viewport,
   graphs). Panels sharing one controller compose: each builds its
   candidate `RunConfig` from the live `sim.config()`, preserving
   sections it does not own, and either rebuilds live (few fields) or
   on explicit Apply (multi-field forms, no per-keystroke rebuilds
   while typing). Suffix every button and label with `##<panel>` so
   duplicate labels across panels sharing a window cannot collide.
3. For new solver data sources, add exactly one adapter function
   beside `link::Frame` plus one entry to the frame-source registry
   (`kFrameSources` in `apps/gui/views/frame_source.hpp`); views stay
   untouched and `Snapshot` never grows for GUI needs. Projection
   rules: [Domain outputs](../domain-outputs.md).
4. Maintain every registry-size `static_assert` (panel registry,
   frame-source registry).
5. Verify with a windowed run and screenshots; there is no
   input-automation tooling, so interaction feel and
   populated-state visuals need manual checks.

Tests to update: the `models/` unit test, controller parity tests
(bit parity with headless runs), and the frame-source tests for new
adapters.
