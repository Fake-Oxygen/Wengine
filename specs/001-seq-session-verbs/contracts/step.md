# Contracts: `step` Verb

**Feature**: Sequential Session Verbs | **Interface**: `logic` session / `logic <verb>` one-shot

## Syntax

```text
step [DOMAIN] [N] [--cycles N]
step --help
```

- `step` — advance the implicit/global clock by 1.
- `step N` — advance all flop domains by N edges (N ≥ 0; 0 reports only).
- `step DOMAIN N` — advance only flops whose CLK/EN is driven by the
  net named DOMAIN (source-pin spelling, e.g. `BUF1.Y` or `DFF2.NQ`);
  flops in other domains are untouched.
- `--cycles N` is the explicit long form of a bare N (both spellings
  complete; bare form preferred in usage text).
- Stray positionals beyond `[DOMAIN] N`, or `N` non-numeric/negative →
  usage error (exit 2), no state change.

## Output (stdout, human-readable)

```text
clk: cycle 5 (5 edges)
DFF1.Q: 0->1 (NQ 1->0)
DFF2.Q: 1->1 (no change)
OUT: AND3.Y=1
```

- First line: `<domain>: cycle <count> (<N> edges)`; N=0 prints
  `<domain>: cycle <count> (0 edges, no-op)` plus current outputs.
- One `NAME.Q: old->new` line per stepped flop in slot order, with the
  implied NQ transition in parentheses; flops with no change are marked
  `(no change)` (still listed — scripts can diff verbatim).
- Settled block outputs as `PIN=value` lines (same idiom as `solve`).

## Errors

| Condition | Message contains | Exit | State change |
|-----------|------------------|------|--------------|
| Unknown domain | `step: unknown clock '<d>'` + known domains | 2 | none |
| Bad count | `step: want N >= 0` | 2 | none |
| No sequential gates | `step: no flops in workspace` | 1 | none |
| Step eval failure (X/Z, topology) | `step failed` + reason | 1 | none (engine + CLI atomic) |
| `--cycles` without value / duplicate | standard `usage_error` | 2 | none |

## Completion

Verb name `step`; DOMAIN position completes domain names (`clk` +
live driver-pin spellings); N position completes nothing (numeric);
`--cycles`/`--cycles=`/`--help` flag forms per `solve` precedent.
