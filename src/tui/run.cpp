// Ncurses front end for PhysicsSimApp (`--tui`) — thin assembly.
//
// Terminal setup, the frame loop, and shutdown live here; every work
// domain owns its TU and operates on `tui::State` (see `state.hpp`):
// session setup, text utilities, submission, line editing, frame chrome,
// and the palette/help/form/builder panes with their key handlers.
//
// Reuse contract: every submitted line goes through `tui::TuiExecutor`
// (i.e. `cli::Shell::execute` with a captured stream). This layer never
// parses commands or calls solvers.
//
// Chrome strings stay ASCII-only: under a C locale, plain (non-wide)
// ncurses renders non-ASCII bytes as `~@`-style control notations and
// miscounts their display width (found via PTY in TUI-2). CLI output is
// ASCII-only, so this keeps every pane exact on any locale.

#include "wengine/tui/run.hpp"

#include "state.hpp"

#include <curses.h>

#include <cstdio>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

namespace wengine::tui {

int run_tui() {
#if defined(__unix__) || defined(__APPLE__)
  if (isatty(STDIN_FILENO) == 0 || isatty(STDOUT_FILENO) == 0) {
    std::fprintf(stderr, "PhysicsSimApp --tui needs an interactive terminal\n");
    return 1;
  }
#endif

  State s;
  init_state(s);

  WINDOW *scr = initscr();
  if (scr == nullptr) {
    std::fprintf(stderr, "PhysicsSimApp --tui: could not initialize terminal\n");
    return 1;
  }
  (void)cbreak();
  (void)noecho();
  (void)keypad(stdscr, TRUE);

  s.use_color = has_colors() != FALSE;
  if (s.use_color) {
    (void)start_color();
    (void)init_pair(1, COLOR_WHITE, COLOR_BLUE);
    (void)init_pair(2, COLOR_BLACK, COLOR_GREEN);
    (void)init_pair(3, COLOR_WHITE, COLOR_RED);
  }

  while (!s.quit) {
    int rows = 0;
    int cols = 0;
    getmaxyx(stdscr, rows, cols);
    if (rows < 5) {
      rows = 5;
    }
    if (cols < 20) {
      cols = 20;
    }
    s.rows = rows;
    s.cols = cols;
    s.out_rows = rows - 3;

    draw_frame(s);

    const int ch = getch();
    if (ch != '\t') {
      // The completion popup is transient: any other key dismisses it.
      s.complete_names.clear();
    }
    handle_key(s, ch);
  }

  (void)endwin();
  return 0;
}

} // namespace wengine::tui
