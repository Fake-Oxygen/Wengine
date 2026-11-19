<!-- Sync Impact Report
Version change: 1.0.0 → 1.1.0 (MINOR: materially expanded guidance; no
principle removed or redefined). This report replaces the stale
initial-adoption report that should have been removed before the
v1.0.0 commit.
Modified principles (titles unchanged):
- II. Determinism and Reproducibility: expanded with step-boundary
  control-plane drain (ControlBus, fixed channel order) and the
  stable-identity convention extended to logic per-kind counters.
- III. Independent-Reference Testing: expanded with front-end parity
  duty (TUI byte-identity), suite-runs-without-optional-deps rule,
  and live-HIL coverage for transport TUs that need real peers.
- V. Simplicity, Minimal Scope, and Dependency Discipline: dependency
  list extended (logic, ros2 edges; GUI never links wengine_cli);
  header-only exception generalized to all platform/library TUs
  (SHM syscalls, YAML store, raster backends, Lua runtime, ROS rmw).
Added sections:
- Delivery and link transport policy (Additional Constraints): three
  delivery classes, four clocks, sticky Status, loud counted drops,
  exact-match wire versions, readers never unlink, serial addressing,
  Frame as one lossy view projection beside typed output blocks.
- External bridge contracts (Additional Constraints): ROS2/Zephyr/
  UART/TCP/UDP bridges are edge-only ISinks with transport-free
  headers; under-budgeted links are visible, never silent.
- Logic domain contract (Additional Constraints): append-only GateKind,
  never-reused per-kind counters, single-driver rule, flop-aware cycle
  rejection, sequential gates opaque to the optimizer, YAML workspace
  loud refusal without fallback loaders.
- Documentation suite duties (Development Workflow): reproducible
  generated site, Doxygen comments ship with new public APIs, catalog
  audits at zero missing/zero ghost, TUI forms compose argv only.
Removed sections: none.
Follow-up TODOs: no constitution placeholders deferred. Observed
outside this update: the AGENTS.md "current state" list lags shipped
domains (logic, TUI, ROS2, Zephyr HIL absent; "control plane pending"
while SIG-10 landed) — refresh the runtime guide separately.
-->

# WEngine Constitution

## Core Principles

### I. Physical and Numerical Correctness (NON-NEGOTIABLE)

All solver and engine code MUST be physically and numerically correct
first. Floating-point operation, iteration, accumulation, timestep, RNG,
solver-update, and snapshot-timing order MUST NOT change without assessing
deterministic-output and golden-test impact. New solvers MUST ship with
tests against independently derived closed forms or trusted references;
expected values MUST come from independent derivation, never from the
implementation under test. `near()` tolerances MUST be justified per
case; tolerances MUST NOT be widened merely to make tests pass. Exact
`==` MUST be used where values are exactly reproducible.

### II. Determinism and Reproducibility (NON-NEGOTIABLE)

Simulation output MUST be deterministic and reproducible for identical
inputs: fixed-step sequencing via `core::Runner` + `world::State`, stable
snapshot timing, stable identities (signal serials and logic per-kind
counters are monotonic per pool and never reused; slots recycle, IDs do
not), and versioned serialization with fallback loaders for every prior
version. Signal evaluation order is pinned: slot order, generators then
`kSum`, snapshots at time `T` carry `value(T)`. Control input (SIG-10
verbs, ROS services, UART control lines) MUST enqueue and drain at the
step boundary via `solvers::ControlBus` in fixed channel order — control
NEVER mutates live state directly, so `dt`, ordering, and output are
untouched. Convenience MUST NEVER override determinism or correctness.
CSV and wire output MUST round-trip bit-exactly (`max_digits10`, `%.17g`);
snapshot CSV is output replay only, never a restart checkpoint (checkpoints
are versioned binary `save_state`/`load_state`).

### III. Independent-Reference Testing (NON-NEGOTIABLE)

Behavior MUST be verified at multiple boundaries where practical:
independent reference → solver result → CLI representation → serialized
artifact → re-read artifact. Tests are plain-`assert` binaries printing
`test_x: OK`, registered with `add_executable` + `target_link_libraries`
+ `add_test`, and every `test_*` target MUST link `wengine_test_assert`
so assertions run in Debug and optimized (`test-opt`) runs. The suite
MUST build and run without optional dependencies (GUI, ROS); transport
TUs that require live peers (rmw, on-hardware links) are verified by
live HIL, not ctest. Front ends MUST preserve engine-text parity: TUI
output is byte-identical to the REPL/headless CLI by construction and
pinned by full-registry parity tests; guided forms only compose argv
strings, and any form that reimplements parsing or validation is a bug.
New commands, solvers, views, and registries MUST update their tests
and every registry-size `static_assert`. TTY behavior MUST be verified
with a real PTY, not piped `script(1)` alone.

### IV. Measured Performance Only

Performance claims MUST be based on measurement, never on appearance.
Optimization REQUIRES an established or inspected benchmark, the smallest
change addressing the measured cost, correctness verification, and a
benchmark re-run. Hot engine paths MUST avoid dynamic allocation,
exceptions, unnecessary strings, unneeded virtual dispatch, and
unnecessary synchronization, but MUST NOT sacrifice numerical
correctness, determinism, or API clarity for unmeasured performance.
Benchmarks MUST use opaque, non-constant-foldable inputs.

### V. Simplicity, Minimal Scope, and Dependency Discipline

Changes MUST be the smallest coherent diff focused on the requested task;
unrelated cleanup, renaming, modernization, or new abstractions without
concrete need are FORBIDDEN. Existing documented API and behavior MUST be
preserved unless the task explicitly changes them. Dependency direction
(solvers → units/math; world standalone; emit → world; link → world;
signal leaf-only; logic → core/status, pure bits; cli → solvers/logic/
core; ros2 → link/world/emit, never the reverse; apps → cli+core) MUST
NOT be reversed without an explicit architectural decision. The GUI MUST
NEVER link `wengine_cli` (header-only `cli/args.hpp` inclusion is allowed
— it adds no link dependency). Engine modules MUST remain header-only
`INTERFACE` libraries except where a concrete platform or library
boundary demands a TU (POSIX SHM syscalls, YAML workspace store, raster
backends, the Lua runtime, ROS rmw) — headers stay platform/library-free
and the TU is replaceable, never the header. Hot paths MUST use
`core::Status`, never exceptions; engine code MUST NOT perform
application I/O (CLI owns interaction, sinks own output).

## Additional Constraints

Technology stack: C++20, GCC 13.3 baseline, CMake ≥ 3.20, out-of-source
builds only, zero-warning target (`-Wall -Wextra -Wpedantic -Wconversion
-Wshadow -Wnon-virtual-dtor` on `wengine_cli`). Reproducible presets
(`dev` / `test-opt` / `bench`) MUST be preferred over hand-rolled flags;
benchmarks MUST NEVER link the assertion policy. Vendored dependencies
live as pinned submodules (GLM, Dear ImGui, Lua, yaml-cpp); terminal,
display, and ROS libraries (ncurses, SDL2, the Vulkan loader, `rclcpp`)
are system packages, never vendored. `BUILD_TUI` defaults ON and the CLI
still builds without ncurses; `BUILD_GUI` and `BUILD_ROS2` default OFF
and fail fast only when enabled.

Physical quantities crossing subsystem boundaries MUST use
`units::Value<Dim>` with SI base units internally; raw `double`s MUST
carry explicit meaning (`_V`, `_A`, `_s`). `units/units.hpp` owns
`Dimension`, `Value`, arithmetic, and literals; `units/scales.hpp` owns
suffix-to-scale mappings; `cli/args.hpp` holds only generic parsing plus
thin forwarders and MUST NOT grow independent unit tables. Signal
quantities follow the single-table discipline (`signal/types.hpp`
`kQuantities` + `DimensionFor` + `StampedValue`).

Sinks are best-effort, non-throwing observers; sink failure MUST NOT
alter simulation state or timing. Snapshots MUST be independent copies,
never mutable live-state references. `RunConfig` stays generic (one
nested section struct per solver, never flat per-solver fields except
grandfathered `--gravity`); one CLI command per solver domain (`sig`,
`ic`, `logic`, `sim run` with defaults otherwise) — module sub-shells
MUST dispatch the same registry one-shot and interactively and MUST NOT
fork a second vocabulary.

Delivery: exactly three delivery classes exist (latest-state viewing,
ordered bounded sample streaming, complete recording) and MUST NEVER be
conflated — each owns its overflow policy, and complete recording is
driver-paced/backpressured within a budget or loud failure (faster
publishing alone is not a metric). Four independent clocks (simulation
`dt`, emitted sample rate, wall-clock packet rate, UI refresh rate) MUST
stay separate; waveform frequency is none of those. The publish path
MUST NOT block the simulation loop; worker-backed outputs MUST move
syscalls off it. Transport failures are sticky `core::Status`, never
throws; over-bound frames are dropped loudly and counted, never silently
truncated. Wire versions exact-match at attach with a loud found-version
refusal; version 0 is reserved. Readers never unlink; a same-name
publisher restart needs explicit re-attach. Stable serials address
signals everywhere (CSV `sig_id`, Frame channels, control plane, ROS
topics); dense slot order never substitutes for an ID. `link::Frame` is
one lossy view projection for displays; each domain owns its typed
output blocks (science traffic) and checkpoint columns (restart) beside
it — `Snapshot` MUST NOT grow for bridge or view needs; one adapter, not
fields.

External bridges (SHM, TCP/UDP, UART/Zephyr HIL, ROS2) attach at the
app edge as `emit::ISink`s, never inside `RunConfig`/`Runner`/`Snapshot`.
Bridge headers stay transport-free (vocabulary, mapping, config only);
the platform TU is replaced, never the header. UART wire framing
round-trips bit-exactly; an under-budgeted link is visible in counters
and health output, never silent.

Logic domain: `GateKind` is append-only; identity uses per-kind
monotonic never-reused counters (same convention as signal serials).
The netlist enforces the single-driver rule (re-wiring an occupied input
is a loud error, never a silent overwrite) and flop-aware cycle
rejection (flop outputs break loops; pure combinational cycles fail
loudly). Sequential gates are opaque barriers to every optimizer pass —
no fold-through, rewrite, or deletion of a state element; the optimizer
is pure, idempotent, and loud on overflow. The versioned YAML workspace
refuses retired generations loudly with no fallback loaders (explicit
decision, unlike world checkpoints).

## Development Workflow

Build and test gates: out-of-source configure, build, then narrowest
relevant tests first and the full `ctest --output-on-failure` suite when
practical. The tree MUST NOT be left knowingly uncompilable or with
failing tests except for explicitly requested intermediate states.

Code review MUST verify constitution compliance, architectural
dependencies, deterministic-output impact, and test coverage at the
levels in Principle III. Complexity MUST be justified.

README is a living specification: user-visible changes MUST update Run
examples, the module table, and the test list. The generated
documentation site (Sphinx + Doxygen, `docs/build.sh`) is reproducible
from the pinned toolchain: new public APIs MUST ship their Doxygen
comments (`@brief`, parameters, `@file` role paragraph) in the same
change; catalog changes (tests/benchmarks/examples) MUST keep
`docs/audit_catalogs.py` at zero missing entries and zero ghost anchors;
the docs build fails loudly on Doxygen warnings and API coverage gaps,
never passes with them.

Task tracking goes through Plane (`PHYSI` project); progress notes belong
in task comments. Git discipline: no commit, push, amend, branch, or PR
unless explicitly requested; never discard or overwrite pre-existing
user changes.

## Governance

This constitution supersedes all other practices for WEngine work. Where
`AGENTS.md` and this constitution conflict, this constitution governs;
`AGENTS.md` remains the runtime development guide.

Amendments REQUIRE documentation of the change, rationale, version bump
per the policy below, and a migration plan for affected code, tests, and
docs. The Sync Impact Report (HTML comment at file top) is scratch review
material and MUST be removed before committing the amended file.

Versioning policy (semantic): MAJOR for backward-incompatible governance
or principle removals/redefinitions; MINOR for new principles or
materially expanded guidance; PATCH for clarifications, wording, or typo
fixes without semantic change.

Compliance review: every change MUST be checked against the completion
checklist (behavior implemented, no unrelated files, architecture
preserved, determinism considered, tests pass, no new warnings, docs
updated, no unrequested git actions, uncertainty reported).

**Version**: 1.1.0 | **Ratified**: 2026-10-31 | **Last Amended**: 2026-11-19
