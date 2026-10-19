// Ncurses front end for PhysicsSimApp (`--tui`) — thin assembly.
//
// Terminal setup, the frame loop, and shutdown live here; every work
// domain owns its TU and operates on `tui::State` (see `state.hpp`):
// session setup, text utilities, submission, line editing, frame chrome,
// and the palette/help/form/builder panes with their key handlers.
//
// Reuse contract: every submitted line runs on the async worker (TUI-5)
// through `cli::Shell::execute` with a captured stream. This layer never
// parses commands or calls solvers.
//
// Chrome strings stay ASCII-only: under a C locale, plain (non-wide)
// ncurses renders non-ASCII bytes as `~@`-style control notations and
// miscounts their display width (found via PTY in TUI-2). CLI output is
// ASCII-only, so this keeps every pane exact on any locale.

#include "wengine/tui/run.hpp"

#include "state.hpp"

#include "wengine/tui/views.hpp"

#include <curses.h>

#include <cstdio>
#include <cstdlib>
#include <csignal>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

namespace wengine::tui {

namespace {

// TUI SIGINT flag: with `cbreak()` the tty may still deliver Ctrl-C as
// SIGINT instead of a key. The handler only sets this flag (async-signal
// safe); the frame loop converts it into `async.request_cancel()` on the
// UI thread. Worker paced/forever runs install their own handler while
// stepping and restore this one afterwards.
volatile std::sig_atomic_t g_tui_sigint = 0;

void on_tui_sigint(int) noexcept { g_tui_sigint = 1; }

} // namespace

int run_tui() {
#if defined(__unix__) || defined(__APPLE__)
  if (isatty(STDIN_FILENO) == 0 || isatty(STDOUT_FILENO) == 0) {
    std::fprintf(stderr, "PhysicsSimApp --tui needs an interactive terminal\n");
    return 1;
  }
#endif

  // Minimal terminals (TERM=dumb/unknown/empty) lack cursor addressing, so
  // the full-screen panes cannot render there (verified via PTY: the loop
  // spins but `cup`/`el` are no-ops, function keys are undefined, and
  // flow-control swallows Ctrl-Q). Refuse loudly instead of garbling or
  // hanging; the REPL/headless CLI covers the same commands. NO_COLOR alone
  // still runs full-screen, only without colors (see `use_color` below).
  {
    const char *term = std::getenv("TERM");
    const std::string_view term_view = term != nullptr ? std::string_view(term) : std::string_view{};
    if (!views::full_screen_supported(term_view)) {
      std::fprintf(stderr,
                   "PhysicsSimApp --tui needs a cursor-addressable terminal "
                   "(TERM=%s is not supported); run without --tui for the REPL/headless CLI\n",
                   term != nullptr ? term : "(unset)");
      return 1;
    }
  }

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
  // 100 ms tick: progress redraws + async completion polling while idle.
  // `getch` returns ERR on timeout (handled below, never dispatched).
  (void)timeout(100);

  using SignalHandler = void (*)(int);
  SignalHandler previous_sigint = std::signal(SIGINT, on_tui_sigint);
  if (previous_sigint == SIG_ERR) {
    (void)endwin();
    std::fprintf(stderr, "PhysicsSimApp --tui: could not install Ctrl-C handler\n");
    return 1;
  }

  s.use_color = (has_colors() != FALSE) && views::colors_allowed_env();
  if (s.use_color) {
    (void)start_color();
    (void)init_pair(1, COLOR_WHITE, COLOR_BLUE);
    (void)init_pair(2, COLOR_BLACK, COLOR_GREEN);
    (void)init_pair(3, COLOR_WHITE, COLOR_RED);
    (void)init_pair(views::kPassPair, COLOR_GREEN, COLOR_BLACK);
    (void)init_pair(views::kFailPair, COLOR_RED, COLOR_BLACK);
    (void)init_pair(views::kPromptPair, COLOR_CYAN, COLOR_BLACK);
  }

  while (!s.quit) {
    // Signal-path cancel (tty delivered SIGINT instead of a key).
    if (g_tui_sigint != 0) {
      g_tui_sigint = 0;
      if (s.async.busy()) {
        s.async.request_cancel();
        s.status_msg = "cancelling `" + s.async.line() + "` at step boundary...";
      }
    }
    // Async completion (non-blocking join + scrollback/history routing).
    (void)poll_async(s);
    if (s.quit) {
      break;
    }

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
    if (ch == ERR) {
      continue; // timeout tick: redraw + re-poll above
    }
    if (ch != '\t') {
      // The completion popup is transient: any other key dismisses it.
      s.complete_names.clear();
    }
    handle_key(s, ch);
  }

  // Quit while running: step-boundary stop, then flush/close normally in
  // the worker before exiting (sinks stay coherent, partial state kept).
  if (s.async.busy()) {
    s.async.request_cancel();
    AsyncResult r;
    if (s.async.wait_take(r)) {
      finish_async(s, r);
    }
  }

  (void)std::signal(SIGINT, previous_sigint);
  (void)endwin();
  return 0;
}

} // namespace wengine::tui
