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
#include "wengine/tui/forms.hpp"
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
    "F3: forms  PgUp/PgDn: scroll  F10/Ctrl-Q: quit";
constexpr std::string_view kPrompt = "> ";

enum class Overlay { kNone, kPalette, kHelp, kForms, kForm };

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
            "context help, F2 for the command palette, F3 for guided forms, `exit` to quit.",
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
  std::string status_msg = "ready -- F2 for commands, F3 for forms";
  Overlay overlay = Overlay::kNone;
  std::string pal_filter;
  std::size_t pal_sel = 0;
  std::string help_title;
  std::vector<std::string> help_lines;
  std::size_t help_first = 0; // first visible help line (docs read top-down)
  std::vector<std::string> complete_names; // transient Tab popup (non-Tab clears)
  bool quit = false;

  // Guided forms (TUI-3): one argv-composing editor per closed-form family.
  // Values persist per form while the TUI runs; submission goes through the
  // executor so validation/output stay CLI-identical.
  const std::vector<forms::FormDef> form_defs = forms::form_defs();
  std::vector<std::vector<std::string>> form_vals;
  form_vals.reserve(form_defs.size());
  for (const auto &d : form_defs) {
    form_vals.emplace_back(d.fields.size());
  }
  auto form_set = [&](std::string_view cmd, std::string_view key_or_label, std::string_view value) {
    for (std::size_t fi = 0; fi < form_defs.size(); ++fi) {
      if (form_defs[fi].command != cmd) {
        continue;
      }
      for (std::size_t i = 0; i < form_defs[fi].fields.size(); ++i) {
        const auto &f = form_defs[fi].fields[i];
        if (f.key == key_or_label || f.label == key_or_label) {
          form_vals[fi][i] = std::string(value);
        }
      }
    }
  };
  form_set("ohm", "--u", "12.4V");
  form_set("ohm", "--i", "0.54A");
  form_set("s0", "mode", "rate");
  form_set("s0", "--power", "2kW");
  form_set("s0", "--const", "1000imp/kWh");
  form_set("i2c", "mode", "pullup");
  form_set("i2c", "--vdd", "3.3V");
  form_set("i2c", "--cb", "200pF");
  form_set("ldo", "--vin", "5V");
  form_set("ldo", "--vout", "3.3V");
  form_set("ldo", "--iout", "500mA");
  form_set("led", "--vs", "5V");
  form_set("led", "--vf", "2V");
  form_set("led", "--if", "20mA");
  form_set("ic", "chip", "ap63205");
  form_set("ic", "op", "inductor");
  form_set("ic", "--vin", "12V");
  std::size_t forms_sel = 0;
  int open_form = -1; // index into form_defs while Overlay::kForm is active
  std::size_t form_field = 0; // 0..fields.size() (last row = Submit)
  std::size_t form_col = 0; // cursor within the active field value
  std::vector<ExecResult> form_res(form_defs.size());
  std::vector<bool> form_has(form_defs.size(), false);
  std::vector<std::string> form_argv(form_defs.size());
  std::vector<std::size_t> form_res_first(form_defs.size(), 0);

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

  // Form submit: compose the open form's argv string and run it through the
  // same executor (never a form-side reimplementation). The composed line
  // also lands in the scrollback + shared history like a typed line, while
  // the form pane keeps the identical result beside the fields.
  auto submit_form = [&]() {
    if (open_form < 0 ||
        static_cast<std::size_t>(open_form) >= form_defs.size()) {
      return;
    }
    const std::size_t fi = static_cast<std::size_t>(open_form);
    const std::string line = forms::compose_line(form_defs[fi], form_vals[fi]);
    form_argv[fi] = line;
    push_text(scrollback, std::string(kPrompt) + line, kMaxScrollback);
    if (!line.empty()) {
      if (history.add(line) && !hist_path.empty()) {
        (void)history.append_to_file(hist_path, line);
      }
    }
    hist_pos = -1;
    draft.clear();
    ExecResult res = exec.execute_line(line);
    scroll = 0;
    form_res_first[fi] = 0;
    if (res.rc == -1) {
      quit = true;
      last_rc = 0;
      status_msg = "bye";
    } else {
      last_rc = res.rc;
      form_res[fi] = res;
      form_has[fi] = true;
      push_text(scrollback, res.text, kMaxOutputLines);
      status_msg = "form `" + line + "` -> exit " + std::to_string(res.rc);
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
    } else if (overlay == Overlay::kForms) {
      (void)mvaddnstr(1, 0, "Forms -- closed-form calculators | Enter: open | F1: help | Esc: close", cols - 1);
      (void)wclrtoeol(stdscr);
      const std::size_t page = out_rows > 1 ? static_cast<std::size_t>(out_rows - 1) : std::size_t{1};
      std::size_t first = 0;
      if (!form_defs.empty() && forms_sel >= page) {
        first = forms_sel - page + 1;
      }
      for (std::size_t i = 0; i < page; ++i) {
        const std::size_t idx = first + i;
        if (idx >= form_defs.size()) {
          break;
        }
        const std::string row = "  " + std::string(form_defs[idx].command) + " - " + std::string(form_defs[idx].description);
        if (idx == forms_sel) {
          (void)attron(A_REVERSE);
        }
        (void)mvaddnstr(static_cast<int>(2 + i), 0, row.c_str(), cols - 1);
        if (idx == forms_sel) {
          (void)attroff(A_REVERSE);
        }
      }
    } else if (overlay == Overlay::kForm) {
      const std::size_t fi = (open_form >= 0) ? static_cast<std::size_t>(open_form) : 0;
      const std::size_t sfi = fi < form_defs.size() ? fi : 0;
      const forms::FormDef &def = form_defs[sfi];
      const std::vector<std::string> &vals = form_vals[sfi];
      const std::string head = std::string(def.command) + " form  (Up/Down/Tab: field, Enter: run, PgUp/PgDn: result, Esc: back)";
      (void)mvaddnstr(1, 0, head.c_str(), cols - 1);
      (void)wclrtoeol(stdscr);
      const std::string argv = forms::compose_line(def, vals);
      const std::string argv_row = "argv: " + argv;
      (void)mvaddnstr(2, 0, argv_row.c_str(), cols - 1);
      (void)wclrtoeol(stdscr);
      int row = 3;
      for (std::size_t i = 0; i < def.fields.size() && row < 1 + out_rows; ++i) {
        const auto &f = def.fields[i];
        const std::string key_disp = f.key.empty() ? "(pos)" : std::string(f.key);
        const std::string val = i < vals.size() ? vals[i] : std::string{};
        const std::string frow = "  " + std::string(f.label) + " " + key_disp + ": " + val + "  # " + std::string(f.hint);
        if (i == form_field) {
          (void)attron(A_REVERSE);
        }
        (void)mvaddnstr(row, 0, frow.c_str(), cols - 1);
        if (i == form_field) {
          (void)attroff(A_REVERSE);
        }
        (void)wclrtoeol(stdscr);
        ++row;
      }
      if (row < 1 + out_rows) {
        const std::string srow = "  [ Submit ]";
        if (form_field == def.fields.size()) {
          (void)attron(A_REVERSE);
        }
        (void)mvaddnstr(row, 0, srow.c_str(), cols - 1);
        if (form_field == def.fields.size()) {
          (void)attroff(A_REVERSE);
        }
        (void)wclrtoeol(stdscr);
        ++row;
      }
      if (row < 1 + out_rows) {
        const std::string sep = form_has[sfi] ? ("--- result rc=" + std::to_string(form_res[sfi].rc) + " ---") : "--- result (Enter to run) ---";
        (void)mvaddnstr(row, 0, sep.c_str(), cols - 1);
        (void)wclrtoeol(stdscr);
        ++row;
      }
      std::vector<std::string> res_lines;
      if (!form_has[sfi]) {
        res_lines.push_back("Fill fields, Enter to run. Empty fields are omitted;");
        res_lines.push_back("the CLI reports all errors (no form-side validation).");
      } else {
        std::size_t dropped = 0;
        split_capped(res_lines, form_res[sfi].text, kMaxOutputLines, dropped);
        if (dropped > 0) {
          res_lines.push_back("[... " + std::to_string(dropped) + " lines truncated -- rerun with --emit-csv for the full log]");
        }
        if (res_lines.empty()) {
          res_lines.push_back("(no output)");
        }
      }
      std::size_t first_res = form_res_first[sfi];
      if (!res_lines.empty() && first_res >= res_lines.size()) {
        first_res = res_lines.size() - 1;
        form_res_first[sfi] = first_res;
      }
      for (std::size_t i = first_res; i < res_lines.size() && row < 1 + out_rows; ++i) {
        (void)mvaddnstr(row, 0, res_lines[i].c_str(), cols - 1);
        (void)wclrtoeol(stdscr);
        ++row;
      }
      for (; row < 1 + out_rows; ++row) {
        (void)move(row, 0);
        (void)wclrtoeol(stdscr);
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
    if (overlay == Overlay::kForm && open_form >= 0 && static_cast<std::size_t>(open_form) < form_defs.size()) {
      // Focus stays in the active form field (bottom line keeps its draft).
      const std::size_t sfi = static_cast<std::size_t>(open_form);
      const forms::FormDef &cdef = form_defs[sfi];
      if (form_field < cdef.fields.size()) {
        const auto &ff = cdef.fields[form_field];
        const std::string key_disp = ff.key.empty() ? "(pos)" : std::string(ff.key);
        const std::string prefix = "  " + std::string(ff.label) + " " + key_disp + ": ";
        const std::string &fval = form_vals[sfi][form_field];
        if (form_col > fval.size()) {
          form_col = fval.size();
        }
        const std::size_t favail = static_cast<std::size_t>(cols) > prefix.size() + 1 ? static_cast<std::size_t>(cols) - prefix.size() - 1 : std::size_t{1};
        std::size_t foff = 0;
        if (fval.size() > favail) {
          foff = fval.size() - favail;
        }
        if (form_col < foff) {
          foff = form_col;
        }
        const int frow = 3 + static_cast<int>(form_field);
        const int fcol = static_cast<int>(prefix.size() + (form_col - foff));
        if (frow >= 1 && frow < rows - 2) {
          (void)wmove(stdscr, frow, fcol < cols - 1 ? fcol : cols - 1);
        } else {
          (void)wmove(stdscr, rows - 1, static_cast<int>(kPrompt.size() + (cursor - offset)));
        }
      } else {
        (void)wmove(stdscr, rows - 1, static_cast<int>(kPrompt.size() + (cursor - offset)));
      }
    } else {
      (void)wmove(stdscr, rows - 1, static_cast<int>(kPrompt.size() + (cursor - offset)));
    }
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
          status_msg = help_title + " (exit " + std::to_string(help_rc) + ", Esc to close)";
        }
      } else if (ch == KEY_F(3)) {
        forms_sel = 0;
        overlay = Overlay::kForms;
        status_msg = "forms -- " + std::to_string(form_defs.size()) + " calculators";
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
      } else if (ch == KEY_F(3)) {
        forms_sel = 0;
        overlay = Overlay::kForms;
        status_msg = "forms -- " + std::to_string(form_defs.size()) + " calculators";
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
    } else if (overlay == Overlay::kForms) {
      if (ch == kEsc || ch == KEY_F(3) || ch == kCtrlC) {
        overlay = Overlay::kNone;
        status_msg = "ready";
      } else if (ch == KEY_F(2)) {
        pal_filter.clear();
        pal_sel = 0;
        overlay = Overlay::kPalette;
        status_msg = "palette -- " + std::to_string(pal_entries.size()) + " commands";
      } else if (ch == KEY_UP) {
        if (forms_sel > 0) {
          --forms_sel;
        }
      } else if (ch == KEY_DOWN) {
        if (!form_defs.empty() && forms_sel + 1 < form_defs.size()) {
          ++forms_sel;
        }
      } else if (ch == KEY_F(1)) {
        if (!form_defs.empty() && forms_sel < form_defs.size()) {
          const std::string cmd(form_defs[forms_sel].command);
          int help_rc = 0;
          help_lines = fetch_help(exec, cmd, help_rc);
          help_title = "help " + cmd;
          help_first = 0;
          overlay = Overlay::kHelp;
          status_msg = help_title + " (exit " + std::to_string(help_rc) + ", Esc to close)";
        }
      } else if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
        if (!form_defs.empty() && forms_sel < form_defs.size()) {
          open_form = static_cast<int>(forms_sel);
          form_field = 0;
          form_col = form_vals[forms_sel].empty() ? 0 : form_vals[forms_sel][0].size();
          overlay = Overlay::kForm;
          status_msg = std::string(form_defs[forms_sel].command) + " form -- Enter runs, Esc lists";
        }
      } else {
        edit_key(ch);
      }
    } else if (overlay == Overlay::kForm) {
      const std::size_t sfi = (open_form >= 0) ? static_cast<std::size_t>(open_form) : 0;
      const std::size_t nfields = sfi < form_defs.size() ? form_defs[sfi].fields.size() : 0;
      const std::size_t page = out_rows > 1 ? static_cast<std::size_t>(out_rows - 1) : std::size_t{1};
      if (ch == kEsc) {
        overlay = Overlay::kForms;
        status_msg = "forms -- " + std::to_string(form_defs.size()) + " calculators";
      } else if (ch == KEY_F(3)) {
        overlay = Overlay::kNone;
        status_msg = "ready";
      } else if (ch == KEY_F(2)) {
        pal_filter.clear();
        pal_sel = 0;
        overlay = Overlay::kPalette;
        status_msg = "palette -- " + std::to_string(pal_entries.size()) + " commands";
      } else if (ch == KEY_F(1)) {
        if (sfi < form_defs.size()) {
          const std::string cmd(form_defs[sfi].command);
          int help_rc = 0;
          help_lines = fetch_help(exec, cmd, help_rc);
          help_title = "help " + cmd;
          help_first = 0;
          overlay = Overlay::kHelp;
          status_msg = help_title + " (exit " + std::to_string(help_rc) + ", Esc to close)";
        }
      } else if (ch == KEY_PPAGE) {
        if (sfi < form_res_first.size()) {
          form_res_first[sfi] = form_res_first[sfi] >= page ? form_res_first[sfi] - page : 0;
        }
      } else if (ch == KEY_NPAGE) {
        if (sfi < form_res_first.size()) {
          form_res_first[sfi] += page; // clamped at draw time
        }
      } else if (ch == KEY_UP) {
        if (form_field > 0) {
          --form_field;
          if (form_field < nfields && sfi < form_vals.size() && form_field < form_vals[sfi].size()) {
            form_col = form_vals[sfi][form_field].size();
          } else {
            form_col = 0;
          }
        }
      } else if (ch == KEY_DOWN || ch == '\t') {
        if (form_field <= nfields) {
          ++form_field;
          if (form_field > nfields) {
            form_field = nfields;
          }
          if (form_field < nfields && sfi < form_vals.size() && form_field < form_vals[sfi].size()) {
            form_col = form_vals[sfi][form_field].size();
          } else {
            form_col = 0;
          }
        }
      } else if (ch == KEY_LEFT) {
        if (form_field < nfields && form_col > 0) {
          --form_col;
        }
      } else if (ch == KEY_RIGHT) {
        if (form_field < nfields && sfi < form_vals.size() && form_field < form_vals[sfi].size() && form_col < form_vals[sfi][form_field].size()) {
          ++form_col;
        }
      } else if (ch == KEY_HOME) {
        form_col = 0;
      } else if (ch == KEY_END) {
        if (form_field < nfields && sfi < form_vals.size() && form_field < form_vals[sfi].size()) {
          form_col = form_vals[sfi][form_field].size();
        }
      } else if (ch == KEY_BACKSPACE || ch == kBackspaceAscii || ch == 8) {
        if (form_field < nfields && sfi < form_vals.size() && form_field < form_vals[sfi].size() && form_col > 0 &&
            form_col <= form_vals[sfi][form_field].size()) {
          form_vals[sfi][form_field].erase(form_col - 1, 1);
          --form_col;
        }
      } else if (ch == KEY_DC) {
        if (form_field < nfields && sfi < form_vals.size() && form_field < form_vals[sfi].size() && form_col < form_vals[sfi][form_field].size()) {
          form_vals[sfi][form_field].erase(form_col, 1);
        }
      } else if (ch == kCtrlU || ch == kCtrlC) {
        if (form_field < nfields && sfi < form_vals.size() && form_field < form_vals[sfi].size()) {
          form_vals[sfi][form_field].clear();
          form_col = 0;
        }
      } else if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
        submit_form();
        // Stay in the form so the CLI-identical result sits beside the
        // fields; Esc lists, F3 closes.
      } else if (ch >= 32 && ch < 127) {
        if (form_field < nfields && sfi < form_vals.size() && form_field < form_vals[sfi].size()) {
          std::string &val = form_vals[sfi][form_field];
          if (form_col > val.size()) {
            form_col = val.size();
          }
          val.insert(form_col, 1, static_cast<char>(ch));
          ++form_col;
        }
      }
    } else if (ch == KEY_F(2)) {
      pal_filter.clear();
      pal_sel = 0;
      overlay = Overlay::kPalette;
      status_msg = "palette -- " + std::to_string(pal_entries.size()) + " commands";
    } else if (ch == KEY_F(3)) {
      forms_sel = 0;
      overlay = Overlay::kForms;
      status_msg = "forms -- " + std::to_string(form_defs.size()) + " calculators";
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
