// Ncurses front end for PhysicsSimApp (`--tui`).
//
// Reuse contract: every submitted line goes through `tui::TuiExecutor`
// (i.e. `cli::Shell::execute` with a captured stream). This TU owns only
// terminal handling -- panes, input editing, history recall, Tab completion,
// command palette, help browser, and scrollback display. It never parses
// commands or calls solvers.
//
// Palette/help reuse: entries derive from `CliRegistry` (see `tui/palette.hpp`)
// so new commands appear for free; help panes run `help <cmd>` through the
// executor, keeping them byte-identical to the CLI by construction.
//
// Chrome strings stay ASCII-only: under a C locale, plain (non-wide)
// ncurses renders non-ASCII bytes as `~@`-style control notations and
// miscounts their display width (found via PTY in TUI-2). CLI output is
// ASCII-only, so this keeps every pane exact on any locale.
//
// Display truncation: one command's captured output is capped at
// `kMaxOutputLines` lines (long `sim`/`sig` console streams belong in
// `--emit-csv` files, not on screen). Async runs with live tails are a
// follow-up on the TUI roadmap; until then a long batch run blocks input
// while computing, exactly like the REPL does.

#include "wengine/tui/run.hpp"

#include "wengine/cli/line_reader.hpp"
#include "wengine/tui/exec.hpp"
#include "wengine/tui/palette.hpp"
#include "wengine/tui/popup.hpp"

#include <curses.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

namespace wengine::tui {
namespace {

constexpr int kCtrlQ = 'Q' - '@';
constexpr int kCtrlC = 'C' - '@';
constexpr int kCtrlU = 'U' - '@';
constexpr int kEsc = 27;
constexpr int kBackspaceAscii = 127;
constexpr std::size_t kMaxScrollback = 2000;
constexpr std::size_t kMaxOutputLines = 500;

constexpr std::string_view kTitle =
    "WEngine TUI  |  Enter: run  Tab: complete  F1: help  F2: commands  "
    "PgUp/PgDn: scroll  F10/Ctrl-Q: quit";
constexpr std::string_view kPrompt = "> ";

enum class Overlay { kNone, kPalette, kHelp };

// Split `text` into lines, keeping at most `max_lines`; `dropped` counts
// the remainder so the caller can say so loudly instead of silently cutting.
void split_capped(std::vector<std::string> &out, std::string_view text, std::size_t max_lines, std::size_t &dropped) {
  std::string cur;
  for (char c : text) {
    if (c == '\n') {
      if (out.size() >= max_lines) {
        ++dropped;
      } else {
        out.push_back(cur);
      }
      cur.clear();
    } else if (c != '\r') {
      if (out.size() < max_lines) {
        cur.push_back(c);
      }
    }
  }
  if (!cur.empty()) {
    if (out.size() >= max_lines) {
      ++dropped;
    } else {
      out.push_back(cur);
    }
  }
}

void push_text(std::vector<std::string> &scrollback, std::string_view text, std::size_t max_lines) {
  std::size_t dropped = 0;
  split_capped(scrollback, text, max_lines, dropped);
  // `split_capped` caps only what this call appends; trim older history here.
  if (scrollback.size() > kMaxScrollback) {
    const auto excess = scrollback.size() - kMaxScrollback;
    scrollback.erase(scrollback.begin(),
                     scrollback.begin() +
                         static_cast<std::vector<std::string>::difference_type>(excess));
  }
  if (dropped > 0) {
    scrollback.push_back("[... " + std::to_string(dropped) + " lines truncated -- rerun with --emit-csv for the full log]");
  }
}

// Fetch `help <target>` (or bare `help`) through the executor so the pane is
// byte-identical to the CLI. The rc is reported, never stored: viewing help
// must not clobber the last command's status-bar exit code.
std::vector<std::string> fetch_help(TuiExecutor &exec, std::string_view target, int &rc_out) {
  const std::string line = target.empty() ? "help" : "help " + std::string(target);
  ExecResult res = exec.execute_line(line);
  rc_out = res.rc;
  std::vector<std::string> lines;
  std::size_t dropped = 0;
  split_capped(lines, res.text, kMaxScrollback, dropped);
  if (dropped > 0) {
    lines.push_back("[... truncated]");
  }
  return lines;
}

// Whitespace-separated token bounds around `cursor` (quotes stay literal:
// completion works on the raw token the CLI tokenizer will later split).
void token_bounds(std::string_view buf, std::size_t cursor, std::size_t &begin, std::size_t &end) {
  begin = cursor;
  while (begin > 0 && buf[begin - 1] != ' ' && buf[begin - 1] != '\t') {
    --begin;
  }
  end = cursor;
  while (end < buf.size() && buf[end] != ' ' && buf[end] != '\t') {
    ++end;
  }
}

} // namespace

int run_tui() {
#if defined(__unix__) || defined(__APPLE__)
  if (isatty(STDIN_FILENO) == 0 || isatty(STDOUT_FILENO) == 0) {
    std::fprintf(stderr, "PhysicsSimApp --tui needs an interactive terminal\n");
    return 1;
  }
#endif

  TuiExecutor exec;
  const std::vector<PaletteEntry> pal_entries = palette_entries();

  // Same history file as the REPL (shared format, shared cap).
  cli::History history;
  const std::string hist_path = cli::LineReader::default_history_path();
  if (!hist_path.empty()) {
    (void)history.load(hist_path);
  }

  std::vector<std::string> scrollback;
  push_text(scrollback,
            "WEngine TUI -- same commands as the CLI. Type `help`, press F1 for "
            "context help, F2 for the command palette, `exit` to quit.",
            kMaxScrollback);

  WINDOW *scr = initscr();
  if (scr == nullptr) {
    std::fprintf(stderr, "PhysicsSimApp --tui: could not initialize terminal\n");
    return 1;
  }
  (void)cbreak();
  (void)noecho();
  (void)keypad(stdscr, TRUE);

  const bool use_color = has_colors() != FALSE;
  if (use_color) {
    (void)start_color();
    (void)init_pair(1, COLOR_WHITE, COLOR_BLUE);
    (void)init_pair(2, COLOR_BLACK, COLOR_GREEN);
    (void)init_pair(3, COLOR_WHITE, COLOR_RED);
  }

  std::string input;
  std::size_t cursor = 0;
  std::string draft;
  int hist_pos = -1; // -1: editing fresh line, else index into history
  int scroll = 0;
  int last_rc = 0;
  std::string status_msg = "ready -- F2 for commands";
  Overlay overlay = Overlay::kNone;
  std::string pal_filter;
  std::size_t pal_sel = 0;
  std::string help_title;
  std::vector<std::string> help_lines;
  std::size_t help_first = 0; // first visible help line (docs read top-down)
  std::vector<std::string> complete_names; // transient Tab popup (non-Tab clears)
  bool quit = false;

  // Shared submit: runs the input line through the executor.
  auto submit_line = [&]() {
    push_text(scrollback, std::string(kPrompt) + input, kMaxScrollback);
    if (!input.empty()) {
      if (history.add(input) && !hist_path.empty()) {
        (void)history.append_to_file(hist_path, input);
      }
    }
    hist_pos = -1;
    draft.clear();
    ExecResult res = exec.execute_line(input);
    input.clear();
    cursor = 0;
    scroll = 0;
    if (res.rc == -1) {
      quit = true;
      last_rc = 0;
      status_msg = "bye";
    } else {
      last_rc = res.rc;
      push_text(scrollback, res.text, kMaxOutputLines);
      status_msg = res.text.empty() ? "ok (no output)" : "exit " + std::to_string(res.rc);
    }
  };

  // Shared line editing for the main view and the pinned help pane (same
  // keys, same history, same completion). Returns true when consumed.
  auto edit_key = [&](int edit_ch) -> bool {
    if (edit_ch == KEY_UP || edit_ch == KEY_DOWN) {
      if (!history.empty()) {
        if (hist_pos == -1) {
          draft = input;
        }
        const int count = static_cast<int>(history.size());
        if (edit_ch == KEY_UP) {
          hist_pos = (hist_pos == -1) ? count - 1 : std::max(0, hist_pos - 1);
        } else {
          hist_pos = (hist_pos == -1) ? -1 : hist_pos + 1;
          if (hist_pos >= count) {
            hist_pos = -1;
          }
        }
        input = (hist_pos == -1) ? draft : history.at(static_cast<std::size_t>(hist_pos));
        cursor = input.size();
      }
      return true;
    }
    if (edit_ch == KEY_LEFT) {
      if (cursor > 0) {
        --cursor;
      }
      return true;
    }
    if (edit_ch == KEY_RIGHT) {
      if (cursor < input.size()) {
        ++cursor;
      }
      return true;
    }
    if (edit_ch == KEY_HOME) {
      cursor = 0;
      return true;
    }
    if (edit_ch == KEY_END) {
      cursor = input.size();
      return true;
    }
    if (edit_ch == KEY_BACKSPACE || edit_ch == kBackspaceAscii || edit_ch == 8) {
      if (cursor > 0) {
        input.erase(cursor - 1, 1);
        --cursor;
        hist_pos = -1;
      }
      return true;
    }
    if (edit_ch == KEY_DC) {
      if (cursor < input.size()) {
        input.erase(cursor, 1);
        hist_pos = -1;
      }
      return true;
    }
    if (edit_ch == kCtrlU || edit_ch == kCtrlC) {
      input.clear();
      cursor = 0;
      hist_pos = -1;
      return true;
    }
    if (edit_ch == '\t') {
      const std::vector<std::string> cand = exec.complete(input, cursor);
      if (cand.empty()) {
        complete_names.clear();
        status_msg = "no completions";
      } else if (cand.size() == 1) {
        std::size_t begin = 0;
        std::size_t end = 0;
        token_bounds(input, cursor, begin, end);
        input.replace(begin, end - begin, cand[0]);
        cursor = begin + cand[0].size();
        if (cursor == input.size()) {
          input.push_back(' ');
          ++cursor;
        }
        hist_pos = -1;
        complete_names.clear();
        status_msg = "completed";
      } else {
        const std::string pre = cli::common_prefix(cand);
        std::size_t begin = 0;
        std::size_t end = 0;
        token_bounds(input, cursor, begin, end);
        const std::string cur_tok = input.substr(begin, end - begin);
        if (pre.size() > cur_tok.size()) {
          input.replace(begin, end - begin, pre);
          cursor = begin + pre.size();
          hist_pos = -1;
          complete_names.clear();
          status_msg = std::to_string(cand.size()) + " candidates -- prefix extended";
        } else {
          // Multi-candidate list goes to the transient popup above the
          // input line, never into the scrollback.
          complete_names = cand;
          status_msg = std::to_string(cand.size()) +
                       " completions above -- keep typing to dismiss";
        }
      }
      return true;
    }
    if (edit_ch >= 32 && edit_ch < 127) {
      input.insert(cursor, 1, static_cast<char>(edit_ch));
      ++cursor;
      hist_pos = -1;
      return true;
    }
    return false;
  };

  while (!quit) {
    int rows = 0;
    int cols = 0;
    getmaxyx(stdscr, rows, cols);
    if (rows < 5) {
      rows = 5;
    }
    if (cols < 20) {
      cols = 20;
    }
    const int out_rows = rows - 3;

    // --- draw ---
    (void)werase(stdscr);
    if (use_color) {
      (void)attron(COLOR_PAIR(1));
    }
    (void)mvaddnstr(0, 0, kTitle.data(), cols - 1);
    (void)wclrtoeol(stdscr);
    if (use_color) {
      (void)attroff(COLOR_PAIR(1));
    }

    if (overlay == Overlay::kHelp) {
      const std::string head =
          help_title + "  (type to compose, Enter runs + closes, PgUp/PgDn scroll, Esc closes)";
      (void)mvaddnstr(1, 0, head.c_str(), cols - 1);
      (void)wclrtoeol(stdscr);
      const std::size_t page =
          out_rows > 1 ? static_cast<std::size_t>(out_rows - 1) : std::size_t{1};
      std::size_t max_first = 0;
      if (help_lines.size() > page) {
        max_first = help_lines.size() - page;
      }
      if (help_first > max_first) {
        help_first = max_first;
      }
      for (std::size_t i = 0; i < page; ++i) {
        const std::size_t idx = help_first + i;
        if (idx >= help_lines.size()) {
          break;
        }
        (void)mvaddnstr(static_cast<int>(2 + i), 0, help_lines[idx].c_str(), cols - 1);
      }
    } else if (overlay == Overlay::kPalette) {
      const std::vector<std::size_t> view = filter_entries(pal_entries, pal_filter);
      const std::size_t sel = view.empty() ? 0 : std::min(pal_sel, view.size() - 1);
      (void)mvaddnstr(1, 0, "Commands -- type to filter | Enter: insert | F1: help | Esc: close",
                      cols - 1);
      (void)wclrtoeol(stdscr);
      const std::string filt = "filter: " + pal_filter;
      (void)mvaddnstr(2, 0, filt.c_str(), cols - 1);
      (void)wclrtoeol(stdscr);
      const std::size_t page =
          out_rows > 2 ? static_cast<std::size_t>(out_rows - 2) : std::size_t{1};
      std::size_t first = 0;
      if (!view.empty() && sel >= page) {
        first = sel - page + 1; // keep the highlight on screen
      }
      if (view.empty()) {
        (void)mvaddnstr(3, 0, "(no match)", cols - 1);
      }
      for (std::size_t i = 0; i < page; ++i) {
        const std::size_t vi = first + i;
        if (vi >= view.size()) {
          break;
        }
        const PaletteEntry &e = pal_entries[view[vi]];
        const std::string row = "  " + e.name + " - " + e.description;
        if (vi == sel) {
          (void)attron(A_REVERSE);
        }
        (void)mvaddnstr(static_cast<int>(3 + i), 0, row.c_str(), cols - 1);
        if (vi == sel) {
          (void)attroff(A_REVERSE);
        }
      }
    } else {
      const std::size_t page = static_cast<std::size_t>(out_rows);
      const std::size_t total = scrollback.size();
      std::size_t max_scroll = 0;
      if (total > page) {
        max_scroll = total - page;
      }
      if (scroll > static_cast<int>(max_scroll)) {
        scroll = static_cast<int>(max_scroll);
      }
      if (scroll < 0) {
        scroll = 0;
      }
      const std::size_t first =
          max_scroll - static_cast<std::size_t>(scroll) > max_scroll
              ? 0
              : max_scroll - static_cast<std::size_t>(scroll);
      for (int i = 0; i < out_rows; ++i) {
        const std::size_t idx = first + static_cast<std::size_t>(i);
        if (idx >= total) {
          break;
        }
        (void)mvaddnstr(1 + i, 0, scrollback[idx].c_str(), cols - 1);
      }
    }

    // Transient completion popup just above the input line (never in the
    // palette, which has its own filter UI). Any non-Tab key already
    // dismissed it above; Tab refreshes it.
    if (overlay != Overlay::kPalette && !complete_names.empty()) {
      const std::size_t width = static_cast<std::size_t>(cols - 1);
      std::size_t max_rows = 6;
      if (max_rows > static_cast<std::size_t>(out_rows)) {
        max_rows = static_cast<std::size_t>(out_rows);
      }
      const std::vector<std::string> popup = wrap_popup_lines(complete_names, width, max_rows);
      const int nrows = static_cast<int>(popup.size());
      for (int i = 0; i < nrows; ++i) {
        (void)attron(A_REVERSE);
        (void)mvaddnstr(out_rows - nrows + 1 + i, 0,
                        popup[static_cast<std::size_t>(i)].c_str(), cols - 1);
        (void)wclrtoeol(stdscr);
        (void)attroff(A_REVERSE);
      }
    }

    if (use_color) {
      (void)attron(COLOR_PAIR(last_rc == 0 ? 2 : 3));
    }
    const std::string status = "rc=" + std::to_string(last_rc) + "  " + status_msg +
                               (overlay == Overlay::kNone && scroll > 0
                                    ? "  [scrolled -PgDn- to follow]"
                                    : "");
    (void)mvaddnstr(rows - 2, 0, status.c_str(), cols - 1);
    (void)wclrtoeol(stdscr);
    if (use_color) {
      (void)attroff(COLOR_PAIR(last_rc == 0 ? 2 : 3));
    }

    const std::size_t avail =
        static_cast<std::size_t>(cols) - kPrompt.size() - 1;
    std::size_t offset = 0;
    if (input.size() > avail) {
      offset = input.size() - avail;
    }
    if (cursor < offset) {
      offset = cursor;
    }
    const std::string visible = input.substr(offset, avail);
    (void)mvaddstr(rows - 1, 0, kPrompt.data());
    (void)addnstr(visible.c_str(), static_cast<int>(avail));
    (void)wclrtoeol(stdscr);
    (void)wmove(stdscr, rows - 1,
                static_cast<int>(kPrompt.size() + (cursor - offset)));
    (void)wrefresh(stdscr);

    // --- input ---
    const int ch = getch();
    if (ch != '\t') {
      // The completion popup is transient: any other key dismisses it.
      complete_names.clear();
    }
    if (ch == KEY_F(10) || ch == kCtrlQ) {
      quit = true;
    } else if (overlay == Overlay::kPalette) {
      const std::vector<std::size_t> view = filter_entries(pal_entries, pal_filter);
      const std::size_t sel = view.empty() ? 0 : std::min(pal_sel, view.size() - 1);
      if (ch == kEsc || ch == kCtrlC) {
        overlay = Overlay::kNone;
        status_msg = "palette closed";
      } else if (ch == KEY_UP) {
        if (sel > 0) {
          pal_sel = sel - 1;
        }
      } else if (ch == KEY_DOWN) {
        if (!view.empty() && sel + 1 < view.size()) {
          pal_sel = sel + 1;
        }
      } else if (ch == KEY_BACKSPACE || ch == kBackspaceAscii || ch == 8) {
        if (!pal_filter.empty()) {
          pal_filter.pop_back();
          pal_sel = 0;
        }
      } else if (ch == KEY_F(1)) {
        if (!view.empty()) {
          const PaletteEntry &e = pal_entries[view[sel]];
          int help_rc = 0;
          help_lines = fetch_help(exec, e.name, help_rc);
          help_title = "help " + e.name;
          help_first = 0;
          overlay = Overlay::kHelp;
          status_msg =
              help_title + " (exit " + std::to_string(help_rc) + ", Esc to close)";
        }
      } else if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
        if (!view.empty()) {
          const PaletteEntry &e = pal_entries[view[sel]];
          input = e.name + " ";
          cursor = input.size();
          hist_pos = -1;
          overlay = Overlay::kNone;
          status_msg = "inserted `" + e.name + "` -- edit args, Enter to run";
        } else {
          status_msg = "no match -- keep typing or Esc";
        }
      } else if (ch >= 32 && ch < 127) {
        pal_filter.push_back(static_cast<char>(ch));
        pal_sel = 0;
      }
    } else if (overlay == Overlay::kHelp) {
      // Pinned help: the input line stays live so syntax stays visible
      // while composing. Up/Down recall history (PgUp/PgDn scroll the pane),
      // Enter runs and keeps the pane open, Esc/F1 closes it.
      const std::size_t help_page =
          out_rows > 1 ? static_cast<std::size_t>(out_rows - 1) : std::size_t{1};
      if (ch == kEsc || ch == KEY_F(1)) {
        overlay = Overlay::kNone;
        status_msg = "ready";
      } else if (ch == KEY_F(2)) {
        pal_filter.clear();
        pal_sel = 0;
        overlay = Overlay::kPalette;
        status_msg = "palette -- " + std::to_string(pal_entries.size()) + " commands";
      } else if (ch == KEY_PPAGE) {
        help_first = help_first >= help_page ? help_first - help_page : 0;
      } else if (ch == KEY_NPAGE) {
        help_first += help_page; // clamped at draw time
      } else if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
        // Running from help closes the pane onto the result.
        submit_line();
        overlay = Overlay::kNone;
      } else {
        edit_key(ch);
      }
    } else if (ch == KEY_F(2)) {
      pal_filter.clear();
      pal_sel = 0;
      overlay = Overlay::kPalette;
      status_msg = "palette -- " + std::to_string(pal_entries.size()) + " commands";
    } else if (ch == KEY_F(1)) {
      const std::string_view tok = first_token(input);
      int help_rc = 0;
      help_lines = fetch_help(exec, tok, help_rc);
      help_title = tok.empty() ? "help" : "help " + std::string(tok);
      help_first = 0;
      overlay = Overlay::kHelp;
      status_msg =
          help_title + " (exit " + std::to_string(help_rc) + ", Esc to close)";
    } else if (ch == KEY_RESIZE) {
      // Layout is recomputed from getmaxyx() every frame; nothing to do.
    } else if (ch == KEY_PPAGE) {
      scroll += out_rows > 1 ? out_rows - 1 : 1;
    } else if (ch == KEY_NPAGE) {
      scroll -= out_rows > 1 ? out_rows - 1 : 1;
    } else if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
      submit_line();
    } else {
      edit_key(ch);
    }
  }

  (void)endwin();
  return 0;
}

} // namespace wengine::tui
