# TUI (ncurses) — design + decision log

`PhysicsSimApp --tui` opens a terminal UI with the full CLI command set.
Status: scaffolding + palette/help browser + closed-form guided forms
(TUI-1..4) + async worker (TUI-5); output views are roadmap tasks in
Plane (parent: TUI epic).

## 1. Reuse decision (D-TUI-1)

The TUI reuses the CLI command path verbatim and never forks one:

```text
TUI input line -> tui::AsyncTask (worker) -> cli::Shell::execute(line, ostringstream)
                                          -> CliRegistry::find -> Spec::run
               -> captured text rendered into the scrollback pane
Tab            -> cli::complete_line over CliRegistry::all()
history        -> cli::History (same file as the REPL)
```

Consequences:

- TUI output is byte-identical to the REPL/headless CLI by construction;
  `tests/test_tui_exec.cpp` pins executor-vs-`Shell` parity (codes + text);
- Every line runs on the async worker (TUI-5): `tui::AsyncTask` owns one
  worker thread + its own `Shell`, the UI thread only sets the global
  `cli::request_cancel()` atom and reads the `cli::progress_*` atoms
  (TSAN-clean). `tests/test_tui_async.cpp` pins worker-vs-`Shell` parity,
  busy rejection, step-boundary cancel with flushed sinks, progress, and
  `--emit-csv` file outputs.
- New commands work in the TUI with no TUI-side change (same guarantee as
  `sim run --solver`: registry-driven).
- Guided forms added later must only *compose argv strings* for the
  executor; any form that reimplements parsing/validation is a bug.
- `--tui` is a process-launch flag in `apps/cli/main.cpp`, not a registry
  command (a `tui` command inside the REPL would nest terminal modes).

This is deliberately different from the GUI, which links `wengine_core`
directly and needs explicit parity tests per panel. The TUI links
`wengine_cli` (the one sanctioned exception to the apps-never-cli shape).

## 2. Dependency decision (D-TUI-2)

ncurses is a **system package** (`libncurses-dev`), in the same category as
SDL2 + the Vulkan loader for the GUI: never vendored, no version pin to
move. `BUILD_TUI` defaults `ON`; when Curses is missing the CLI still
builds and `--tui` prints a rebuild hint (exit 1). No engine headers
change for TUI work; `wengine/tui/exec.hpp` is ncurses-free.

## 3. Shipped scope

Scaffolding (TUI-1): `include/wengine/tui/exec.hpp` (executor seam),
`include/wengine/tui/run.hpp` + `src/tui/` (`wengine_tui` STATIC TUs, one per
work domain — `state`/`text`/`submit`/`edit`/`draw_main`/`panes_help`/
`panes_forms`/`panes_batches`/`keys` behind the thin `run.cpp` assembly),
`--tui` flag, `BUILD_TUI` option, `tests/test_tui_exec.cpp`, README updates.

Palette + help browser (TUI-2): `include/wengine/tui/palette.hpp`
(registry-derived entries + prefix filter + F1 context token, all
ncurses-free and unit-tested). F2 opens the palette (type to filter,
Enter inserts `<name> ` into the input line, F1 opens help for the
highlighted entry, Esc closes). F1 anywhere opens a scrollable help pane
whose content is `help <cmd>` (or bare `help`) run through the executor --
byte-identical to the CLI, including error shapes. The pane stays pinned
while composing: typing, editing, history recall, and Tab all work with
syntax visible; Enter runs the command and closes the pane onto the output.
F2 from the pane switches back to the palette. Tab multi-candidates render
as a transient reverse-video popup just above the input line (wrapped,
capped with a loud `+N more`, never into the scrollback); any non-Tab key
dismisses it, single/prefix completion behaves as in the REPL.
Viewing help never clobbers the status bar's last-command exit code. TUI chrome strings stay
ASCII-only (C-locale safety: plain ncurses renders non-ASCII bytes as
control notations and miscounts their width -- found via PTY in TUI-2;
CLI output is ASCII-only so panes stay exact). Shared loop notes:
minimal editing, Up/Down shared-history recall, Tab completion mirroring
the REPL, F10/Ctrl-Q quit, `KEY_RESIZE`-safe redraw, non-TTY refusal
(exit 1), `--tui` with extra args is a usage error (exit 2).

Closed-form forms (TUI-3): `include/wengine/tui/forms.hpp`
(ncurses-free composition model + registry-derived `ic` chips/ops,
unit-tested). F3 opens the form browser (all six closed-form families),
Enter opens the highlighted form, Enter inside the form composes one argv
string and submits it through `TuiExecutor` -- the result pane shows the
CLI-identical text beside the fields (also mirrored into the scrollback +
shared history). Field hints come from each spec's `kUsage`/completion
vocabulary; `ic` chips/ops enumerate `IcRegistry` (plus each chip's
`complete`), so new parts appear with no form-side change. Forms never
validate: empty fields are omitted and the CLI owns every error, so
invalid input shows the CLI usage-error text verbatim.
`tests/test_tui_forms.cpp` pins the composition + executor-vs-`Shell`
parity matrix (every option, every `ic` op, plus usage-error shapes).
Verified on a real PTY (F3 browser, ohm prefill submit `R = 22.963`,
Esc back, scrollback mirror).

Run builders (TUI-4): `include/wengine/tui/batches.hpp` (ncurses-free
composition model + registry-derived solver/kind pickers, unit-tested).
F4 opens the builder browser (`sim run` / `sig run`), Enter opens the
highlighted builder, Enter inside composes one argv string and runs it on
the async worker -- finite batches and `--forever` alike show the
CLI-identical text beside the fields (also mirrored into the scrollback +
shared history), so the builder never blocks. Solver names come from `SolverRegistry`, signal kinds from
`kKindNames` (plus `U|I` quantities and `ascii|cobs` UART modes), so new
solvers/kinds appear with no builder-side change; file paths (e.g.
`--emit-csv`, `--sig-table`, `--sig-lua`) are plain editable fields whose
values quote to one argv token when they carry spaces. Builders never
validate: empty fields are omitted, bare flags (`--paced`, `--forever`,
`--emit-health`, `--control-uart`) emit only when enabled, and the CLI
owns every error.
`tests/test_tui_batches.cpp` pins the composition + executor-vs-`Shell`
parity matrix (projectile batches, every batch signal kind incl. the
batch-rejected `sum` refusal, expression/table/lua file kinds, paced
runs, csv strides/selects with on-disk CSV proof, plus usage-error
shapes); paced runs pin rc + deterministic lines only (wall lateness
varies run-to-run even headless); `--forever` pins composition + `wants_async` routing; the worker executes
it until Ctrl-C (step-boundary stop, sinks flushed/closed normally).

Async runs (TUI-5): `include/wengine/cli/cancel.hpp` (global cancel +
progress atoms, owned by the CLI so `sim`/`sig` chunk loops poll them at
step boundaries) + `include/wengine/tui/async.hpp` (ncurses-free single
worker: one thread + its own `Shell`, non-blocking `start`, `request_cancel`
(Ctrl-C), non-blocking `try_take` for the 100 ms frame tick, blocking
`wait_take` for quit). Every input-line / form / builder submission runs
async: the scrollback echoes the prompt line immediately, history appends
on the UI thread, and completion tails the captured text with the 500-line
cap policy (`--emit-*` files land on disk in the worker via the normal CLI
path, unaffected). The status bar shows `running <line> steps done/total
(elapsed) -- Ctrl-C cancels` from the progress atoms (`(forever)` when
`total == UINT64_MAX`); Ctrl-C anywhere (key or SIGINT) requests the
step-boundary stop, F10/Ctrl-Q cancels + joins before exiting. `sim` fast
batches and `sig` finite/control/forever/paced paths all poll
`cli::cancel_requested()` (combined with their SIGINT flag), report
`cli::progress_*` per chunk, and print the partial `interrupted by Ctrl-C`
prefix on success + partial (sinks still flushed/closed, TCP finished).
`tests/test_tui_async.cpp` pins parity, busy rejection, cancel (finite +
forever, CSV flushed with partial rows), progress, and emit-csv outputs;
Clang-TSAN clean (never GCC per AGENTS.md). Verified on a real PTY
(20M-step run: progress counts up, `abc` typing echoes live, Ctrl-C tails
`interrupted by Ctrl-C`, 8-step CSV lands on disk, F10 exits 0).

## 4. Known limits (roadmap, not bugs)

- One command's displayed output is capped at 500 lines (loud truncation
  note). Full logs belong in `--emit-csv` files.
- No mouse, no scrollback search/save yet. `sig` per-source family
  (`list`/`add`/`set`/`start`/`stop`/`drop`) stays on the typed line; the
  F4 builders cover batch `run` modes only.

## 5. Manual verification

No input-automation tooling exists (same as the GUI panels): run
`./build/PhysicsSimApp --tui` on a real TTY and check help display,
completion, history recall across restarts, resize, a long-run
truncation note, and an async cancel (`/tmp/opencode/tui5_pty_smoke.py`:
progress, live typing, Ctrl-C tail, CSV, quit).
