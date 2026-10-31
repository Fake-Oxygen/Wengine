# Contracts: `poke` Verb

**Feature**: Sequential Session Verbs | **Interface**: `logic` session / `logic <verb>` one-shot

## Syntax

```text
poke <PIN>=<0|1> [--from <PIN>=<0|1> ...]
poke <PIN>=release
poke --clear
poke --help
```

- `PIN` uses named pin spelling for sequential blocks (`DFF1.D`,
  `DFF1.CLK`, `DLATCH1.EN`) and numeric spelling elsewhere (`AND1.1`);
  numeric aliases (`DFF1.1`) resolve to the same pin (canonical display
  is the named form). Tied members resolve to their class leader (same
  rule as `solve --from`).
- Value `0`/`1` drives Low/High; `release` removes the sticky entry
  (pin returns to wired/default behavior).
- `--clear` drops all poke entries (reported count); `reset` also clears
  (existing verb, no flag change).

## Output (stdout, human-readable)

```text
poked DFF1.D=1 (leader DFF1.D)
poked AND1.1=0 (leader AND1.1)
released DFF1.D
poke map: empty
```

- One line per applied entry in argument order; repeat pokes report
  `updated PIN old->new`.
- Bare `poke` (no args) lists the current map in slot order, or
  `poke map: empty`.

## Errors

| Condition | Message contains | Exit | State change |
|-----------|------------------|------|--------------|
| Unknown pin | `poke: bad block input pin '<p>'` | 2 | none |
| Wired/ignored/output pin | `poke: '<p>' is not a pokable block input (wired or ignored?)` | 2 | none |
| Named pin on wrong kind (`AND1.D`) or `NQ` as poke target | `poke: bad block input pin '<p>'` | 2 | none |
| Bad value | `poke: takes PIN=0|1|release (e.g. poke DFF1.D=1)` | 2 | none |
| Duplicate `--from` for same pin | `poke: duplicate value for '<p>'` | 2 | none |
| `save`/`load` round-trip | poke map intentionally NOT persisted (usage documents this) | — | — |

## Completion

Verb name `poke`; value position completes `PIN=0`, `PIN=1`,
`PIN=release` forms for pokable pins (reuse `complete_from_value`
helper); `--from`/`--from=`/`--clear`/`--help` flag forms.
