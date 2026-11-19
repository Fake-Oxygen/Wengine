# Contract: Documentation Build & Output

**Feature**: `specs/009-comprehensive-code-documentation/spec.md` | **Date**: 2026-11-19

How the HTML suite is produced, pinned, and kept out-of-tree (FR-011,
research R5).

## BC1. Prerequisites

| Tool | Version (pinned) | Source |
|------|------------------|--------|
| Doxygen | 1.9.8 host baseline (≥ 1.9.2 required by Breathe) | system package |
| Python | ≥ 3.9 (host: 3.12.3) | system |
| sphinx | 9.1.1 | `docs/requirements.txt` |
| myst-parser | 5.1.0 | `docs/requirements.txt` |
| breathe | 5.1.0 | `docs/requirements.txt` |
| furo | 2025.12.19 | `docs/requirements.txt` |
| sphinx-copybutton | 0.5.2 | `docs/requirements.txt` |

`pip install -r docs/requirements.txt` is the single setup step;
`docs/build.sh` checks every prerequisite and names what is missing.

## BC2. Build commands

```sh
docs/build.sh                  # doxygen → sphinx-build, with prereq checks
# manual equivalents:
doxygen docs/Doxyfile                        # XML → build/docs/doxygen-xml/
sphinx-build -b html docs build/docs/html   # site → build/docs/html/
```

The script fails loudly (non-zero exit, named missing dependency) rather
than producing a half-built site — mirrors the project's loud-failure
habit. No CMake target exists for docs; engine builds never require the
docs toolchain (research R5).

## BC3. Output contract

- HTML: `build/docs/html/` — entry point `index.html`.
- Doxygen XML: `build/docs/doxygen-xml/` (input for breathe, disposable).
- Nothing generated inside the source tree; `build/` is already
  gitignored — out-of-source discipline holds.
- Build artifacts are disposable: deleting `build/docs/` and re-running
  reproduces the site deterministically from pinned versions.

## BC4. Warning policy

- Doxygen warnings (undocumented entities, missing param docs) are the
  **coverage audit** input: delivery target is zero (SC-003).
- Sphinx build must complete without errors; warnings allowed only from
  a short recorded allow-list (e.g. breathe-internal quirks). "document
  isn't included in any toctree" is never allow-listed — that is an
  orphan page (NC2 violation).

## BC5. Regeneration after code changes (FR-011)

1. Re-run `docs/build.sh` — API pages re-render from the new comments.
2. If inventories changed (new test/benchmark/example): add the catalog
   entry, then re-run `docs/audit_catalogs.py`.
3. If behavior contradicts a page: fix the page toward actual behavior
   and record a finding if the behavior itself looks wrong (FR-010).

`docs/regeneration.md` on the site states this for site readers; this
contract is the executor's version of the same procedure.
