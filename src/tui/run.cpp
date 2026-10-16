// Minimal ncurses front end for PhysicsSimApp (`--tui`).
//
// Reuse contract: every submitted line goes through `tui::TuiExecutor`
// (i.e. `cli::Shell::execute` with a captured stream). This TU owns only
// terminal handling — panes, input editing, history recall, Tab completion,
// and scrollback display. It never parses commands or calls solvers.
//
// Display truncation: one command's captured output is capped at
// `kMaxOutputLines` lines (long `sim`/`sig` console streams belong in
// `--emit-csv` files, not on screen). Async runs with live tails are a
// follow-up on the TUI roadmap; until then a long batch run blocks input
// while computing, exactly like the REPL does.

#include "wengine/tui/run.hpp"

#include "wengine/cli/line_reader.hpp"
#include "wengine/tui/exec.hpp"

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
constexpr int kBackspaceAscii = 127;
constexpr std::size_t kMaxScrollback = 2000;
constexpr std::size_t kMaxOutputLines = 500;

constexpr std::string_view kTitle =
    "WEngine TUI  |  Enter: run  Tab: complete  F1: help  PgUp/PgDn: scroll  F10/Ctrl-Q: quit";
constexpr std::string_view kPrompt = "> ";

// Split `text` into lines, keeping at most `max_lines`; `dropped` counts
// the remainder so the caller can say so loudly instead of silently cutting.
void split_capped(std::vector<std::string> &out, std::string_view text,
                  std::size_t max_lines, std::size_t &dropped) {
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

void push_text(std::vector<std::string> &scrollback, std::string_view text,
               std::size_t max_lines) {
  std::size_t dropped = 0;
  const std::size_t before = scrollback.size();
  split_capped(scrollback, text, max_lines, dropped);
  // `split_capped` caps only what this call appends; trim older history here.
  if (scrollback.size() > kMaxScrollback) {
    const auto excess = scrollback.size() - kMaxScrollback;
    scrollback.erase(scrollback.begin(),
                     scrollback.begin() +
                         static_cast<std::vector<std::string>::difference_type>(excess));
  }
  if (dropped > 0) {
    scrollback.push_back("[... " + std::to_string(dropped) +
                         " lines truncated — rerun with --emit-csv for the full log]");
  }
  (void)before;
}

// Whitespace-separated token bounds around `cursor` (quotes stay literal:
// completion works on the raw token the CLI tokenizer will later split).
void token_bounds(std::string_view buf, std::size_t cursor, std::size_t &begin,
                  std::size_t &end) {
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

  // Same history file as the REPL (shared format, shared cap).
  cli::History history;
  const std::string hist_path = cli::LineReader::default_history_path();
  if (!hist_path.empty()) {
    (void)history.load(hist_path);
  }

  std::vector<std::string> scrollback;
  push_text(scrollback,
            "WEngine TUI — same commands as the CLI. Type `help` for the list, "
            "`exit` to quit.",
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
  std::string status_msg = "ready — type `help`";
  bool quit = false;

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

    if (use_color) {
      (void)attron(COLOR_PAIR(last_rc == 0 ? 2 : 3));
    }
    const std::string status = "rc=" + std::to_string(last_rc) + "  " + status_msg +
                               (scroll > 0 ? "  [scrolled -PgDn- to follow]"
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
    if (ch == KEY_F(10) || ch == kCtrlQ) {
      quit = true;
    } else if (ch == KEY_RESIZE) {
      // Layout is recomputed from getmaxyx() every frame; nothing to do.
    } else if (ch == KEY_UP || ch == KEY_DOWN) {
      if (!history.empty()) {
        if (hist_pos == -1) {
          draft = input;
        }
        const int count = static_cast<int>(history.size());
        if (ch == KEY_UP) {
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
    } else if (ch == KEY_LEFT) {
      if (cursor > 0) {
        --cursor;
      }
    } else if (ch == KEY_RIGHT) {
      if (cursor < input.size()) {
        ++cursor;
      }
    } else if (ch == KEY_HOME) {
      cursor = 0;
    } else if (ch == KEY_END) {
      cursor = input.size();
    } else if (ch == KEY_BACKSPACE || ch == kBackspaceAscii || ch == 8) {
      if (cursor > 0) {
        input.erase(cursor - 1, 1);
        --cursor;
        hist_pos = -1;
      }
    } else if (ch == KEY_DC) {
      if (cursor < input.size()) {
        input.erase(cursor, 1);
        hist_pos = -1;
      }
    } else if (ch == kCtrlU || ch == kCtrlC) {
      input.clear();
      cursor = 0;
      hist_pos = -1;
    } else if (ch == KEY_PPAGE) {
      scroll += out_rows > 1 ? out_rows - 1 : 1;
    } else if (ch == KEY_NPAGE) {
      scroll -= out_rows > 1 ? out_rows - 1 : 1;
    } else if (ch == '\t') {
      const std::vector<std::string> cand = exec.complete(input, cursor);
      if (cand.empty()) {
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
          status_msg = std::to_string(cand.size()) + " candidates — prefix extended";
        } else {
          std::string listed;
          for (const auto &c : cand) {
            if (!listed.empty()) {
              listed.push_back(' ');
            }
            listed += c;
            if (listed.size() > 2000) {
              listed += " ...";
              break;
            }
          }
          push_text(scrollback, listed, kMaxScrollback);
          scroll = 0;
          status_msg = std::to_string(cand.size()) + " candidates listed above";
        }
      }
    } else if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
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
        status_msg = res.text.empty() ? "ok (no output)"
                                      : "exit " + std::to_string(res.rc);
      }
    } else if (ch == KEY_F(1)) {
      ExecResult res = exec.execute_line("help");
      last_rc = res.rc;
      push_text(scrollback, res.text, kMaxOutputLines);
      scroll = 0;
      status_msg = "help (F1)";
    } else if (ch >= 32 && ch < 127) {
      input.insert(cursor, 1, static_cast<char>(ch));
      ++cursor;
      hist_pos = -1;
    }
  }

  (void)endwin();
  return 0;
}

} // namespace wengine::tui
