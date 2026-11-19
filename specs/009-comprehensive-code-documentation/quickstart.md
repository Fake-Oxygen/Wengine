# Quickstart: Comprehensive Code Documentation

**Feature**: `specs/009-comprehensive-code-documentation/spec.md` | **Contracts**: [doxygen-comment-conventions.md](contracts/doxygen-comment-conventions.md) · [docs-build.md](contracts/docs-build.md) · [navigation-map.md](contracts/navigation-map.md) · [catalog-schemas.md](contracts/catalog-schemas.md) | **Date**: 2026-11-19

Runnable validation for the documentation suite end-to-end. Every
scenario states its expected outcome; together they discharge the spec's
success criteria. Implementation specifics (page contents, comment
rewording) belong to `tasks.md`/implementation — this guide validates
outcomes only.

## Prerequisites

```sh
# engine build (unchanged, standard tree)
cmake -S . -B build -DBUILD_TESTS=ON && cmake --build build

# docs toolchain
doxygen --version                       # ≥ 1.9.2 (host baseline 1.9.8)
pip install -r docs/requirements.txt    # pinned sphinx/myst/breathe/furo
```

## 1. Build the HTML suite (BC2, SC-002 backbone)

```sh
docs/build.sh
```

**Expected**: completes without errors; Doxygen XML lands in
`build/docs/doxygen-xml/`; `build/docs/html/index.html` opens in a
browser showing the five audience portals ([NC1](contracts/navigation-map.md)).
Nothing is generated inside the source tree.

## 2. Three-step navigation per audience (NC6, SC-008)

From `index.html`, without using search:

1. **User**: portal → outputs/CSV page → the emit command — instructions
   runnable as written.
2. **Developer**: portal → extension-points page → the new-solver entry
   path.
3. **Reviewer**: portal → tests domain page → `test_link_protocol` entry.

**Expected**: each answer reached in ≤ 3 clicks; sidebar shows the same
hierarchy everywhere.

## 3. Per-test entry completeness (CS1, SC-001)

Open any `docs/tests/` domain page and pick a section at random.

**Expected**: the entry names its exact CTest target and answers all
five required fields — what it checks, purpose, guards against, how to
run, prerequisites/boundaries. Pick `tests/assert_fail_probe`'s entry:
it must explain the assertion-policy property it protects (CS2).

## 4. Doxygen-convention rendering proof (DC1–DC4)

Edit one `@brief` line in a `/** ... */` block of any
`include/wengine/units/*.hpp` entity (a
temporary local change), then:

```sh
docs/build.sh
```

**Expected**: the module's API page in the rebuilt HTML shows the new
brief under that entity; params/`@return`/`@note` render as labeled
fields. Reverting the comment and rebuilding restores the original page —
comments are the only input to API rendering. Then verify comment-only
safety at scale:

```sh
ctest --test-dir build --output-on-failure   # run before the in-code
                                             # pass AND after it:
                                             # identical results (SC-007)
```

## 5. Catalog audit (CS5, SC-001/SC-004)

```sh
python3 docs/audit_catalogs.py
```

**Expected**: exit 0, reporting full coverage of the CTest inventory
(74 targets + support scripts today), all `benchmarks/` files, all
`examples/` files — zero missing entries, zero ghost anchors.

## 6. API coverage audit (SC-003)

Check the Doxygen warnings emitted by the build script (they stream to
the terminal during the doxygen step).

**Expected**: zero undocumented-entity warnings for
`include/wengine/**` — every public header has `@file` role text and
every public entity has brief/params/returns (the rendered module page
shows no bare signatures).

## 7. Corpus integration (FR-009, NC3/NC5)

Browse the sidebar.

**Expected**: `signals.md`, `tui.md`, `logic.md`, `delivery.md`,
`ros2.md`, `domain-outputs.md`, `zephyr-hil.md` each reachable from
their NC3 slot, unmodified; no new page restates their content — it
links. Landing page links README.md and GLOSSARY.md.

## 8. Search (the anti-hunting backstop)

Type a cross-cutting term (e.g. `seqlock`, `emit-csv`) into the search
box.

**Expected**: results span user, developer, and API pages; following the
top result lands on the relevant section anchor.

## 9. Accuracy pass sample (FR-013, SC-006)

Run three commands copied verbatim from the end-user guide.

**Expected**: each executes exactly as documented (exit codes and output
match the page). The full pass repeats this for every documented
command/example before delivery and must report zero broken
instructions.
