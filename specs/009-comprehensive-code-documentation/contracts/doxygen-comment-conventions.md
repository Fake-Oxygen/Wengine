# Contract: Doxygen Comment Conventions

**Feature**: `specs/009-comprehensive-code-documentation/spec.md` | **Date**: 2026-11-19

The grammar every source comment follows so Doxygen renders it into the
API Reference. This is the contract behind FR-008 and the SC-003 coverage
audit; `docs/Doxyfile` enforces it mechanically (`WARN_IF_UNDOCUMENTED`,
`WARN_NO_PARAMDOC`).

**Comment form is user-mandated (planning input, 2026-11-19)**: JavaDoc
blocks `/** ... */` with `@`-commands (`@brief`, `@param`, `@return`,
`@note`, `@file`). `@` and `\` markers are equivalent to Doxygen —
uniformity to this one form is what is being pinned.

## DC1. File headers (every public header, 129 total)

```cpp
/**
 * @file
 * One-paragraph statement of the header's role: what it owns, what it
 * must never do (per AGENTS.md dependency rules), and who consumes it.
 */
```

The role statement is the module map's anchor: a reader landing on any
header file page in the HTML site knows the file's place in the engine
before reading a single entity.

## DC2. Entity blocks (public types, functions, members)

```cpp
/**
 * @brief Total energy of the projectile.
 *
 * @param[in] state Particle slot to read; the slot must be live.
 * @return Kinetic + potential energy at the last stepped instant, in
 *         joules (SI).
 * @note Closed-form exact for constant gravity; no accumulation error.
 */
constexpr units::Value<units::Energy> total_energy(const Particle& p) noexcept;
```

Rules:

- JavaDoc-style `/** ... */` blocks with ` * ` continuation lines — one
  house style (`///` triple-slash and `\`-commands render identically
  but are rejected for uniformity, research R4).
- `@brief` is an explicit tag. `JAVADOC_AUTOBRIEF=NO` — nothing depends
  on the "first line is special" rule, which is easy to break while
  editing. Keep briefs one line.
- `@param[in] name` / `@param[out] name` + a description that states
  meaning **and units** when a raw number crosses the boundary.
- `@return` states the value's meaning; failure behavior names the
  status codes (DC3).
- `@note` carries contracts: units/ownership/lifetime, determinism
  obligations, "hot path: no allocation" style facts.
- Multi-entity headers document **every** public entity — the audit
  counts them all.

## DC3. Failure behavior: status vocabulary, not exceptions

The engine throws nothing and uses `core::Status`; there is no
`@throws` anywhere. Failure behavior is part of `@return` prose:

```cpp
 * @return kOk on success; kBadArgument when dt is non-positive;
 *         kFull when the pool has no free slot (state unchanged).
```

The named codes must match the real enum spellings — they link up in the
rendered page and get checked by the accuracy pass (FR-013).

## DC4. Units, ownership, determinism — the three recurring contracts

- Quantities crossing a boundary are `units::Value<Dim>` (SI base units
  internally); comment says the dimension and practical unit.
- Non-owning views/sinks say **who owns the lifetime** ("pointer must
  outlive the runner; never stores state").
- Anything order-sensitive (evaluation order, slot order, serial
  monotonicity, snapshot timing) says so in `@note` — these are the rules
  Constitution II forbids breaking, so the API page must surface them.

## DC5. Non-obvious logic sites (implementation files, not extracted)

`src/`, `apps/`, `tests/`, `benchmarks/`, `examples/` get explanatory
**free-form** comments at tricky sites (FP evaluation order, seqlock
re-read loops, terminal raw-mode re-entry, protocol framing, E12
next-decade rule, `assert()` macro comma pitfall). Doxygen syntax is not
required there — nothing renders them — but the *why* must be at the
site. Public headers always use full DC1/DC2 block syntax.

## DC6. Editing rules (comment-only guarantee, SC-007)

- Comments touch no code tokens: no reflowing, no renaming, no
  whitespace-only "improvements", no macro-argument edits.
- Near `assert()` calls: braces do not protect macro arguments — add
  comments only on their own lines (AGENTS §11 lesson).
- Anything spotted that looks like a defect becomes a **Finding**
  (reported, not fixed, FR-010); it is never silently corrected inside
  the documentation pass.

## Failure modes

- Undocumented public entity → renders as a bare signature on the site
  **and** appears in the Doxygen warnings → fails the coverage audit.
- Mismatched `@param` name → Doxygen warning → build not warning-clean.
- Stale status-code name in `@return` → caught by the accuracy pass
  (FR-013), recorded as a finding.
