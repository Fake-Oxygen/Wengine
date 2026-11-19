# Contract: Workspace YAML — Shared Nets (011)

**Seam**: `include/wengine/logic/yaml_store.hpp` (platform-free
header) + `src/logic/yaml_store.cpp` (the single yaml-cpp TU). All
schema work stays inside this seam.

## Generation policy (recorded decision — spec US4.2)

**No generation bump; no retirement.** The marker stays
`version: 1` (`kWorkspaceVersion` unchanged). Rationale (research
D5, the `ignore_nq` precedent): no pre-011 file can contain a shared
construct — savers never wrote one and loaders refused duplicate
`to:` targets — so nothing existing is reinterpreted and no
ambiguity exists to gate on. The module's no-fallback-loader rule
governs retired generations; 011 retires none. This decision is
recorded in `docs/logic.md` schema notes in the same change.

## Canonical save format (byte-stable)

Section order: `version`, `gates`, `wires`, **`shared`**, `ties`,
`clocks`, `meta`. The `shared:` section is emitted **iff at least one
input is marked**, one row per marked input in slot order:

```yaml
version: 1
gates:
  # unchanged rows
wires:
  - {from: AND1.Y, to: BUS.1}      # unchanged row shape;
  - {from: OR2.Y,  to: BUS.1}      # repeated to: legal iff marked
shared:
  - {net: BUS.1}                    # the mark; attachment count is
                                    # derivable from wires rows and is
                                    # not duplicated here
ties: []
clocks: []
meta: {tool: wengine-logic}
```

Round-trip: `save(load(x)) == x` bytes for both mark-free files
(no `shared:` section — byte-identical to today's canonical output)
and shared files.

## Loader rules (loud everywhere)

- Root-key acceptance grows `shared`; unknown sections/keys stay loud
  with the module's `line N:` diagnostics and want-strings updated.
- Absent `shared:` section = no marks (pre-011 files load
  byte-identically — this is the carried-forward decision above).
- `shared:` row keys: `net` only (unknown keys loud); rows must
  resolve to live inputs of live gates; duplicate rows for the same
  input are loud.
- Wire rows: repeated `to:` targets are legal **iff** that input is
  marked; an unmarked duplicate keeps today's `"input already wired
  '<TO>'"` refusal byte-identically (the pre-011 refusal tests stay
  green). Per-attachment validation mirrors the engine exactly: live
  slots, direction, untied destination, per-edge flop-aware cycle
  check, cap (loud at 4 attachments).
- Attachment replay order is file order; the resulting array state is
  deterministic regardless of row order (multiset-order-independent
  resolution, pinned promotion/compaction — see engine-seam.md).
- Whole-graph topology validation and clock-domain set-equality
  checks apply exactly as today; clock domains name the primary CLK
  attachment (documented edge).
- Load stays atomic: all validation into a temporary netlist; the
  workspace is untouched on any failure.

## Post-load session behavior (CLI)

Unchanged contract: an illegal SR net refuses after the swap and
restores workspace + pool backups. Contention counters are never
serialized: a freshly loaded workspace starts with zero counts and
the loaded netlist replays byte-identically with or without prior
contention history (FR-009).

## Test pins (tests/test_logic_yaml.cpp)

- Mark-free canonical bytes unchanged (existing `test_canonical_bytes_v1`
  and `test_examples` stay green untouched).
- Shared round-trip: marks + repeated wires survive save→load→save
  byte-identically; zero-attachment marks round-trip.
- Unmarked duplicate `to:` keeps the existing refusal (existing
  malformed-fixture row stays).
- New loud rows: unknown key in `shared:`, duplicate `net:`, marked
  input exceeding the cap, unmarked duplicate with a `shared:` section
  absent, marked input also tied.
- The new `examples/logic/` shared-bus file joins the `test_examples`
  golden table (the test loads every file in the directory).
