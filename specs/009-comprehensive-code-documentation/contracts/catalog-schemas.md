# Contract: Catalog Entry Schemas

**Feature**: `specs/009-comprehensive-code-documentation/spec.md` | **Date**: 2026-11-19

The required shape of every Test / Benchmark / Example entry (FR-004/
FR-005/FR-006). `docs/audit_catalogs.py` validates completeness against
these schemas; empty fields fail review.

## CS1. Test entry

Location: per-domain page under `docs/tests/`. Anchor **is** the CTest
target name.

```markdown
## test_<exact-target-name>

**What it checks**: the behaviors/assertion families the binary asserts
(quote-level specific: "serials never reuse across despawn/respawn",
not "signal logic").

**Purpose**: why those behaviors matter to the project.

**Guards against**: the regression class caught if it broke (determinism
break, serialization drift, silent route drop, model/engine divergence,
completion regression, ...).

**How to run**: exact commands against the standard build tree
(ctest -R name, or direct binary invocation with a representative
command line).

**Prerequisites**: standard build unless stated; real PTY / ROS 2
stack / embedded hardware when applicable, plus what happens when the
environment is absent (skip? loud failure?).

**Boundaries**: what the test deliberately does not cover (the known
untested neighbor classes).
```

## CS2. Test-support artifacts

`assert_fail_probe`, `assert_probe_check.sh`, `logic_pty_smoke.py`,
`tui_pty_smoke.py` get entries in the domain page of the suite they
support, same schema. Their identity anchor is the file name. Their
**Guards against** explains what test-fabric property they protect
(e.g. the assertion policy actually failing builds, PTY behavior that
piped `script(1)` cannot reproduce).

## CS3. Benchmark entry

Location: `docs/benchmarks/`. Anchor `## bench_<name>`.

| Field | Content |
|-------|---------|
| Measured path | what exactly is timed/measured (solver eval, seqlock copy, snapshot, ...) |
| Why it matters | which project principle/consumers care about that path |
| How to run | build + run commands (bench preset; never the assertion policy, per AGENTS §3) |
| Interpretation | how to read the output; which counters mean what |
| Pitfalls | the constant-folding trap and any benchmark-specific caveats (opaque inputs requirement, FR-005) |

## CS4. Example entry

Location: `docs/examples/`. Anchor `## <file-name>` (covers C++, Python
helper, and Zephyr sample files).

| Field | Content |
|-------|---------|
| Demonstrates | the capability the example shows |
| How to run | build + run commands incl. transport/endpoint prerequisites when the example talks to a live bus |
| Expected outcome | observable result to compare against |

## CS5. Audit semantics

`docs/audit_catalogs.py` (stdlib-only Python):

- Inventory sources: CTest target registration parsed from the build
  files; `benchmarks/*` and `examples/*` file listings.
- Catalog sources: `## test_*` / `## bench_*` / `## <file>` anchors in
  `docs/tests|benchmarks|examples/*.md`.
- Behavior: exits 0 listing "N/N covered"; exits non-zero listing every
  inventory item with no catalog anchor (and every anchor with no
  inventory item — renamed tests must not leave ghost entries).
- The audit is the SC-001/SC-004 gate: completeness "auditable by
  construction" means this script, run at delivery, reports zero gaps.
