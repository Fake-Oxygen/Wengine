# Research: Comprehensive Code Documentation

**Feature**: `specs/009-comprehensive-code-documentation/spec.md` | **Date**: 2026-11-19

All unknowns resolved — no NEEDS CLARIFICATION remains. The planning-phase
user input pins the toolchain: Sphinx + MyST + Doxygen, HTML output,
logically indexed. Research below resolves every remaining integration
unknown against the host's actual state (Sphinx 9.1.1, MyST-Parser 5.1.0,
Doxygen 1.9.8 installed; Breathe/furo not yet installed) and current
upstream release data (verified 2026-11-19).

## R1. Doxygen→Sphinx bridge: Breathe with curated per-module pages

**Decision**: Breathe 5.1.0 with hand-curated, per-module API pages under
`docs/api/`. Each page carries module prose, one minimal usage pattern, and
breathe directives for the module's key entities, plus a per-module
auto-generated entity index (`autodoxygenindex` scoped by directives) so
**every** documented entity of the module is reachable even when not
individually curated — logical navigation *and* completeness coverage from
one page shape.

**Rationale**: Breathe 5.1.0 (PyPI, released 2026-10-02) is actively
maintained by Sphinx-core contributors and explicitly requires "Python 3.9+,
Sphinx 7.2+, and Doxygen 1.9.2+" — the host's Sphinx 9.1.1 and Doxygen
1.9.8 both qualify. Breathe directives are ordinary Sphinx directives and
therefore usable from MyST sources (`{doxygenclass}` code fences), keeping
the whole site in Markdown per the user's MyST requirement.

**Alternatives considered**:
- **Exhale 0.3.7** (auto-generated full class/file/namespace hierarchies on
  top of Breathe) — rejected: upstream is Alpha-status with its last
  release 2024-01-21 and its compatibility table never validated against
  Sphinx 9; its own README warns it is "known to work well on small to
  medium sized projects" and generates "a large number of .rst files".
  A stale bridge under a nine-month-old Sphinx is a build breakage risk,
  and its sprawling auto-tree is exactly the "hunt for docs" navigation
  the user rejected.
- **Breathe `autodoxygenindex` alone** (one flat listing per project) —
  rejected as the primary shape: complete but unreadable; adopted instead
  as the per-module completeness listing inside each curated page.
- **Doxygen's own HTML output, linked side-by-side with Sphinx** —
  rejected: two visual systems, two search boxes, inconsistent indexing;
  violates the single-indexed-site requirement.

## R2. Theme and site navigation: furo

**Decision**: furo (pin 2025.12.19) as `html_theme`, Sphinx's built-in
full-text search, and a two-level global toctree organized by audience
portal (see R6/navigation contract). Light/dark modes, sidebar with the
full hierarchy, per-page "On this page" outline, keyboard-navigable.

**Rationale**: furo is maintained by a Sphinx core maintainer with a
calendar-versioned steady cadence (latest 2025.12.19; its sample site was
rebuilt against Sphinx 9.1.1 in Oct 2026, confirming current-Sphinx
compatibility). Its design goal — "easy to navigate, with
carefully-designed sidebar navigation", "good looking search" — matches
the user's "not required to hunt" requirement directly. WEngine's site is
a few dozen pages at ≤3 nav levels, squarely inside furo's intended
"smaller docsets" sweet spot.

**Alternatives considered**:
- **sphinx-rtd-theme** — better for very deep hierarchies (>4 levels);
  heavier chrome; not needed at our depth.
- **sphinx-book-theme** — viable, but furo's search and outline UX are the
  better-known quantity and it is the theme pip itself uses.
- **Alabaster (Sphinx default)** — minimal sidebar/search; rejected.

## R3. Sphinx source root and the existing documentation corpus

**Decision**: `docs/` is the Sphinx source root (`docs/conf.py`,
`docs/index.md`). The seven existing topic guides (`signals.md`, `tui.md`,
`logic.md`, `delivery.md`, `ros2.md`, `domain-outputs.md`,
`zephyr-hil.md`) stay at their current paths and join the toctree from
there — MyST-Parser is a CommonMark superset, so they render unmodified.
New suites get new subdirectories (`user/`, `dev/`, `api/`, `tests/`,
`benchmarks/`, `examples/`). Root-level `README.md` and `GLOSSARY.md`
remain repository documents, linked from the landing page.

**Rationale**: The corpus's canonical location is already `docs/` —
AGENTS.md and README reference `docs/signals.md` etc. by path, so moving
files would churn unrelated references for zero content gain; FR-009
demands extending the corpus, not reorganizing it. Copying files into a
new `docs-src/` would fork content (two copies to keep in sync — the
exact drift the spec's Edge Cases forbid). Keeping the site root at
`docs/` means one source of truth rendered two ways: GitHub-raw Markdown
and the Sphinx HTML site.

**Trade-off accepted**: files outside the Sphinx source root (README,
GLOSSARY) are not covered by the site's search box. Rejected mitigation
of copying/symlinking them in (content fork / symlink portability);
instead the landing page and the dev guide link both prominently. If
search coverage of the glossary later proves painful, moving
`GLOSSARY.md` under `docs/` is a one-line follow-up task — recorded, not
done speculatively (Constitution V).

**Alternatives considered**:
- New `docs-src/` tree with copies — rejected: forks the corpus.
- Sphinx root at repository root (README as landing) — rejected: README
  is the repo landing page with a different audience; site index.md
  needs audience portals.

## R4. Doxygen configuration and the comment convention

**Decision**: `docs/Doxyfile` extracts `include/wengine/` only
(`RECURSIVE=YES`, `GENERATE_XML=YES`, output `build/docs/doxygen-xml/`)
with `EXTRACT_ALL=YES`, `WARN_IF_UNDOCUMENTED=YES`,
`WARN_NO_PARAMDOC=YES`, `JAVADOC_AUTOBRIEF=NO`, Markdown-in-comments
enabled. The comment convention (full grammar:
`contracts/doxygen-comment-conventions.md`):

```cpp
/**
 * @file
 * One-paragraph role statement for the header (module map anchor).
 */

/**
 * @brief Total energy of the projectile.
 *
 * @param[in] state Particle slot to read (must be live).
 * @return Kinetic + potential energy at the last stepped instant.
 * @note Exact for constant-gravity closed form; SI units (m, m/s).
 */
```

i.e. JavaDoc-style `/** ... */` blocks everywhere with `@`-commands —
the user-mandated form (planning input, 2026-11-19): explicit `@brief`,
`@param[in] name` + description, `@return`/`@note` for contracts
(units, ownership, failure behavior), `@file` headers. The project's
`core::Status` vocabulary means there are no `@throws` — failure
behavior is documented as `@return` prose naming the status codes.

**Rationale**: `EXTRACT_ALL=YES` renders *undocumented* entities too —
gaps stay visible in the built site instead of silently disappearing,
which is what makes FR-008/SC-003's "100% of public interfaces have
contract documentation" auditable: the same run's warnings list every
miss. `WARN_NO_PARAMDOC` extends the audit to missing parameter docs.
`JAVADOC_AUTOBRIEF=NO` + explicit `@brief` tags keeps briefs uniform
and review-able in raw source, where the "first line is special" rule
is easy to break when editing. The `/** ... */` block form and `@`
command markers are the user's stated style preference; `@` and `\`
are exactly equivalent to Doxygen, so the requirement actually being
pinned is uniformity. Doxygen 1.9.8 satisfies Breathe's ≥ 1.9.2 floor.
Extraction scoped to `include/wengine/` matches FR-003's subject (the
public API); `apps/`, `src/`, `tests/` get the same comment *convention*
(FR-008 applies project-wide) but not rendered API pages — their story
is the dev guide narrative plus catalogs, which keeps the reference
tree clean and navigable.

**Alternatives considered**:
- `///` triple-slash comments (and `/*! ... */` Qt blocks) — render
  identically, but the user explicitly mandated the JavaDoc `/** ... */`
  block form; one uniform style wins for consistency.
- `\`-style commands (`\brief`, `\param`, ...) — exactly equivalent to
  `@` commands in Doxygen; `@` is the user's stated preference.
- `EXTRACT_ALL=NO` — rejected: silently hides undocumented entities;
  destroys the coverage audit.
- `JAVADOC_AUTOBRIEF=YES` — rejected: implicit-first-line briefs invite
  drift between raw comment and rendered brief.
- Doxygen over `apps/`+`src/`+`tests/` too — rejected for the rendered
  reference (noise, hurts navigation); convention still applied there.

## R5. Build orchestration and prerequisites

**Decision**: `docs/build.sh` orchestrates: dependency check (doxygen,
sphinx-build, python modules) → `doxygen docs/Doxyfile` → `sphinx-build
-b html docs build/docs/html`. Python deps pinned in
`docs/requirements.txt` (sphinx==9.1.1, myst-parser==5.1.0,
breathe==5.1.0, furo==2025.12.19, sphinx-copybutton==0.5.2). All
generated output is out-of-tree under `build/docs/` (inside the existing
gitignored build dir — out-of-source discipline preserved). No CMake
changes: docs builds are invoked on demand, never as part of engine
configure/build/test.

**Rationale**: Coupling Doxygen/Python availability into CMake configure
would make every engine build carry a docs-toolchain requirement it
doesn't need — against Constitution V (smallest coherent diff, no new
coupling) and the project's precedent of keeping heavy optional surfaces
fully optional (GUI is a CMake flag precisely because its system
dependencies are heavy; docs here are pure user-space scripts). A pinned
requirements file gives reproducible builds without a lockfile manager.
Out-of-tree output keeps the source tree clean, matching the enforced
out-of-source rule.

**Alternatives considered**:
- CMake `BUILD_DOCS=OFF` option + custom `docs` target (mirroring
  BUILD_GUI) — legitimate and idiomatic, rejected *for this phase* to
  keep the diff engine-free; recorded as the natural follow-up if CI
  later wants a single-command build.
- Sphinx `Makefile` (`make -C docs html`) — equivalent; a plain script
  with an explicit dependency check gives better failure messages on this
  host (system pip vs user site-packages).

## R6. Catalog organization and completeness auditing

**Decision**: Test Catalog = per-domain pages under `docs/tests/`
(one page per test family: link, logic-seq, tui, signal, emit, gui,
ros2, ...), each test artifact an `## test_<name>` section with the
five mandatory fields (what it checks / purpose / guards against / how
to run incl. prerequisites / deliberate boundaries) per
`contracts/catalog-schemas.md`. Test-support scripts (probe, PTY smoke
scripts) get entries in the same pages. Benchmark and Example Catalogs
follow the same per-artifact section shape. `docs/audit_catalogs.py`
cross-checks inventories against entries: registered tests parsed from
the CMake test registration (74 today), benchmark/example files from
their directories, and exits non-zero listing every missing entry — the
mechanism behind SC-001/SC-004's "auditable by construction".

**Rationale**: One page per test would be ~74 near-empty pages — noise
that itself causes hunting; one giant page would be unscannable.
Per-domain pages with stable `## test_<name>` anchors keep every entry
deep-linkable (CI, reviews, cross-references from dev-guide pages) while
the domain grouping matches how maintainers actually think about the
suite. Anchored sections make the audit script trivial (name the section
after the target) and make regression review diff-friendly.

**Alternatives considered**:
- One page per test — rejected: navigation noise.
- Single monolithic catalog page — rejected: unscannable at 74 entries.
- Doxygen `\page` comments in test sources as the catalog — rejected:
  prose of this depth does not belong in code comments, and test sources
  are not in the rendered Doxygen tree (R4); the entry needs run
  instructions and prerequisites, which are documentation, not code
  commentary.

## R7. Comment-only safety: how the in-code pass cannot change behavior

**Decision**: The in-code documentation pass is strictly comment-only,
guarded by: (1) comments are added inside existing comment blocks or as
new `/** ... */` blocks touching no code tokens; (2) known pitfalls are handled
by rule — never reflow code while commenting (the `assert()` macro splits
on commas inside braces, AGENTS §11), never "fix" anything spotted along
the way: mismatches become **Findings** reported against the task, per
FR-010; (3) proof by execution — the full `ctest` suite runs before and
after the pass and must produce identical results (SC-007), and the
engine builds warning-clean before and after (comments cannot introduce
warnings, and any that appear indicate a code touch, which is a defect of
the pass itself).

**Rationale**: The feature's only source-file risk is an editing slip
being smuggled in as a "comment". Pre/post suite parity plus the
zero-warning gate makes the claim mechanical rather than aspirational.
Splitting discovered issues into findings keeps the diff reviewable as
pure documentation (Constitution V: no unrelated changes, no
opportunistic fixes).

**Alternatives considered**:
- Rely on review alone — rejected: unverifiable at this diff width
  (129 headers + edge TUs + test files).
- Compile-time `-Wdocumentation` clang gate — attractive extra, but the
  project's GCC 13.3 baseline lacks the flag; noted as optional hardening
  for a future CI lane, not a plan requirement.
