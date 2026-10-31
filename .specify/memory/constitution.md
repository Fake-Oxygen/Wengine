<!-- Sync Impact Report
Version change: (none — template placeholders) → 1.0.0 (initial adoption)
Modified principles: none (initial adoption; 5 principles defined fresh)
Added sections:
- Core Principles I–V (correctness, determinism, independent-reference testing,
  measured performance, simplicity/scope/discipline)
- Additional Constraints (stack, architecture, I/O, units)
- Development Workflow (build/test gates, review, docs, task tracking, git)
- Governance (amendment procedure, versioning policy, compliance review)
Removed sections: none
Follow-up TODOs: none — all placeholders resolved from AGENTS.md/README.md.
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
inputs: fixed-step sequencing via `core::Runner` + `world::State`,
stable snapshot timing, stable signal serials and evaluation order
(slot order, generators then `kSum`), and versioned serialization with
fallback loaders. Convenience MUST NEVER override determinism or
correctness. CSV output MUST round-trip exactly (`max_digits10`); snapshot
CSV is output replay only, never a restart checkpoint (checkpoints are
versioned binary `save_state`/`load_state`).

### III. Independent-Reference Testing (NON-NEGOTIABLE)

Behavior MUST be verified at multiple boundaries where practical:
independent reference → solver result → CLI representation → serialized
artifact → re-read artifact. Tests are plain-`assert` binaries printing
`test_x: OK`, registered with `add_executable` + `target_link_libraries`
+ `add_test`, and every `test_*` target MUST link `wengine_test_assert`
so assertions run in Debug and optimized (`test-opt`) runs. New commands,
solvers, views, and registries MUST update their tests and every
registry-size `static_assert`. TTY behavior MUST be verified with a real
PTY, not piped `script(1)` alone.

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
signal leaf-only; cli → solvers/core; apps → cli+core) MUST NOT be
reversed without an explicit architectural decision. Engine modules MUST
remain header-only `INTERFACE` libraries except for concrete reasons
(e.g. platform-syscall TUs). Hot paths MUST use `core::Status`, never
exceptions; engine code MUST NOT perform application I/O (CLI owns
interaction, sinks own output).

## Additional Constraints

Technology stack: C++20, GCC 13.3 baseline, CMake ≥ 3.20, out-of-source
builds only, zero-warning target (`-Wall -Wextra -Wpedantic -Wconversion
-Wshadow -Wnon-virtual-dtor` on `wengine_cli`). Reproducible presets
(`dev` / `test-opt` / `bench`) MUST be preferred over hand-rolled flags;
benchmarks MUST NEVER link the assertion policy.

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
`ic`, `sim run` with defaults otherwise).

## Development Workflow

Build and test gates: out-of-source configure, build, then narrowest
relevant tests first and the full `ctest --output-on-failure` suite when
practical. The tree MUST NOT be left knowingly uncompilable or with
failing tests except for explicitly requested intermediate states.

Code review MUST verify constitution compliance, architectural
dependencies, deterministic-output impact, and test coverage at the
levels in Principle III. Complexity MUST be justified.

README is a living specification: user-visible changes MUST update Run
examples, the module table, and the test list.

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

Versioning policy (semantic): MAJOR for backward-incompatible
governance or principle removals/redefinitions; MINOR for new
principles or materially expanded guidance; PATCH for clarifications,
wording, or typo fixes without semantic change.

Compliance review: every change MUST be checked against the completion
checklist (behavior implemented, no unrelated files, architecture
preserved, determinism considered, tests pass, no new warnings, docs
updated, no unrequested git actions, uncertainty reported).

**Version**: 1.0.0 | **Ratified**: 2026-10-31 | **Last Amended**: 2026-10-31
