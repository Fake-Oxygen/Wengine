# AGENTS.md — working agreement for AI sessions in WEngine

## 1. Project

WEngine ("Physics Simulation App") is a high-performance C++20 physics
simulation engine.

Long-term goals, in order:

1. Provide independent solvers for physical domains (circuits, meters,
   particles, rigid bodies, fields, and future domains).
2. Advance simulations deterministically through a fixed-step engine
   (`core::Runner` + `world::State`) and emit snapshots.
3. Allow external applications to consume simulation state, initially through
   file/CSV sinks and eventually through `wengine-link` shared memory and a GUI.

Current state:

* CLI (`PhysicsSimApp`) plus Dear ImGui GUI (`WEngineGui`, `BUILD_GUI=OFF`
  by default)
* REPL and headless execution
* Closed-form solvers: `ohm`, `s0`, `i2c`
* Stepped solver: `projectile`
* `sim run` command (`--emit-csv`, `--emit-shm`)
* Console and CSV sinks
* GUI: solver panels, stepped-sim control, graphs, 2D canvas, 3D viewport
* GUI engine linkage: in-process + SHM subscribe + CSV replay (source panel)
* `wengine-link` SHM bus: versioned seqlock snapshot bus with C++ publisher/
  reader endpoints, CSV↔bus replay bridge, sample + fast subscriber tools

### Project priorities

When requirements conflict, prioritize:

1. Physical and numerical correctness
2. Determinism and reproducibility
3. Existing documented API/behavior
4. Performance on measured hot paths
5. Simplicity and maintainability
6. Convenience

Do not trade determinism or correctness for convenience.

---

## 2. General agent behavior

### Inspect before modifying

Before making non-trivial changes:

1. Inspect the relevant implementation, tests, and callers.
2. Identify architectural constraints and dependencies.
3. Determine the smallest coherent change.
4. Implement it.
5. Run the relevant tests/build checks.
6. Report remaining uncertainty or unverified behavior.

Do not make speculative changes to unrelated code.

### Minimize scope

* Keep diffs focused on the requested task.
* Do not perform unrelated cleanup, formatting, renaming, or modernization.
* Do not introduce abstractions without a concrete need.
* Preserve existing behavior unless the task explicitly changes it.
* Do not rewrite working code merely because another style appears preferable.
* When a broad refactor is required, split it into independently buildable/testable steps.

### Existing code vs. this document

If existing code contradicts a rule in this document:

* Do not blindly rewrite it.
* Determine whether the contradiction is intentional, legacy behavior, or a defect.
* Preserve externally observable behavior unless the task requires changing it.
* Document significant architectural deviations when they remain.

---

## 3. Build, test, and run

Standard configuration:

```sh
cmake -S . -B build -DBUILD_TESTS=ON -DBUILD_BENCHMARKS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Headless smoke test:

```sh
./build/PhysicsSimApp sim run --steps 100 --particles 1
```

REPL smoke test:

```sh
printf 'ohm 12V 3A\nexit\n' | ./build/PhysicsSimApp
```

GUI build (off by default; needs system `libsdl2-dev` + `libvulkan-dev`):

```sh
cmake -S . -B build-gui -DBUILD_GUI=ON -DBUILD_TESTS=OFF
cmake --build build-gui --target WEngineGui
./build-gui/WEngineGui --help
```

Requirements:

* Out-of-source builds only; this is enforced by CMake.
* C++20.
* GCC 13.3 is the baseline compiler.
* `wengine_cli` uses:
  `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wnon-virtual-dtor`.
* Zero warnings is the target. Fix warnings rather than suppressing them.
* Tests are plain-`assert` binaries printing `test_x: OK`.
* Tests are registered with `add_executable`, `target_link_libraries`,
  and `add_test`.
* Do not introduce external test dependencies without an explicit reason.

After changes, run the narrowest relevant tests first, then the full test suite
when practical.

Never leave the tree knowingly uncompilable or with failing tests unless the
task explicitly requires an intermediate broken state.

---

## 4. Architecture

Directory structure:

```text
include/wengine/<module>/*.hpp   public engine API
include/wengine/link/*.hpp       wire protocol + shared Frame (solver-agnostic)
src/cli/*.cpp                    CLI implementation
src/link/*.cpp                   POSIX SHM endpoints (platform TUs, see below)
apps/cli/main.cpp                application entry point
apps/gui/main.cpp                GUI entry (thin assembly)
apps/gui/{backend,controllers,models,panels,views}/
                                 GUI layers (see GUI rules below)
examples/link_{sample,bridge,fastread}.cpp
                                 link tools (built with CLI)
tests/test_<domain>.cpp          domain tests
```

Dependency direction:

```text
solvers -> units, math
world   -> standalone state
emit    -> world
link    -> world (+ core/status vocabulary; endpoints attach as emit::ISink
           at the app edge, never inside the engine)
cli     -> solvers, core
core/solver.hpp -> world
apps    -> cli + core
apps/gui -> wengine_core + wengine_imgui + wengine_link_shm (SHM endpoints
            only) (never the wengine_cli library; header-only cli/args.hpp
            parsing may be included — it adds no link dependency and reads
            the same units tables)
views   -> link::Frame view-model only (never Snapshot internals;
            Frame is shared with the link wire encoder, see link/frame.hpp)
```

Do not reverse these dependencies without an explicit architectural decision.

### Engine modules

Engine modules are `INTERFACE` libraries and should remain header-only unless
there is a concrete reason to introduce translation units.

`src/` is currently for CLI and future platform-specific implementation code.

The established exception is platform syscalls: link SHM endpoints live in
`src/link/*.cpp` under the `wengine_link_shm` STATIC target while their
headers stay platform-free (a Windows port replaces the TU, never the header).

### I/O boundaries

Engine and solver code must not depend on `iostream` for application I/O.

* CLI owns user interaction.
* Sinks own output.
* Engine code exposes data/status rather than performing application I/O.
* `<ostream>` declarations may be used where required for unit/value formatting.

### GUI rules

`main.cpp` stays thin (arguments, backend lifetime, frame loop, window
layout). Feature code lives in its layer; new directories appear only with
the files that need them (no empty placeholder dirs).

* `backend/` owns SDL/Vulkan lifetime and frame render/present.
* `controllers/` own `Runner` + solver, rebuild from `RunConfig`, and
  re-attach non-owning sinks on every rebuild. External sources
  (`ShmSource`, `CsvReplay`) own no sim state: lag and failure stay on
  the display side by construction.
* `models/` holds engine-only logic (no ImGui): unit-tested linking
  `wengine_core` alone.
* `panels/` renders ImGui UI; verified by windowed runs/screenshots.
  Panels sharing a window share one ImGui ID scope (IDs default to widget
  labels): suffix every button/label with `##<panel>` so duplicate labels
  across panels cannot collide and misbehave.
* `views/` consume the `link::Frame` view-model only — never `Snapshot`
  internals and never wire structs. `link::Frame` (link/frame.hpp) is the
  single source of truth shared with the link wire encoder. New solvers
  add one adapter function (beside `link::Frame`) plus one entry to the
  frame-source registry; views stay untouched. Engine `Snapshot` must not
  grow for GUI needs.
* Third-party GUI headers are included as `SYSTEM` (warning-clean without
  touching upstream). `BUILD_GUI` defaults `OFF`; unit tests must build
  and run without GUI libraries (ImGui headers may be included for math
  checks, never linked).

### Error handling

Hot engine paths:

* no exceptions
* no unnecessary allocations
* no strings
* prefer `constexpr`
* prefer `noexcept` when actually guaranteed
* use `[[nodiscard]]` for values whose accidental discard is dangerous

Do not add `constexpr` or `noexcept` mechanically where they make the API
incorrect or unnecessarily restrictive.

Engine errors use `core::Status`.

CLI maps failures to:

```text
0  success
1  runtime/check failure
2  usage error
-1 internal exit signal
```

---

## 5. Performance

Performance claims must be based on measurement.

Do not optimize code solely because it "looks hot".

When optimizing:

1. Establish or inspect a benchmark.
2. Make the smallest change that addresses the measured cost.
3. Verify correctness.
4. Re-run the benchmark.
5. Check generated behavior when relevant.

### Hot paths

Where a path is actually performance-critical, avoid:

* dynamic allocation
* exceptions
* unnecessary string construction
* unnecessary virtual dispatch
* unnecessary synchronization

Do not sacrifice numerical correctness, determinism, or API clarity for
unmeasured performance.

Benchmarks must use opaque/non-constant-foldable inputs. A `constexpr` solver
called only with literals can be optimized away and therefore does not provide
a meaningful benchmark.

---

## 6. Physical quantities and units

Physical quantities crossing subsystem boundaries use:

```cpp
units::Value<Dim>
```

and use SI base units internally.

Raw `double` values crossing a boundary must have an explicit meaning,
preferably encoded by naming:

```text
_V
_A
_s
```

Do not introduce ad-hoc unit conversion tables in the CLI or individual
solvers.

GUI parses unit strings through the header-only `cli/args.hpp` forwarders
(no `wengine_cli` link); the suffix tables stay owned by `units/`.

### Single source of truth

`units/units.hpp` owns:

* `Dimension`
* `Value<Dim>`
* arithmetic
* exact comparisons where appropriate
* `ostream <<` representation
* literals

`units/scales.hpp` owns:

* suffix-to-scale mappings

This is the only place where unit-text knowledge belongs.

`cli/args.hpp` contains generic parsing machinery and thin forwarders. It must
not grow independent unit tables.

---

## 7. Numerical correctness and determinism

Determinism is a core project requirement.

Do not change:

* floating-point operation order
* iteration order
* accumulation order
* timestep sequencing
* RNG behavior
* solver update ordering
* snapshot timing
* serialization formatting

without considering the effect on deterministic output and golden tests.

When changing numerical code:

* derive expected behavior independently where practical;
* compare against hand-derived closed forms;
* distinguish closed-form error from accumulated integration error;
* use justified tolerances;
* do not widen tolerances merely to make tests pass.

Use `==` when values are expected to be exactly reproducible or have not passed
through inexact arithmetic.

Use `near()` with a justified epsilon for floating-point calculations where
exact equality is not appropriate.

Golden/reference values should come from an independent derivation or trusted
reference, not from the implementation under test.

---

## 8. State, snapshots, and sinks

`world::State` owns the live simulation state.

Stepped solvers mutate state in place according to the solver contract.

Snapshots are independent copies:

* never expose mutable live state to sinks;
* never give sinks references whose lifetime depends on mutable simulation state;
* snapshot timing must remain deterministic.

Sinks are best-effort, non-throwing observers.

A sink failure must not silently alter simulation state or simulation timing.

Whoever owns a run loop re-attaches its non-owning observers after every
rebuild; sinks themselves stay unaware of rebuilds.

Link transports (`wengine-link` SHM bus):

* publishers are `ISink`s owned at the app edge, never in `RunConfig`;
* the publish path performs no syscalls or I/O: adapt, encode, seqlock copy;
* failures are sticky `Status`, never throws; over-bound frames are dropped
  loudly and counted, never silently truncated;
* wire versions exact-match at attach with a loud found-version refusal;
  version 0 is reserved as the mid-construction marker, never assigned;
* the writer drops oldest on full; readers take latest only and never drain
  a backlog; receiver lag never alters sim state or timing;
* readers never unlink (the publisher owns the segment lifetime); a
  same-name publisher restart needs explicit re-attach (stale mapping).

---

## 9. Adding features

### New closed-form solver

1. Add:
   `include/wengine/solvers/<name>.hpp`
2. Prefer static, pure functions.
3. Use `constexpr`/`noexcept` when actually appropriate.
4. Use SI quantities for physical inputs/outputs.
5. Add reference-anchored tests.
6. Include a trusted reference value in comments where useful.
7. Add `static_assert` where the result is exactly representable.

Example existing solvers:

```text
ohm
s0
i2c
```

### New stepped solver

1. Subclass `core::ISolver`.
2. `name()` returns `kName`.
3. Mutate `world::State` according to the solver contract.
4. Skip detached/inactive slots.
5. Validate `dt`.
6. Provide:

   * `static constexpr kName`
   * `static constexpr kDescription`
   * `static create()` returning `unique_ptr<ISolver>`
7. Register it in `solver_registry.hpp`.
8. Add tests against an independently derived closed form or reference model.

Registration should remain a single obvious entry.

### New CLI command

1. Add the command specification to `commands.hpp`:
   `kName`, `kDescription`, `kUsage`, `run`, `complete`.
2. Register it in `command_registry.hpp`.
3. Implement it in:
   `src/cli/commands/<name>.cpp`.
4. Parameters are named:
   `--key value` or `--key=value`.
5. Parse through `ArgCursor` and existing `parse_*` helpers.
6. Bad arguments use `usage_error` and return code `2`.
7. Runtime/check failures return code `1`.
8. Stray positional arguments are errors; never silently ignore them.
9. `complete()` must cover modes, option names, and `--opt=` value forms.
10. Update `tests/test_cli_args.cpp`.
11. Maintain the registry-size `static_assert`.
12. Update README examples, module table, and test list.

Whenever adding a command, check all existing registry-size assertions.

### New unit

1. Add the literal to `units.hpp`.
2. Add exactly one corresponding scale entry to `units/scales.hpp`.
3. Do not add suffix knowledge to `cli/`.
4. Existing table-driven tests should cover parsing.
5. Add literal spot-checks.

### New GUI view or panel

1. Engine-only logic (math, evaluate, adaptation) goes in `models/` or a
   `views/` math header with no ImGui includes; add a unit test linking
   `wengine_core` alone.
2. Rendering goes in `panels/` or `views/`; verify with a windowed run and
   screenshots (no input-automation tooling exists — interaction feel and
   populated-state visuals need manual checks).
3. New solver data sources add exactly one adapter function (beside
   `link::Frame`) plus one entry to the frame-source registry; views stay
   untouched.
4. Maintain every registry-size `static_assert` (panel registry,
   frame-source registry, like the CLI ones).

---

## 10. Testing discipline

Test behavior at multiple boundaries where practical.

For numerical solvers:

```text
independent reference
        ↓
solver result
        ↓
CLI representation
        ↓
serialized artifact
        ↓
re-read artifact
```

This three-level or end-to-end verification has previously caught real bugs.

### Floating point

* Use justified `near()` tolerances.
* Do not use arbitrary large epsilons.
* Remember that repeated integration and closed-form evaluation have different
  error characteristics.
* For example, repeated `0.1` addition need not equal `10.0` exactly.

### TTY behavior

For line editing, raw mode, and completion behavior:

* verify with a real PTY;
* use Python `pty.fork` scripts under `/tmp/opencode` where appropriate;
* do not rely solely on piped `script(1)` tests.

---

## 11. Known lessons

These are established project constraints/lessons and should not be
re-investigated without a reason:

* Re-entering raw mode must use `TCSANOW`/`TCSADRAIN`, never `TCSAFLUSH`;
  flushing discards queued typeahead between successive reads.
* E12 nearest-value selection must consider the next decade's `1.0`; otherwise
  values such as `966Ω` incorrectly snap to `820Ω` instead of `1kΩ`.
* Semi-implicit Euler: constant-velocity axes remain exact; integrated axes
  accumulate error. Derive goldens per axis.
* `max_digits10` CSV output round-trips exactly. Assert parsed values with `==`
  where appropriate; compare formatted strings only for fixed-format details.
* Literal collisions such as `operator""_s` must remain inside
  `wengine::units::literals`. Never introduce `using namespace` for literals
  into headers.
* Frame-to-step folding: exact step counts at accumulator-cap edges are
  FP-fragile — assert bounded ranges there, or clamp play to the step
  budget and assert the exact park.
* The ImGui Vulkan backend owns the `VkSurface` (`DestroyWindow` destroys
  it); never destroy it separately. `SDL_Vulkan_LoadLibrary` must precede
  the instance-extension query (no lazy loading on that path).

---

## 12. Documentation

README is a living specification.

When adding or changing user-visible functionality:

* update Run examples;
* update the module table;
* update the test list;
* check the `tests/test_{...}.cpp` glob/documentation line.

Do not modify documentation merely to make unrelated wording changes while
working on another task.

---

## 13. Task tracking

Task tracking is handled through Plane.

For task-related work, load the `plane` skill.

Workspace:

```text
xxx
```

Project:

```text
Physics Simulation App - WEngine
PHYSI
```

Resolve the project by name and use its UUID.

Parent epics:

```text
scaffolding:
54df7a42-3e91-470e-a72f-0018c23b002b

wengine-link:
be6e606c-836e-4eb6-810e-fcb0e67b9dde

gui-integration:
0e4374ad-ab52-4404-b72a-76c7bc8eb4c3
```

New work should have a sub-task containing:

* what
* why
* acceptance criteria

Workflow:

```text
Backlog -> In Progress -> Done
```

Done state:

```text
9319ee4a-96ad-43db-b62a-abb9a0b9a076
```

If the API rejects the state UUID, re-list the available states rather than
guessing.

Progress notes belong in task comments, not description rewrites.

Only delete tasks when explicitly requested. Prefer `Cancelled`.

Do not create, modify, move, or close tasks unless the task-tracking operation
is part of the requested work.

---

## 14. Git discipline

Do not:

* commit
* push
* amend
* create branches
* create PRs

unless explicitly requested.

Keep the working tree buildable and tests passing after changes.

Do not discard or overwrite pre-existing user changes.

Before modifying a file with unrelated local changes, inspect the diff and
preserve those changes.

---

## 15. Completion checklist

Before considering a coding task complete:

* [ ] Requested behavior implemented.
* [ ] No unrelated files changed.
* [ ] Architecture/dependency rules preserved.
* [ ] Numerical/deterministic behavior considered where applicable.
* [ ] Relevant tests pass.
* [ ] Full test suite passes when practical.
* [ ] No new compiler warnings.
* [ ] Documentation updated when user-visible behavior changed.
* [ ] No commit/push/PR performed unless requested.
* [ ] Remaining uncertainty or unverified behavior explicitly reported.

