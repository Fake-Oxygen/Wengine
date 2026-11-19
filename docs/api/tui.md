# TUI

`tui` is the terminal UI model: everything ncurses-free and
unit-testable. The executor seam reuses the CLI verbatim —
`TuiExecutor::execute_line` runs `cli::Shell::execute` and returns the
captured text, so TUI output is byte-identical to the REPL/headless
CLI by construction; `views` own PASS/FAIL classification, scrollback
search, wrapping, and scroll math over that captured text; `ArgSet`,
forms, and batches model the guided builders; `themes` own the
palettes. The ncurses draw layer itself lives in `src/tui/` and is out
of scope here. Collaborator: `cli` (Shell, `complete_line`). Design and
decision log: [terminal UI](../tui.md); layering:
[Architecture](../dev/architecture.md).

## Usage pattern

Header-only: compile with `g++ -std=c++20 -I include`, run, expect
`kinds=1,4 rows=4 scroll=1` and exit status 0. Classification is
purely textual (no command knowledge), so new solvers need no
view-side change; the two logical lines wrap to four display rows at
width 12.

```cpp
#include "wengine/tui/views.hpp"

#include <cstdio>
#include <string>
#include <vector>

int main() {
  using wengine::tui::views::LineKind;
  // Captured CLI output is the only model input: every pane renders
  // `cli::Shell::execute` text verbatim, so views classify it textually.
  std::vector<std::string> lines{"> sim run --steps 10", "Error: netlist full"};
  const LineKind k0 = wengine::tui::views::classify_line(lines[0]);
  const LineKind k1 = wengine::tui::views::classify_line(lines[1]);

  // Wrap + scroll math are pure model calls; ncurses only applies colors.
  const wengine::tui::views::Wrapped w = wengine::tui::views::wrap_lines(lines, 12);
  const int scroll = wengine::tui::views::scroll_to_show(1, w.rows.size(), 2);
  std::printf("kinds=%d,%d rows=%zu scroll=%d\n", static_cast<int>(k0),
              static_cast<int>(k1), w.rows.size(), scroll);
  return (k0 == LineKind::kPrompt && k1 == LineKind::kError && w.rows.size() == 4 &&
          scroll == 1) ? 0 : 1;
}
```

## Key entities

`TuiExecutor` and `AsyncTask` are the seam into the CLI library: both
delegate into `wengine_cli` (link `-Lbuild -lwengine_cli` for live
use), run one line per call, and never own simulation state.
`AsyncTask` moves that line onto a worker thread with a step-boundary
cancel, keeping input live during long batches. `ArgSet` is the
unbounded argument-index set for guided builders (it replaced a 64-arg
bitmask cap), and `ChoiceDialog` is the ncurses-free model behind
enum-valued form fields — the draw layer owns only attributes and
keys.

```{doxygenclass} wengine::tui::TuiExecutor
:project: wengine
```

```{doxygenstruct} wengine::tui::ExecResult
:project: wengine
```

```{doxygenclass} wengine::tui::AsyncTask
:project: wengine
```

```{doxygenclass} wengine::tui::ArgSet
:project: wengine
```

```{doxygenstruct} wengine::tui::ChoiceDialog
:project: wengine
```

```{doxygenfunction} wengine::tui::views::classify_line
:project: wengine
```

```{doxygenfunction} wengine::tui::views::wrap_lines
:project: wengine
```

## Complete module listing

Everything public in the module, including the form/batch builder
definitions (`tui::forms`, `tui::batches`), the theme palettes
(`tui::themes`), and the output views (`tui::views`).

```{doxygennamespace} wengine::tui
:project: wengine
```
