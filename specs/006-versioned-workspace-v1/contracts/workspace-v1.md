# Contract: Workspace File Format v1

**Feature**: specs/006-versioned-workspace-v1/spec.md | **Date**: 2026-11-04
(The single file-format contract for this feature. Saver, loader, examples, and tests all conform to it.)

## 1. Grammar

```text
file     := "version: 1\n" gates wires ties clocks meta
gates    := "gates:\n" gate-row*
gate-row := "  - {id: KINDnn, kind: lowerkind, inputs: n, ignore: [i, ...],
               ignore_output: bool[, ignore_nq: bool][, init: 0|1]}\n"
wires    := "wires:\n" ("  - {from: PIN, to: PIN}\n")*
ties     := "ties:\n" ("  - {a: PIN, to: PIN}\n")*      ; a = leader, b = member, b slot-ordered
clocks   := "clocks:\n" ("  - {name: DOMAIN}\n")*       ; first-seen slot order, may be empty
meta     := "meta:\n  tool: wengine-logic\n"

KINDnn   := gate-kind name + counter ≥ 1, kind-matching (e.g. DFF1, SRLATCH2)
PIN      := output: GATE.Y | GATE.Q | GATE.NQ (negated-Q sources keep NQ spelling)
           input:  comb GATE.k | seq named (DFF D/CLK/RST/SET/EN, DLATCH D/EN,
                   JK J/K/CLK, T T/CLK, SRLATCH S/R)
DOMAIN   := source-pin spelling (e.g. BUF1.Y, DFF2.NQ) | "clk" (implicit domain)
```

Rules:
- Sections appear in the fixed order above; all six required, even when empty.
- `init:` appears only on sequential-kind rows; absent means `0`. `ignore_nq` keeps its existing sequential-only rule.
- The loader accepts the YAML superset (comments, quoting, block/flow styles, multiline); the saver emits exactly the canonical bytes above (lowercase kinds, `true`/`false` spelled out, ascending ignore lists).
- Unknown top-level keys, unknown row keys, and unknown `meta` keys are errors.

## 2. Loader gate

| Input marker | Other sections | Verdict |
|---|---|---|
| `version: 1` | all six present, valid | ACCEPT — restore topology + inits + pinned domains |
| `version: 1` | any required section missing | REFUSE as legacy: diagnostic names version 1 and the missing section |
| `version: 2` | anything | REFUSE as legacy: diagnostic names version 2 and states retirement |
| any other scalar | anything | REFUSE: `unsupported workspace version N (want 1)` |
| missing/non-scalar `version:` | anything | REFUSE (existing first-line diagnostics, reworded to `want 1`) |

After structural acceptance, in order: gate rows → wires → ties → topology validation (flop-aware cycles) → `init` values → `clocks:` set-equality vs derived domains → post-swap async-control/level-settle check (existing illegal-SR refusal). First failure aborts with a `line N:`-prefixed diagnostic; nothing is committed (FR-011).

## 3. Canonical-byte stability

- `save(load(canonical))` is byte-identical (pinned by test).
- `save(load(hand-written superset))` yields canonical bytes; stability holds from the second save (same caveat as the existing comment convention).
- Saver always writes `init:` explicitly on every sequential row and `clocks:` exhaustively (empty list when no edge flops).

## 4. CLI behavior contract (existing spellings, changed file effects)

- `logic save <f>` — writes v1 canonical bytes with live pool Q as inits; reports `saved G gates W wires`.
- `logic load <f>` — accepts v1 only; resets pool lifetime, powers on to inits, applies live controls; reports `loaded G gates W wires`; any refusal leaves workspace AND pool untouched.
- `logic export --schema s` — schema output follows the same v1 writer.
- `sim run --solver logic --seq-file <f> [--seq-clocks d,…] [--seq-from …]` — v1 only; powers the run pool on to file inits; domain names resolve against file-pinned domains (identical spelling to session `step`).

## 5. Reference files

`examples/logic/`: `nand_pair`, `fanout`, `parity3`, `tieoff`, `tied_demorgan` (converted to v1) plus three new advanced sequential files (two-clock counter with mixed inits; enable-gated full-control-set counter with async reset/set; mixed JK/T + latch net). Each carries a header comment with its run command and expected output; each loads, runs, and re-saves stably per §3.
