# Contract: CLI Verbs — `--with-delays` on solve / step / truth (014)

One flag, three existing verbs, inside the logic command domain.
No new verbs, no registry growth (27 stays; `test_cli_args.cpp:689`
pin untouched). Decisions D6–D8 of [research.md](../research.md)
pin everything below.

## Grammar

```text
solve [--in <bits>] [--from <PIN>=<0|1> ...] [--with-delays]
step [DOMAIN] [N] [--cycles N] [--with-delays]
truth [--csv path] [--limit N] [--with-delays]
```

- Boolean flag, per-invocation, never sticky, never serialized
  (nothing persists; no schema or version bump anywhere).
- Unknown flag spellings hit the existing stray-argument usage
  error path (exit 2, workspace untouched); wheel failures
  (budget/overflow) are runtime failures (exit 1) with the settled
  output and workspace untouched.
- Completion: each verb's option list gains `--with-delays`;
  `test_cli_args.cpp` gains the completion vectors (option lists
  only — registry size and verb names unchanged).

## Output contracts (byte-stable)

### solve --with-delays

```text
in: <bits> (<PIN>=<b> ...) [(defaults assumed)]     # unchanged echo
timing: unit 1ps, <K> annotated
  t=<T>ps <PIN>: <old>-><new>                        # pinned order:
  ...                                                # time, then slot
settled in <D>ps
<OUTPIN>=<b> ...                                     # unchanged tail
[contention: ...]                                     # unchanged
```

- `<PIN>` uses the module's display spellings (`AND2.Y`,
  `DFF1.Q`, `DFF1.NQ`, `--from` pin spellings for input nets);
  values spell `0|1|X|Z`; an NQ event follows its Q at the same
  timestamp.
- The settled tail is byte-identical to the flag-off invocation
  (values come from the unchanged `solve()`).
- Empty window (assignment equals the all-zero pre-vector):
  header, `settled in 0ps`, no event lines.

### step --with-delays

```text
<DOMAIN>: cycle <C> (<N> edges)                      # unchanged
timing (edge 1): unit 1ps, <K> annotated              # one window per edge
  t=<T>ps <PIN>: <old>-><new>
  ...
settled in <D>ps
[DFF1.Q: a->b (NQ ...)]                              # unchanged per-flop report
<OUTPIN>=<b> ...                                     # unchanged settled outputs
[contention: ...]                                     # unchanged
```

- The unchanged step machinery runs first (async apply, level
  settle, edge loop); per edge the adapter diffs Q and runs one
  window; `--cycles N` prints N windows with `edge k:` separators
  (only edges taken; `N=0` keeps the existing no-op report).
- The per-flop transition report and settled outputs are
  byte-identical to the flag-off invocation; trajectories are
  equal across modes by construction (edges sample the drained
  pre-edge state).

### truth --with-delays

- Pretty rows: identical values plus an appended `settle` column
  (per-row drain time in ps, hand-derivable as the longest
  annotated path the row's assignment activates).
- `--csv` rows: identical columns plus a trailing `settle_ps`
  column; the `wrote N rows` summary line unchanged.
- Standing refusals inherited identically (shared attachments,
  live buffers); flag-off truth output byte-identical.

## Usage strings, registries, parity

- `LogicSpec::kUsage` and the three verbs' `kUsage` texts gain the
  flag spelling; `help [verb]` renders them.
- Interactive terminal surfaces (REPL session, one-shot, TUI)
  compose the same invocations — byte-identical output by
  construction, pinned by the standing full-registry parity tests;
  no TUI change ships.
- Exit codes: 0 success, 1 runtime/check failure (wheel refusals,
  settled X/Z, S=R=1), 2 usage errors — the module's standing map.

## Documentation duties (same change)

- `docs/logic.md`: §10 verb grammar gains the flag on the three
  verbs; §12 Timing slot ships (the named delay-honoring evaluator
  follow-on lands; the runner/export follow-ons are named).
- `docs/tests/logic.md`: page for `test_logic_timing` (guards:
  goldens, 4-state windows, refusals, parity, determinism).
- `README.md`: flag in the module verb list; runnable
  `glitch_chain.yaml` / `race_window.yaml` examples; test list;
  module-table wording.
- `examples/logic/glitch_chain.yaml` + `examples/logic/
  race_window.yaml`: headers show the run command and the expected
  trace (the workspace save/load round-trip already carries
  annotations per 013).
- `docs/audit_catalogs.py` stays zero-missing / zero-ghost for the
  new test and examples.
