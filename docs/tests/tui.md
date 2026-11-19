# Terminal UI

The ncurses TUI slice: everything terminal-free lives in unit binaries
(no display needed), and the one behavior that needs a real terminal —
full-screen launch, panes, completion, recall, quit — lives in the PTY
smoke. The architectural pin throughout: builders and the executor
compose argv strings run through `cli::Shell::execute`, so TUI output
stays byte-identical to the CLI by construction.

## test_tui_async

**What it checks**: the async worker running `Shell::execute` off the UI
thread: result parity with headless runs (goldens, usage errors, and
unknown commands), busy rejection of a second start, Ctrl-C cancel at a
step boundary with flushed sinks, `--forever` cancel, progress counters,
truncation staying loud, and `--emit-*` CSV files landing on disk
unaffected by cancellation.

**Purpose**: long runs must keep the UI live, stay cancellable without
losing flushed data, and print exactly what the CLI would print.

**Guards against**: second-start races, cancel losing buffered output,
progress/truncation drift, and async-vs-headless text divergence.

**How to run**:

```sh
ctest --test-dir build -R tui_async
./build/test_tui_async
```

**Prerequisites**: standard build only (needs threads; also run under
Clang TSAN per project practice for the atomics-only discipline).

**Boundaries**: pane rendering and key handling are not covered here;
see `tui_pty_smoke.py`.

## test_tui_batches

**What it checks**: the `sim`/`sig` run builders: definitions track the
specs, field keys match the `complete` vocabulary, registry-driven
coverage, per-kind signal profiles (sine/square/noise/sum/expression/
table/lua, including UART-family visibility), per-solver `sim` profiles
(projectile gravity vs signal-only uart), argv compose shapes,
headless-execution parity over the full matrix, invalid input surfacing
CLI error text verbatim, and `--forever` composed but never executed
synchronously (async-owned).

**Purpose**: builders must never reimplement parsing; composing one argv
string keeps every batch byte-identical to the CLI.

**Guards against**: builder/spec drift, kind-profile leakage (duty on
sine, uart on projectile), and `--forever` blocking the test thread.

**How to run**:

```sh
ctest --test-dir build -R tui_batches
./build/test_tui_batches
```

**Prerequisites**: standard build only.

**Boundaries**: guided closed-form forms live in `test_tui_forms`;
full-screen interaction lives in `tui_pty_smoke.py`.

## test_tui_choice

**What it checks**: the Space-opened enum-choice dialogs: the shared
ncurses-free dialog model (open/filter/accept/cancel), every static
choice list on forms resolving, registry-derived dynamic options
resolving, batch (run-builder) choices resolving, free-text fields
staying choice-free, and a synthetic future field with fresh choices
resolving with no pane-logic change.

**Purpose**: new commands get dialogs by declaring `choices` (or a
resolver), not by editing panes; this pins that scaling contract.

**Guards against**: stale choice lists after registry edits, dialogs on
free-text fields, and per-command dialog code creeping back in.

**How to run**:

```sh
ctest --test-dir build -R tui_choice
./build/test_tui_choice
```

**Prerequisites**: standard build only.

**Boundaries**: actual key handling and popup rendering need the PTY
smoke; option semantics belong to the CLI tests.

## test_tui_exec

**What it checks**: executor parity over the full registry matrix
(goldens, usage-error shapes, stray positionals — same text, same exit
code), completion candidates equal to `cli::complete_line`, history
sharing proven at the `cli::History` file-format level (TUI loads the
same default path the REPL appends to), palette derivation from the
registry with filtering and first-token rules, help-pane parity, and
popup wrap math.

**Purpose**: the TUI calls `Shell::execute` verbatim, so every line must
print exactly what the headless CLI prints — and new commands must
appear with no TUI-side change.

**Guards against**: executor reimplementation drift, palette missing new
commands, and history-file divergence between TUI and REPL.

**How to run**:

```sh
ctest --test-dir build -R tui_exec
./build/test_tui_exec
```

**Prerequisites**: standard build only.

**Boundaries**: ncurses rendering, panes, and async runs live in
`test_tui_views`, `test_tui_async`, and `tui_pty_smoke.py`.

## test_tui_forms

**What it checks**: the guided closed-form builders: definitions track
the specs, field keys/usages match the `kUsage`/`complete` vocabulary,
IC per-chip dispatch driven from the registry, argv compose shapes,
per-option argument profiles, headless-execution parity over the full
`ohm`/`s0`/`i2c`/`ldo`/`led` plus per-chip matrix, invalid input
surfacing CLI error text verbatim, and argument-set scaling for future
options.

**Purpose**: same composition-only contract as batches: one argv string
through `TuiExecutor`, so valid output and usage errors agree
byte-for-byte with the CLI.

**Guards against**: form/spec drift, per-chip form duplication, and
silently dropped options.

**How to run**:

```sh
ctest --test-dir build -R tui_forms
./build/test_tui_forms
```

**Prerequisites**: standard build only.

**Boundaries**: run builders live in `test_tui_batches`; live
interaction lives in `tui_pty_smoke.py`.

## test_tui_views

**What it checks**: the output-view layer on captured CLI text: line
classification (prompt/truncation/PASS/FAIL/error, with PASS+FAIL usage
lines staying neutral), color allow-listing (`NO_COLOR`, `dumb`/empty/
unknown TERM handling, full-screen gate), theme selection and
preference round-trip, search match spans, wrap reflow, scroll math with
indicator, save reproducing visible text, and the no-per-command-view
pin (the model never parses commands).

**Purpose**: views render whatever the executor captured; highlighting,
search, save, and resize must work uniformly with no per-command code.

**Guards against**: classifier false verdicts, theme-preference loss,
scroll/save drift after reflow, and view logic re-parsing commands.

**How to run**:

```sh
ctest --test-dir build -R tui_views
./build/test_tui_views
```

**Prerequisites**: standard build only (ncurses-free).

**Boundaries**: full-screen drawing needs the PTY smoke; command
semantics belong to the CLI/TUI exec suites.

## tui_pty_smoke.py

**What it checks**: the live `--tui` session on a real PTY: boot renders
the title, F9 opens the theme picker (preview Dracula, apply, reopen
shows active, Esc closes), F3 forms switch panes and submit the dynamic
Ohm U+R pair (`I = 3.1 A`) plus the S0 mode-specific profile switch,
F4 builders switch projectile/signal profiles and submit a square run,
typed `ohm 12V 3A` prints CLI-identical `R = 4` text, single- and
multi-candidate Tab completion, Up history recall with byte-identical
re-run, F10 quit with exit 0, shared history-file contents, theme
persistence across restart, and the non-TTY refusal (exit 1) plus
extra-args usage error (exit 2).

**Purpose**: cursor-addressable panes, keys, and completion only engage
on a TTY; headless binaries cannot prove any of this.

**Guards against**: pane-switch regressions, builder/CLI text drift,
completion/recall breaks, theme-preference loss, and terminal-gating
refusals going silent.

**How to run**:

```sh
ctest --test-dir build -R tui_pty_smoke
python3 tests/tui_pty_smoke.py --bin ./build/PhysicsSimApp
```

**Prerequisites**: the built `PhysicsSimApp` with TUI support plus a
real PTY via `python3 pty.fork` (no display needed); cursor-addressable
`$TERM` (the script sets `xterm`) and isolated history via a temp
`XDG_CACHE_HOME`. Without a PTY-capable host the test fails at spawn
rather than silently skipping.

**Boundaries**: parity matrices and async semantics live in the
`test_tui_*` binaries; this script proves live terminal behavior.
