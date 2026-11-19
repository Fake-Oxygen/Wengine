# Regenerating the documentation suite

This site is generated, not hand-maintained as a snapshot. When the code
changes, refresh the affected parts with this procedure (FR-011). The
executor's version of the same procedure lives in the feature contract
`contracts/docs-build.md` (specs `009-comprehensive-code-documentation`).

## Rebuild the site

```sh
docs/build.sh
```

This runs Doxygen (`include/wengine/` → `build/docs/doxygen-xml/`) and
then Sphinx (`docs/` → `build/docs/html/`). It fails loudly on missing
prerequisites, on any Doxygen warning (broken comment blocks, misused
`@file`, unknown commands — every warning here is actionable), and on
any API coverage gap (see below). Nothing is generated inside the
source tree; deleting `build/docs/` and re-running reproduces the site
from the pinned toolchain (`docs/requirements.txt`).

## Refresh catalogs when inventories change

If tests, benchmarks, or examples were added, renamed, or removed, add
or update the corresponding catalog entry, then re-run the audit:

```sh
python3 docs/audit_catalogs.py
```

Delivery requires zero missing entries and zero ghost anchors: every
`add_test` registration, every `benchmarks/` file, and every
`examples/` file has a catalog entry, and no entry points at an
artifact that no longer exists. The audit compares against the live
registration, so it stays correct as the suite grows.

## Re-check API coverage after header changes

```sh
python3 docs/check_api_coverage.py
```

Delivery requires zero gaps: every public entity under
`include/wengine/` carries a `@brief`, every function parameter is
documented, and every file carries a `@file` role paragraph. New
public APIs must ship their Doxygen comments in the same change (the
comment conventions in `contracts/doxygen-comment-conventions.md` of the
`009-comprehensive-code-documentation` feature spec pin the grammar);
the audit fails the docs build until they do.

## If behavior contradicts a page

Fix the page toward actual behavior — the code is the truth, the page
is the claim. If the behavior itself looks wrong, record it as a
Finding (reported against the change, never fixed silently inside a
docs edit, and never documented-as-intended).

## Refresh rhythm

- Every code change touching public headers: rebuild + coverage audit.
- Every test/benchmark/example add or rename: catalog entry + audit.
- Before any release: the full accuracy pass — every documented
  command, example, and usage pattern executed verbatim.
