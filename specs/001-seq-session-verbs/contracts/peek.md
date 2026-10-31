# Contracts: `peek` Verb

**Feature**: Sequential Session Verbs | **Interface**: `logic` session / `logic <verb>` one-shot

## Syntax

```text
peek [GATE]
peek --help
```

- `peek` — all live flops, slot order.
- `peek GATE` — flops of one gate (e.g. `peek DFF1`; gate must be a live
  sequential gate).
- Unknown gate, or a live combinational gate → usage error naming the
  problem (exit 2). A gate name that never existed vs a removed one both
  report `peek: unknown gate '<g>'`; `peek AND1` (live, combinational)
  reports `peek: '<g>' is not a sequential gate`.

## Output (stdout, human-readable, slot order)

```text
DFF1.Q=1 NQ=0 (serial 0)
DFF2.Q=0 NQ=1 (serial 3)
flops: 2
```

- One `NAME.Q=<0|1> NQ=<0|1> (serial <id>)` line per flop in slot order;
  `NQ` is always NOT Q. Trailing `flops: <k>` count line (scripts assert
  on the count + full text).
- No flops in workspace → `no flops` + success exit (empty-but-success,
  per spec edge case).

## Errors

| Condition | Message contains | Exit | State change |
|-----------|------------------|------|--------------|
| Unknown gate | `peek: unknown gate '<g>'` | 2 | none (read-only verb) |
| Combinational gate | `peek: '<g>' is not a sequential gate` | 2 | none |
| Stray positionals | standard `usage_error` | 2 | none |

(`peek` never fails at runtime: Q values are Low/High by construction;
sync is infallible. A corrupted pool surfaces through `step`, not here.)

## Completion

Verb name `peek`; GATE position completes live sequential gate names
only (reuses `support::collect_gate_names` filtered by
`is_sequential_kind`); `--help` flag form.
