// Run-builder panes: browser + field editor (draw + keys).
//
// Each builder composes one argv string run on the async worker (TUI-5);
// finite batches and `--forever` alike, so the builder never blocks.

#include "state.hpp"

#include "wengine/cli/cancel.hpp"

#include <cstdint>

#include <curses.h>

namespace wengine::tui {

void draw_batches_pane(State &s) {
  const int cols = s.cols;
  const int out_rows = s.out_rows;
  (void)mvaddnstr(1, 0, "Run builders -- sim/sig batches | Enter: open | F1: help | Esc: close", cols - 1);
  (void)wclrtoeol(stdscr);
  const std::size_t page = out_rows > 1 ? static_cast<std::size_t>(out_rows - 1) : std::size_t{1};
  std::size_t first = 0;
  if (!s.batch_defs.empty() && s.batches_sel >= page) {
    first = s.batches_sel - page + 1;
  }
  for (std::size_t i = 0; i < page; ++i) {
    const std::size_t idx = first + i;
    if (idx >= s.batch_defs.size()) {
      break;
    }
    const std::string row = "  " + std::string(s.batch_defs[idx].command) + " " + std::string(s.batch_defs[idx].mode) + " - " +
                            std::string(s.batch_defs[idx].description);
    if (idx == s.batches_sel) {
      (void)attron(A_REVERSE);
    }
    (void)mvaddnstr(static_cast<int>(2 + i), 0, row.c_str(), cols - 1);
    if (idx == s.batches_sel) {
      (void)attroff(A_REVERSE);
    }
  }
}

void draw_batch_pane(State &s) {
  const int cols = s.cols;
  const int out_rows = s.out_rows;
  const std::size_t bi = (s.open_batch >= 0) ? static_cast<std::size_t>(s.open_batch) : 0;
  const std::size_t sbi = bi < s.batch_defs.size() ? bi : 0;
  const batches::BatchDef &def = s.batch_defs[sbi];
  const std::vector<std::string> &vals = s.batch_vals[sbi];
  const std::string head = std::string(def.command) + " " + std::string(def.mode) +
      " builder  (Up/Down/Tab: field, Enter: run, PgUp/PgDn: result, Esc: back)";
  (void)mvaddnstr(1, 0, head.c_str(), cols - 1);
  (void)wclrtoeol(stdscr);
  const std::string argv = batches::compose_line(def, vals);
  const std::string argv_row = "argv: " + argv;
  (void)mvaddnstr(2, 0, argv_row.c_str(), cols - 1);
  (void)wclrtoeol(stdscr);
  // Field viewport: builders carry ~30 fields, so only a window shows
  // at once. `batch_top` scrolls to keep the highlight visible; the
  // Submit row is the last viewport row and the result tail keeps the
  // rows below it.
  const std::size_t nfields = def.fields.size();
  const std::size_t total_rows = nfields + 1; // fields + Submit
  std::size_t cap = 1;
  if (out_rows > 5) {
    cap = static_cast<std::size_t>(out_rows) - 4;
  }
  if (cap > total_rows) {
    cap = total_rows;
  }
  std::size_t &top = s.batch_top[sbi];
  if (top >= total_rows) {
    top = total_rows > 0 ? total_rows - 1 : 0;
  }
  if (s.batch_field < top) {
    top = s.batch_field;
  } else if (s.batch_field >= top + cap) {
    top = s.batch_field - cap + 1;
  }
  if (top + cap > total_rows) {
    top = total_rows > cap ? total_rows - cap : 0;
  }
  int row = 3;
  for (std::size_t i = top; i < total_rows && row < 1 + out_rows; ++i) {
    if (i == nfields) {
      const std::string srow = "  [ Submit ]";
      if (s.batch_field == nfields) {
        (void)attron(A_REVERSE);
      }
      (void)mvaddnstr(row, 0, srow.c_str(), cols - 1);
      if (s.batch_field == nfields) {
        (void)attroff(A_REVERSE);
      }
      (void)wclrtoeol(stdscr);
      ++row;
      continue;
    }
    const auto &f = def.fields[i];
    const std::string flag_mark = f.flag ? "[flag] " : "";
    const std::string val = i < vals.size() ? vals[i] : std::string{};
    const std::string frow = "  " + std::string(f.label) + " " + std::string(f.key) + ": " + flag_mark + val + "  # " + std::string(f.hint);
    if (i == s.batch_field) {
      (void)attron(A_REVERSE);
    }
    (void)mvaddnstr(row, 0, frow.c_str(), cols - 1);
    if (i == s.batch_field) {
      (void)attroff(A_REVERSE);
    }
    (void)wclrtoeol(stdscr);
    ++row;
  }
  const bool running_here = s.async.busy() && s.async_origin == AsyncOrigin::kBatch && s.async_index == sbi;
  if (row < 1 + out_rows) {
    std::string sep;
    if (running_here) {
      sep = "--- running (Ctrl-C cancels at step boundary) ---";
    } else {
      sep = s.batch_has[sbi] ? ("--- result rc=" + std::to_string(s.batch_res[sbi].rc) + " ---") : "--- result (Enter to run) ---";
    }
    (void)mvaddnstr(row, 0, sep.c_str(), cols - 1);
    (void)wclrtoeol(stdscr);
    ++row;
  }
  std::vector<std::string> res_lines;
  if (running_here) {
    std::string prog = "starting...";
    if (cli::progress_active()) {
      const std::uint64_t done = cli::progress_done();
      const std::uint64_t total = cli::progress_total();
      if (total == UINT64_MAX) {
        prog = "steps " + std::to_string(done) + " (forever)";
      } else {
        prog = "steps " + std::to_string(done) + "/" + std::to_string(total);
      }
    }
    res_lines.push_back("running `" + s.async.line() + "` " + prog);
    res_lines.push_back("input stays live -- Ctrl-C cancels, Esc lists");
  } else if (!s.batch_has[sbi]) {
    res_lines.push_back("Fill fields, Enter to run. Empty fields are omitted;");
    res_lines.push_back("the CLI reports all errors (no builder-side validation).");
  } else {
    std::size_t dropped = 0;
    split_capped(res_lines, s.batch_res[sbi].text, kMaxOutputLines, dropped);
    if (dropped > 0) {
      res_lines.push_back("[... " + std::to_string(dropped) + " lines truncated -- rerun with --emit-csv for the full log]");
    }
    if (res_lines.empty()) {
      res_lines.push_back("(no output)");
    }
  }
  std::size_t first_res = s.batch_res_first[sbi];
  if (!res_lines.empty() && first_res >= res_lines.size()) {
    first_res = res_lines.size() - 1;
    s.batch_res_first[sbi] = first_res;
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
}

void batches_key(State &s, int ch) {
  if (ch == kEsc || ch == KEY_F(4) || ch == kCtrlC) {
    s.overlay = Overlay::kNone;
    s.status_msg = "ready";
  } else if (ch == KEY_F(2)) {
    s.pal_filter.clear();
    s.pal_sel = 0;
    s.overlay = Overlay::kPalette;
    s.status_msg = "palette -- " + std::to_string(s.pal_entries.size()) + " commands";
  } else if (ch == KEY_F(3)) {
    s.forms_sel = 0;
    s.overlay = Overlay::kForms;
    s.status_msg = "forms -- " + std::to_string(s.form_defs.size()) + " calculators";
  } else if (ch == KEY_UP) {
    if (s.batches_sel > 0) {
      --s.batches_sel;
    }
  } else if (ch == KEY_DOWN) {
    if (!s.batch_defs.empty() && s.batches_sel + 1 < s.batch_defs.size()) {
      ++s.batches_sel;
    }
  } else if (ch == KEY_F(1)) {
    if (!s.batch_defs.empty() && s.batches_sel < s.batch_defs.size()) {
      const std::string cmd(s.batch_defs[s.batches_sel].command);
      int help_rc = 0;
      s.help_lines = fetch_help(s.exec, cmd, help_rc);
      s.help_title = "help " + cmd;
      s.help_first = 0;
      s.overlay = Overlay::kHelp;
      s.status_msg = s.help_title + " (exit " + std::to_string(help_rc) + ", Esc to close)";
    }
  } else if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
    if (!s.batch_defs.empty() && s.batches_sel < s.batch_defs.size()) {
      s.open_batch = static_cast<int>(s.batches_sel);
      s.batch_field = 0;
      s.batch_col = s.batch_vals[s.batches_sel].empty() ? 0 : s.batch_vals[s.batches_sel][0].size();
      s.overlay = Overlay::kBatch;
      s.status_msg = std::string(s.batch_defs[s.batches_sel].command) + " " + std::string(s.batch_defs[s.batches_sel].mode) +
                     " builder -- Enter runs, Esc lists";
    }
  } else {
    edit_key(s, ch);
  }
}

void batch_key(State &s, int ch) {
  const std::size_t sbi = (s.open_batch >= 0) ? static_cast<std::size_t>(s.open_batch) : 0;
  const std::size_t bnfields = sbi < s.batch_defs.size() ? s.batch_defs[sbi].fields.size() : 0;
  const std::size_t page = s.out_rows > 1 ? static_cast<std::size_t>(s.out_rows - 1) : std::size_t{1};
  if (ch == kEsc) {
    s.overlay = Overlay::kBatches;
    s.status_msg = "builders -- " + std::to_string(s.batch_defs.size()) + " batch runs";
  } else if (ch == KEY_F(4)) {
    s.overlay = Overlay::kNone;
    s.status_msg = "ready";
  } else if (ch == KEY_F(2)) {
    s.pal_filter.clear();
    s.pal_sel = 0;
    s.overlay = Overlay::kPalette;
    s.status_msg = "palette -- " + std::to_string(s.pal_entries.size()) + " commands";
  } else if (ch == KEY_F(3)) {
    s.forms_sel = 0;
    s.overlay = Overlay::kForms;
    s.status_msg = "forms -- " + std::to_string(s.form_defs.size()) + " calculators";
  } else if (ch == KEY_F(1)) {
    if (sbi < s.batch_defs.size()) {
      const std::string cmd(s.batch_defs[sbi].command);
      int help_rc = 0;
      s.help_lines = fetch_help(s.exec, cmd, help_rc);
      s.help_title = "help " + cmd;
      s.help_first = 0;
      s.overlay = Overlay::kHelp;
      s.status_msg = s.help_title + " (exit " + std::to_string(help_rc) + ", Esc to close)";
    }
  } else if (ch == KEY_PPAGE) {
    if (sbi < s.batch_res_first.size()) {
      s.batch_res_first[sbi] = s.batch_res_first[sbi] >= page ? s.batch_res_first[sbi] - page : 0;
    }
  } else if (ch == KEY_NPAGE) {
    if (sbi < s.batch_res_first.size()) {
      s.batch_res_first[sbi] += page; // clamped at draw time
    }
  } else if (ch == KEY_UP) {
    if (s.batch_field > 0) {
      --s.batch_field;
      if (s.batch_field < bnfields && sbi < s.batch_vals.size() &&
          s.batch_field < s.batch_vals[sbi].size()) {
        s.batch_col = s.batch_vals[sbi][s.batch_field].size();
      } else {
        s.batch_col = 0;
      }
    }
  } else if (ch == KEY_DOWN || ch == '\t') {
    if (s.batch_field <= bnfields) {
      ++s.batch_field;
      if (s.batch_field > bnfields) {
        s.batch_field = bnfields;
      }
      if (s.batch_field < bnfields && sbi < s.batch_vals.size() &&
          s.batch_field < s.batch_vals[sbi].size()) {
        s.batch_col = s.batch_vals[sbi][s.batch_field].size();
      } else {
        s.batch_col = 0;
      }
    }
  } else if (ch == KEY_LEFT) {
    if (s.batch_field < bnfields && s.batch_col > 0) {
      --s.batch_col;
    }
  } else if (ch == KEY_RIGHT) {
    if (s.batch_field < bnfields && sbi < s.batch_vals.size() &&
        s.batch_field < s.batch_vals[sbi].size() &&
        s.batch_col < s.batch_vals[sbi][s.batch_field].size()) {
      ++s.batch_col;
    }
  } else if (ch == KEY_HOME) {
    s.batch_col = 0;
  } else if (ch == KEY_END) {
    if (s.batch_field < bnfields && sbi < s.batch_vals.size() &&
        s.batch_field < s.batch_vals[sbi].size()) {
      s.batch_col = s.batch_vals[sbi][s.batch_field].size();
    }
  } else if (ch == KEY_BACKSPACE || ch == kBackspaceAscii || ch == 8) {
    if (s.batch_field < bnfields && sbi < s.batch_vals.size() &&
        s.batch_field < s.batch_vals[sbi].size() && s.batch_col > 0 &&
        s.batch_col <= s.batch_vals[sbi][s.batch_field].size()) {
      s.batch_vals[sbi][s.batch_field].erase(s.batch_col - 1, 1);
      --s.batch_col;
    }
  } else if (ch == KEY_DC) {
    if (s.batch_field < bnfields && sbi < s.batch_vals.size() &&
        s.batch_field < s.batch_vals[sbi].size() &&
        s.batch_col < s.batch_vals[sbi][s.batch_field].size()) {
      s.batch_vals[sbi][s.batch_field].erase(s.batch_col, 1);
    }
  } else if (ch == kCtrlU || ch == kCtrlC) {
    if (s.batch_field < bnfields && sbi < s.batch_vals.size() &&
        s.batch_field < s.batch_vals[sbi].size()) {
      s.batch_vals[sbi][s.batch_field].clear();
      s.batch_col = 0;
    }
  } else if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
    submit_batch(s);
    // Stay in the builder so the CLI-identical result sits beside the
    // fields (or the async hold note for --forever); Esc lists, F4 closes.
  } else if (ch >= 32 && ch < 127) {
    if (s.batch_field < bnfields && sbi < s.batch_vals.size() &&
        s.batch_field < s.batch_vals[sbi].size()) {
      std::string &val = s.batch_vals[sbi][s.batch_field];
      if (s.batch_col > val.size()) {
        s.batch_col = val.size();
      }
      val.insert(s.batch_col, 1, static_cast<char>(ch));
      ++s.batch_col;
    }
  }
}

} // namespace wengine::tui
