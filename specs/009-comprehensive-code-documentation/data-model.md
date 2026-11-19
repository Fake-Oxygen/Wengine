# Data Model: Comprehensive Code Documentation

**Feature**: `specs/009-comprehensive-code-documentation/spec.md` | **Date**: 2026-11-19

No runtime entities. This feature's "data" is the documentation suite's
own structure: the site, its navigation, the catalog entries, the Doxygen
annotations, and the audit/regeneration machinery. Source files gain
comments only — no structs, no serialization, no state.

## Entity 1 — Documentation Site

The generated HTML deliverable (spec: Documentation Suite + Documentation
Index).

| Field | Value / rule |
|-------|--------------|
| Sources | `docs/**/*.md` (MyST) + Doxygen XML from `include/wengine/**` |
| Builders | `doxygen` (XML extraction) → `sphinx-build -b html` (breathe renders API pages) |
| Output | `build/docs/html/` — out-of-tree, gitignored, disposable |
| Search | Sphinx built-in full-text index (site sources only; README/GLOSSARY linked, not indexed — research R3) |
| Theme | furo, two-level global toctree, per-page outline |

**Validation**: build completes warning-clean apart from a recorded
allow-list; every page reachable from `index.md` within 3 navigation steps
per audience (SC-008).

**Relationships**: built by the pipeline in Entity 7; composed of Entities
2–6.

## Entity 2 — Audience Portal (navigation)

One labeled entry group on the landing page per audience.

| Field | Value / rule |
|-------|--------------|
| Portals | Using WEngine (end user) · Developing WEngine (developer) · API Reference (embedder) · Quality — Tests & Benchmarks (reviewer) · Examples |
| Entry page | `user/getting-started.md`, `dev/architecture.md`, `api/index.md`, `tests/index.md` + `benchmarks/index.md`, `examples/index.md` |
| Existing corpus | the 7 topic guides join the toctree at their audience-appropriate slot (e.g. `signals.md` under developer, `tui.md` under user) |
| Top-level rule | a page belongs to exactly one toctree slot; orphans are build warnings treated as failures |

**Validation**: no orphan pages; no duplicate canonical content — existing
topic guides are linked once, never restated (FR-009).

**Relationships**: portals are the shape of the index contract
(`contracts/navigation-map.md`).

## Entity 3 — Test Catalog Entry

One `## test_<name>` section (or support-script entry) per test artifact
in a per-domain page under `docs/tests/`.

| Field | Required content |
|-------|------------------|
| `test_<target>` | exact CTest target name (anchor identity) |
| What it checks | the behaviors/assertion families the binary actually asserts |
| Purpose | why those behaviors matter to the project |
| Guards against | the regression class caught if this test broke (determinism break, serialization drift, silent route drop, model/engine divergence, ...) |
| How to run | exact command(s), incl. via the standard build tree |
| Prerequisites | special environments (real PTY, ROS 2 stack, embedded hardware) and what happens without them |
| Boundaries | what the test deliberately does not cover (known-untested neighbor classes) |

**Validation**: target exists in the CTest inventory; all fields
non-empty; unique anchor per page; completeness audited by
`docs/audit_catalogs.py` (SC-001 — inventory 74 targets + support scripts
today, matched at delivery time).

**Relationships**: audited by Entity 8; schema pinned in
`contracts/catalog-schemas.md`; referenced (deep-linked) from dev-guide
pages where a subsystem's protection is discussed.

## Entity 4 — Benchmark / Example Catalog Entries

Same per-artifact section shape as Entity 3, under `docs/benchmarks/` and
`docs/examples/`.

| Field | Benchmark | Example |
|-------|-----------|---------|
| Identity | `bench_<name>` source file | example file (incl. Python helpers, Zephyr sample) |
| What | measured path + why that path matters | demonstrated capability |
| Run | build + run commands | build + run commands |
| Interpretation | how to read output; pitfalls (opaque-input/constant-folding discipline, FR-005) | expected observable outcome |

**Validation**: every file in `benchmarks/` and `examples/` has an entry
(SC-004, audited); every documented run command executes as written
(FR-013).

## Entity 5 — API Module Page

One page per engine module directory under `docs/api/` (12 today:
units, core, world, physics, solvers, signal, emit, link, logic, ros2,
tui, cli).

| Field | Content |
|-------|---------|
| Prose | module responsibility, collaborators, dependency rules it obeys (links into dev guide for the full map) |
| Usage pattern | one minimal correct consumer snippet (compile-grade, per FR-003) |
| Rendered API | breathe directives for the module's key entities |
| Completeness listing | auto-generated index of **all** the module's Doxygen entities — nothing undocumented can hide |
| Coverage state | module's headers contribute zero entries to the Doxygen undocumented-warnings list |

**Validation**: `WARN_IF_UNDOCUMENTED` + `WARN_NO_PARAMDOC` reach zero for
`include/wengine/**` (SC-003); usage patterns verified to work (FR-013).

## Entity 6 — Doxygen Contract Comment

The in-source annotation, per `contracts/doxygen-comment-conventions.md`.

| Site | Required |
|------|----------|
| File header | `/** @file */` block + one-paragraph role statement (129 headers) |
| Public type/function | `@brief` lead line; `@param[in/out] name` + description; `@return` naming status codes on failure paths; `@note` for units, ownership, determinism obligations |
| Non-obvious logic (any file) | site comment explaining *why*: FP order, seqlock reads, raw-mode re-entry, protocol framing (FR-008) |

**Validation**: comments only — pre/post `ctest` parity and zero-warning
build gate (SC-007); rendering verified in the built site.

**Relationships**: extracted by Entity 7; rendered by Entity 5.

## Entity 7 — Build & Regeneration Pipeline

| Step | Command / rule |
|------|----------------|
| Prereq check | doxygen present; `pip install -r docs/requirements.txt` satisfied |
| Extract | `doxygen docs/Doxyfile` → `build/docs/doxygen-xml/` |
| Render | `sphinx-build -b html docs build/docs/html` |
| Refresh after code change | re-run pipeline; update the affected catalog pages when inventories change; re-run audits (documented in `docs/regeneration.md`, FR-011) |

**Validation**: runnable from a clean checkout with only documented
prerequisites (quickstart scenario 1).

## Entity 8 — Audit & Findings

| Item | Rule |
|------|------|
| `docs/audit_catalogs.py` | inventory ↔ catalog cross-check (tests, benchmarks, examples); exits non-zero listing every gap |
| Doxygen warnings | undocumented public entities — must reach zero |
| Finding | doc/behavior contradiction or suspected defect spotted while documenting: recorded and reported, never "fixed" in-pass and never documented-as-intended (FR-010) |

**Validation**: audits pass with zero gaps at delivery (SC-001/SC-003/
SC-004); findings list delivered with the task report.
