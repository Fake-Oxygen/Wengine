# Contract: Workspace YAML — Pulls + TBUF (012)

**Seam**: `include/wengine/logic/yaml_store.hpp` (platform-free
declarations) + `src/logic/yaml_store.cpp` (the single yaml-cpp TU).
Additive schema, no generation retired, marker stays `version: 1`
(research D6).

## Canonical bytes

```yaml
version: 1
gates:
  - {id: TBUF1, kind: tbuf, inputs: 2}
wires:
  - {from: AND1.Y, to: TBUF1.1}
  - {from: EN1.Y, to: TBUF1.2}
  - {from: TBUF1.Y, to: BUS.1}
shared:
  - {net: BUS.1}
pulls:
  - {net: BUS.1, pull: up}
ties:
clocks:
meta:
  tool: wengine-logic
```

- `pulls:` is optional: emitted iff ≥1 pull exists, one row per
  pulled input in slot order (input index breaks ties
  deterministically). Level spellings: `up` | `down` only
  (lowercase, matching the lowercase kind convention).
- Canonical section order (pinned by the save/load/save round-trip
  test): `version, gates, wires, shared, pulls, ties, clocks, meta`.
- `none` is never written — absence of a row (or of the section)
  expresses it. Save emits no `pulls:` section for pull-free
  workspaces, so their bytes are identical to pre-012 saves.

## Loader rules (all loud, workspace untouched on failure)

- `pulls` joins the accepted root-key set; unknown root keys and
  unknown per-row keys stay loud (existing convention).
- Each row validates: `net` parses to a live input pin
  (dead/out-of-range pins fail); tied or ignored destinations fail
  ("untie/use first" — the engine refusals surface with line
  diagnostics); duplicate `net:` rows fail; unknown `pull:` levels
  fail (including `none` — absence expresses it).
- Row replay order is file order; replay through the engine seams
  (`pull` semantics) so validation matches the interactive verbs
  exactly.
- Pre-012 files (no `pulls:`, no `tbuf` rows) load byte-identically;
  an unmarked duplicate `to:` row keeps today's refusal
  byte-identically; `tbuf` rows in old-loader hands fail on the
  unknown kind (forward-compat is not required — old code never
  wrote them).

## Test pins

- Round-trip: buffer-and-pull workspace saves → loads → replays
  byte-identically (marks, attachments, pulls, outputs).
- Prior-generation policy exercised loudly per the recorded decision
  (carried additively — `version: 1` files without `pulls:` load;
  no retired generation exists to refuse).
- Unchanged-refusal goldens stay green (duplicate `to:` without a
  mark, tied/ignored destinations, unknown kinds/keys).
