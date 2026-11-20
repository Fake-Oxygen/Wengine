# Contract: Workspace YAML — `delay_ps` (013, pins the shipped schema)

**Seam**: `include/wengine/logic/yaml_store.hpp` (platform-free
declarations) + `src/logic/yaml_store*.cpp` (the single yaml-cpp TU
split). **This feature makes no store change** — the field's schema
shipped with the V2 timing hook; this contract pins what already holds
so the annotation verbs inherit it, and adds only round-trip test
coverage.

## Canonical bytes (unchanged shape)

```yaml
version: 1
gates:
  - {id: AND1, kind: and, inputs: 8, ignore: [2, 8], ignore_output: false, delay_ps: 1500}
  - {id: NOT1, kind: not, inputs: 1, ignore: [], ignore_output: true, delay_ps: 250}
wires:
  ...
```

- `delay_ps:` is a **required key on every gate row** — the checker's
  `kWant[6]` includes it (yaml_store_check.cpp:22); a missing key is a
  loud "gate entry needs 6 fields … missing 'delay_ps'" error (:40);
  unknown keys stay loud.
- Values validate as u64 (:184 — negative or non-numeric is a loud
  "bad delay_ps" with the workspace untouched, the atomic-load
  convention).
- Annotated and un-annotated workspaces share one row shape: only the
  value differs. Pre-feature files (all `delay_ps: 0`) are already in
  canonical form and load byte-identically.
- No marker bump, no new section, no retirement: nothing is
  reinterpreted (the 011-D5/012-D6 additive convention has nothing to
  do here — there is no schema delta at all).

## Loader rules (already loud, stay green unchanged)

- Required-key, u64-validation, and unknown-key refusals are pinned by
  existing tests; this feature must not alter a single refusal
  message or acceptance.
- Replay stores the row value verbatim (yaml_store_replay.cpp:48) —
  loader-accepted values equal verb-accepted values: the u64 field is
  the shared bound (spec FR-013's parity by construction).
- The tieoff example fixture already carries nonzero rows
  (`delay_ps: 1500`, `delay_ps: 250`) and its load-time pins
  (test_logic_yaml.cpp:861-862) already prove nonzero round-trip; no
  new example file joins `examples/logic/` (nothing new to fixture).

## New test pins (this feature's additions)

- **Verb-written round-trip**: `set-delay` via the verb → `save_text`
  carries the value → `load_text` restores it → replay and solve
  outputs byte-identical to the pre-save session.
- **Clear round-trip**: annotate, save, clear, save again — the second
  file's row reads `delay_ps: 0` and both files load cleanly.
- **Pre-feature byte-identity**: a workspace saved before any
  annotation (all zeros) saves/loads byte-identically after the
  feature (pinned by existing goldens staying green).
- **Checkpoint column** (beside the YAML store): the world binary
  serialization round-trips the field (serialize.hpp:352/:791) — the
  new binary pins a save/load cycle with an annotated netlist.

## Forward compatibility note

A workspace saved by a future delay-aware build carries whatever
values it carries; this build loads them verbatim (u64 ≥ 0) and
evaluates zero-delay — honoring the values is the evaluator's job,
never the loader's (spec edge case, "a workspace saved by a future
delay-aware version").
