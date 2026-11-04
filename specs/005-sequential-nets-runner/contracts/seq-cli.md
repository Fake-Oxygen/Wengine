# Contract: `sim run --solver logic` sequential options

**Shape**: CLI options on the existing `sim run` command (no new command).

```text
sim run --solver logic --seq-file <workspace.yaml>
        [--dt <s>] [--steps N] [--emit-csv <path>] [--emit-shm <name>]
        [--seq-clocks <d1,d2>] [--seq-from <PIN>=<0|1>]...
```

| Option | Required | Meaning |
|---|---|---|
| `--seq-file` | yes for logic | YAML workspace (schema v1/v2, same loader as `logic load`) |
| `--seq-clocks` | no (default: all) | comma-separated domain names (`clk`, `BUF1.Y`, …); exactly one selection form |
| `--seq-from` | no | repeatable stimulus `PIN=0/1` (named or numeric pins); unset pins read Low (DFF EN reads High per the existing rule) |

**Errors** (all exit 2, state unchanged, message names the cause):
- `--seq-file` missing/unreadable, YAML strict-load refusal (line-numbered).
- unknown domain name; empty domain list; duplicate domain names.
- unknown pin, non-binary value, stray positional args.
- any `--seq-*` with `--solver` ≠ `logic`; `--gravity`/signal payloads with `--solver logic`.
- `dt`/`steps` violations (shared messages with other solvers).

**Completion**: `complete()` covers `--seq-file` (path form), `--seq-clocks`
(domain names from the loaded workspace when available, else flag name),
`--seq-from` (pin names → `PIN=0/1` forms, mirroring `complete_from_value`).
