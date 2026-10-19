// Closed-form guided forms: browser + field editor (draw + keys).
//
// Each form composes one argv string submitted through `TuiExecutor`;
// the CLI owns all validation, so invalid input surfaces the CLI
// usage-error text verbatim.

#include "state.hpp"

#include <curses.h>

namespace wengine::tui {

void draw_forms_pane(State &s) {
  const int cols = s.cols;
  const int out_rows = s.out_rows;
  (void)mvaddnstr(1, 0, "Forms -- closed-form calculators | Enter: open | F1: help | Esc: close", cols - 1);
  (void)wclrtoeol(stdscr);
  const std::size_t page = out_rows > 1 ? static_cast<std::size_t>(out_rows - 1) : std::size_t{1};
  std::size_t first = 0;
  if (!s.form_defs.empty() && s.forms_sel >= page) {
    first = s.forms_sel - page + 1;
  }
  for (std::size_t i = 0; i < page; ++i) {
    const std::size_t idx = first + i;
    if (idx >= s.form_defs.size()) {
      break;
    }
    const std::string row = "  " + std::string(s.form_defs[idx].command) + " - " + std::string(s.form_defs[idx].description);
    if (idx == s.forms_sel) {
      (void)attron(A_REVERSE);
    }
    (void)mvaddnstr(static_cast<int>(2 + i), 0, row.c_str(), cols - 1);
    if (idx == s.forms_sel) {
      (void)attroff(A_REVERSE);
    }
  }
}

void draw_form_pane(State &s) {
  const int cols = s.cols;
  const int out_rows = s.out_rows;
  const std::size_t fi = (s.open_form >= 0) ? static_cast<std::size_t>(s.open_form) : 0;
  const std::size_t sfi = fi < s.form_defs.size() ? fi : 0;
  const forms::FormDef &def = s.form_defs[sfi];
  const std::vector<std::string> &vals = s.form_vals[sfi];
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
    if (i == s.form_field) {
      (void)attron(A_REVERSE);
    }
    (void)mvaddnstr(row, 0, frow.c_str(), cols - 1);
    if (i == s.form_field) {
      (void)attroff(A_REVERSE);
    }
    (void)wclrtoeol(stdscr);
    ++row;
  }
  if (row < 1 + out_rows) {
    const std::string srow = "  [ Submit ]";
    if (s.form_field == def.fields.size()) {
      (void)attron(A_REVERSE);
    }
    (void)mvaddnstr(row, 0, srow.c_str(), cols - 1);
    if (s.form_field == def.fields.size()) {
      (void)attroff(A_REVERSE);
    }
    (void)wclrtoeol(stdscr);
    ++row;
  }
  if (row < 1 + out_rows) {
    const std::string sep = s.form_has[sfi] ? ("--- result rc=" + std::to_string(s.form_res[sfi].rc) + " ---") : "--- result (Enter to run) ---";
    (void)mvaddnstr(row, 0, sep.c_str(), cols - 1);
    (void)wclrtoeol(stdscr);
    ++row;
  }
  std::vector<std::string> res_lines;
  if (!s.form_has[sfi]) {
    res_lines.push_back("Fill fields, Enter to run. Empty fields are omitted;");
    res_lines.push_back("the CLI reports all errors (no form-side validation).");
  } else {
    std::size_t dropped = 0;
    split_capped(res_lines, s.form_res[sfi].text, kMaxOutputLines, dropped);
    if (dropped > 0) {
      res_lines.push_back("[... " + std::to_string(dropped) + " lines truncated -- rerun with --emit-csv for the full log]");
    }
    if (res_lines.empty()) {
      res_lines.push_back("(no output)");
    }
  }
  std::size_t first_res = s.form_res_first[sfi];
  if (!res_lines.empty() && first_res >= res_lines.size()) {
    first_res = res_lines.size() - 1;
    s.form_res_first[sfi] = first_res;
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

void forms_key(State &s, int ch) {
  if (ch == kEsc || ch == KEY_F(3) || ch == kCtrlC) {
    s.overlay = Overlay::kNone;
    s.status_msg = "ready";
  } else if (ch == KEY_F(2)) {
    s.pal_filter.clear();
    s.pal_sel = 0;
    s.overlay = Overlay::kPalette;
    s.status_msg = "palette -- " + std::to_string(s.pal_entries.size()) + " commands";
  } else if (ch == KEY_F(4)) {
    s.batches_sel = 0;
    s.overlay = Overlay::kBatches;
    s.status_msg = "builders -- " + std::to_string(s.batch_defs.size()) + " batch runs";
  } else if (ch == KEY_UP) {
    if (s.forms_sel > 0) {
      --s.forms_sel;
    }
  } else if (ch == KEY_DOWN) {
    if (!s.form_defs.empty() && s.forms_sel + 1 < s.form_defs.size()) {
      ++s.forms_sel;
    }
  } else if (ch == KEY_F(1)) {
    if (!s.form_defs.empty() && s.forms_sel < s.form_defs.size()) {
      const std::string cmd(s.form_defs[s.forms_sel].command);
      int help_rc = 0;
      s.help_lines = fetch_help(s.exec, cmd, help_rc);
      s.help_title = "help " + cmd;
      s.help_first = 0;
      s.overlay = Overlay::kHelp;
      s.status_msg = s.help_title + " (exit " + std::to_string(help_rc) + ", Esc to close)";
    }
  } else if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
    if (!s.form_defs.empty() && s.forms_sel < s.form_defs.size()) {
      s.open_form = static_cast<int>(s.forms_sel);
      s.form_field = 0;
      s.form_col = s.form_vals[s.forms_sel].empty() ? 0 : s.form_vals[s.forms_sel][0].size();
      s.overlay = Overlay::kForm;
      s.status_msg = std::string(s.form_defs[s.forms_sel].command) + " form -- Enter runs, Esc lists";
    }
  } else {
    edit_key(s, ch);
  }
}

void form_key(State &s, int ch) {
  const std::size_t sfi = (s.open_form >= 0) ? static_cast<std::size_t>(s.open_form) : 0;
  const std::size_t nfields = sfi < s.form_defs.size() ? s.form_defs[sfi].fields.size() : 0;
  const std::size_t page = s.out_rows > 1 ? static_cast<std::size_t>(s.out_rows - 1) : std::size_t{1};
  if (ch == kEsc) {
    s.overlay = Overlay::kForms;
    s.status_msg = "forms -- " + std::to_string(s.form_defs.size()) + " calculators";
  } else if (ch == KEY_F(3)) {
    s.overlay = Overlay::kNone;
    s.status_msg = "ready";
  } else if (ch == KEY_F(2)) {
    s.pal_filter.clear();
    s.pal_sel = 0;
    s.overlay = Overlay::kPalette;
    s.status_msg = "palette -- " + std::to_string(s.pal_entries.size()) + " commands";
  } else if (ch == KEY_F(4)) {
    s.batches_sel = 0;
    s.overlay = Overlay::kBatches;
    s.status_msg = "builders -- " + std::to_string(s.batch_defs.size()) + " batch runs";
  } else if (ch == KEY_F(1)) {
    if (sfi < s.form_defs.size()) {
      const std::string cmd(s.form_defs[sfi].command);
      int help_rc = 0;
      s.help_lines = fetch_help(s.exec, cmd, help_rc);
      s.help_title = "help " + cmd;
      s.help_first = 0;
      s.overlay = Overlay::kHelp;
      s.status_msg = s.help_title + " (exit " + std::to_string(help_rc) + ", Esc to close)";
    }
  } else if (ch == KEY_PPAGE) {
    if (sfi < s.form_res_first.size()) {
      s.form_res_first[sfi] = s.form_res_first[sfi] >= page ? s.form_res_first[sfi] - page : 0;
    }
  } else if (ch == KEY_NPAGE) {
    if (sfi < s.form_res_first.size()) {
      s.form_res_first[sfi] += page; // clamped at draw time
    }
  } else if (ch == KEY_UP) {
    if (s.form_field > 0) {
      --s.form_field;
      if (s.form_field < nfields && sfi < s.form_vals.size() &&
          s.form_field < s.form_vals[sfi].size()) {
        s.form_col = s.form_vals[sfi][s.form_field].size();
      } else {
        s.form_col = 0;
      }
    }
  } else if (ch == KEY_DOWN || ch == '\t') {
    if (s.form_field <= nfields) {
      ++s.form_field;
      if (s.form_field > nfields) {
        s.form_field = nfields;
      }
      if (s.form_field < nfields && sfi < s.form_vals.size() && s.form_field < s.form_vals[sfi].size()) {
        s.form_col = s.form_vals[sfi][s.form_field].size();
      } else {
        s.form_col = 0;
      }
    }
  } else if (ch == KEY_LEFT) {
    if (s.form_field < nfields && s.form_col > 0) {
      --s.form_col;
    }
  } else if (ch == KEY_RIGHT) {
    if (s.form_field < nfields && sfi < s.form_vals.size() &&
        s.form_field < s.form_vals[sfi].size() &&
        s.form_col < s.form_vals[sfi][s.form_field].size()) {
      ++s.form_col;
    }
  } else if (ch == KEY_HOME) {
    s.form_col = 0;
  } else if (ch == KEY_END) {
    if (s.form_field < nfields && sfi < s.form_vals.size() && s.form_field < s.form_vals[sfi].size()) {
      s.form_col = s.form_vals[sfi][s.form_field].size();
    }
  } else if (ch == KEY_BACKSPACE || ch == kBackspaceAscii || ch == 8) {
    if (s.form_field < nfields && sfi < s.form_vals.size() &&
        s.form_field < s.form_vals[sfi].size() && s.form_col > 0 && s.form_col <= s.form_vals[sfi][s.form_field].size()) {
      s.form_vals[sfi][s.form_field].erase(s.form_col - 1, 1);
      --s.form_col;
    }
  } else if (ch == KEY_DC) {
    if (s.form_field < nfields && sfi < s.form_vals.size() && s.form_field < s.form_vals[sfi].size() &&
        s.form_col < s.form_vals[sfi][s.form_field].size()) {
      s.form_vals[sfi][s.form_field].erase(s.form_col, 1);
    }
  } else if (ch == kCtrlU || ch == kCtrlC) {
    if (s.form_field < nfields && sfi < s.form_vals.size() && s.form_field < s.form_vals[sfi].size()) {
      s.form_vals[sfi][s.form_field].clear();
      s.form_col = 0;
    }
  } else if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
    submit_form(s);
    // Stay in the form so the CLI-identical result sits beside the
    // fields; Esc lists, F3 closes.
  } else if (ch >= 32 && ch < 127) {
    if (s.form_field < nfields && sfi < s.form_vals.size() && s.form_field < s.form_vals[sfi].size()) {
      std::string &val = s.form_vals[sfi][s.form_field];
      if (s.form_col > val.size()) {
        s.form_col = val.size();
      }
      val.insert(s.form_col, 1, static_cast<char>(ch));
      ++s.form_col;
    }
  }
}

} // namespace wengine::tui
