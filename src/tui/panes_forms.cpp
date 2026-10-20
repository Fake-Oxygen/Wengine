// Closed-form guided forms: browser + field editor (draw + keys).
//
// Each form composes one argv string run on the async worker (TUI-5);
// the CLI owns all validation, so invalid input surfaces the CLI
// usage-error text verbatim.

#include "state.hpp"

#include "wengine/tui/views.hpp"

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

namespace {

void reset_form_argument_cursor(State &s, std::size_t form, const forms::ArgumentProfile &profile) {
  const forms::FormDef &def = s.form_defs[form];
  s.form_arg_sel = 0;
  while (s.form_arg_sel < def.arguments.size() &&
         forms::argument_state(def.arguments[s.form_arg_sel], s.form_arg_selected[form], profile) == forms::ArgumentState::kInactive) {
    ++s.form_arg_sel;
  }
}

const forms::ArgumentProfile *sync_form_profile(State &s, std::size_t form) {
  const forms::FormDef &def = s.form_defs[form];
  const std::size_t index = forms::active_profile_index(def, s.form_vals[form]);
  if (index >= def.profiles.size()) {
    return nullptr;
  }
  if (s.form_active_profile[form] != static_cast<int>(index)) {
    s.form_active_profile[form] = static_cast<int>(index);
    s.form_arg_selected[form] = def.profiles[index].default_selected;
    reset_form_argument_cursor(s, form, def.profiles[index]);
  }
  return &def.profiles[index];
}

bool form_field_visible(const forms::FormDef &def, const forms::ArgumentProfile &profile, std::size_t field, std::uint64_t selected) {
  for (const forms::ArgumentDef &argument : def.arguments) {
    if (argument.user_input && argument.field_index == field) {
      return forms::argument_state(argument, selected, profile) == forms::ArgumentState::kSelected;
    }
  }
  return true;
}

std::size_t step_form_argument(const forms::FormDef &def, const forms::ArgumentProfile &profile, std::uint64_t selected, std::size_t current,
                               int direction) {
  std::size_t candidate = current;
  for (std::size_t n = 0; n < def.arguments.size(); ++n) {
    if (direction < 0) {
      if (candidate == 0) {
        return current;
      }
      --candidate;
    } else {
      if (candidate + 1 >= def.arguments.size()) {
        return current;
      }
      ++candidate;
    }
    if (forms::argument_state(def.arguments[candidate], selected, profile) != forms::ArgumentState::kInactive) {
      return candidate;
    }
  }
  return current;
}

void draw_dynamic_form(State &s, std::size_t sfi) {
  const int out_rows = s.out_rows;
  const int cols = s.cols;
  const forms::FormDef &def = s.form_defs[sfi];
  const forms::ArgumentProfile *profile = sync_form_profile(s, sfi);
  if (profile == nullptr) {
    return;
  }
  const std::vector<std::string> &vals = s.form_vals[sfi];
  const std::uint64_t selected = s.form_arg_selected[sfi];
  const int left_width = cols / 2;
  const int right_x = left_width + 1;
  const int right_width = cols - right_x;
  const int value_width = left_width > 1 ? left_width - 1 : 1;

  const std::string left_head = s.form_active_pane == 0 ? "Command builder [active]" : "Command builder (Ctrl+B, Left)";
  const std::string right_head = s.form_active_pane == 1
      ? "Arguments [active]: " + std::string(profile->name)
      : "Arguments: " + std::string(profile->name) + " (Ctrl+B, Right)";
  (void)mvaddnstr(1, 0, left_head.c_str(), value_width);
  (void)mvaddnstr(1, right_x, right_head.c_str(), right_width - 1);
  (void)mvaddnstr(2, 0, ("argv: " + forms::compose_line(def, vals, selected)).c_str(), value_width);
  (void)mvaddnstr(2, right_x, "Space: select / deselect", right_width - 1);

  int row = 3;
  s.form_cursor_row = s.rows - 1;
  for (std::size_t i = 0; i < def.fields.size() && row < 1 + out_rows; ++i) {
    if (!form_field_visible(def, *profile, i, selected)) {
      continue;
    }
    const forms::FormField &field = def.fields[i];
    const std::string key = field.key.empty() ? "(pos)" : std::string(field.key);
    const std::string value = i < vals.size() ? vals[i] : std::string{};
    const std::string text = "  " + std::string(field.label) + " " + key + ": " + value + "  # " + std::string(field.hint);
    if (s.form_active_pane == 0 && i == s.form_field) {
      (void)attron(A_REVERSE);
    }
    (void)mvaddnstr(row, 0, text.c_str(), value_width);
    if (s.form_active_pane == 0 && i == s.form_field) {
      (void)attroff(A_REVERSE);
      s.form_cursor_row = row;
    }
    (void)move(row, left_width);
    (void)addch('|');
    (void)wclrtoeol(stdscr);
    ++row;
  }
  if (row < 1 + out_rows) {
    if (s.form_active_pane == 0 && s.form_field == def.fields.size()) {
      (void)attron(A_REVERSE);
    }
    (void)mvaddnstr(row, 0, "  [ Submit ]", value_width);
    if (s.form_active_pane == 0 && s.form_field == def.fields.size()) {
      (void)attroff(A_REVERSE);
      s.form_cursor_row = row;
    }
    (void)move(row, left_width);
    (void)addch('|');
    (void)wclrtoeol(stdscr);
    ++row;
  }

  const bool running_here = s.async.busy() && s.async_origin == AsyncOrigin::kForm && s.async_index == sfi;
  std::vector<std::string> result_lines;
  if (running_here) {
    result_lines.push_back("running `" + s.async.line() + "`");
    result_lines.push_back("input stays live -- Ctrl-C cancels");
  } else if (!s.form_has[sfi]) {
    result_lines.push_back("Enter runs the composed command.");
    result_lines.push_back("The CLI owns parsing and validation.");
  } else {
    std::size_t dropped = 0;
    split_capped(result_lines, s.form_res[sfi].text, kMaxOutputLines, dropped);
    if (dropped > 0) {
      result_lines.push_back("[... " + std::to_string(dropped) + " lines truncated]");
    }
    if (result_lines.empty()) {
      result_lines.push_back("(no output)");
    }
  }
  if (row < 1 + out_rows) {
    const std::string divider = running_here ? "--- running (Ctrl-C cancels) ---"
        : (s.form_has[sfi] ? "--- result rc=" + std::to_string(s.form_res[sfi].rc) + " ---" : "--- result (Enter to run) ---");
    (void)mvaddnstr(row, 0, divider.c_str(), value_width);
    (void)move(row, left_width);
    (void)addch('|');
    ++row;
  }
  const std::size_t result_width = value_width > 1 ? static_cast<std::size_t>(value_width - 1) : std::size_t{1};
  const views::Wrapped wrapped = views::wrap_lines(result_lines, result_width);
  std::size_t first = s.form_res_first[sfi];
  if (wrapped.rows.empty()) {
    first = 0;
  } else if (first >= wrapped.rows.size()) {
    first = wrapped.rows.size() - 1;
  }
  s.form_res_first[sfi] = first;
  for (std::size_t i = first; i < wrapped.rows.size() && row < 1 + out_rows; ++i) {
    const std::size_t logical = wrapped.logical_for_row[i];
    const views::LineKind kind = logical < result_lines.size() ? views::classify_line(result_lines[logical]) : views::LineKind::kNormal;
    const int pair = views::color_pair_for(kind);
    if (s.use_color && pair != 0) {
      (void)attron(COLOR_PAIR(pair));
    } else if (!s.use_color && (kind == views::LineKind::kFail || kind == views::LineKind::kError)) {
      (void)attron(A_BOLD);
    }
    (void)mvaddnstr(row, 0, wrapped.rows[i].c_str(), value_width);
    if (s.use_color && pair != 0) {
      (void)attroff(COLOR_PAIR(pair));
    } else if (!s.use_color && (kind == views::LineKind::kFail || kind == views::LineKind::kError)) {
      (void)attroff(A_BOLD);
    }
    (void)move(row, left_width);
    (void)addch('|');
    ++row;
  }

  int arg_row = 3;
  std::size_t shown_arguments = 0;
  for (std::size_t i = 0; i < def.arguments.size() && arg_row < 1 + out_rows; ++i) {
    const forms::ArgumentDef &argument = def.arguments[i];
    const forms::ArgumentState state = forms::argument_state(argument, selected, *profile);
    if (state == forms::ArgumentState::kInactive) {
      continue;
    }
    std::string text;
    switch (state) {
    case forms::ArgumentState::kSelected:
      text = "[x] " + std::string(argument.label) + "  selected";
      break;
    case forms::ArgumentState::kAvailable:
      text = "[ ] " + std::string(argument.label) + "  ";
      switch (forms::argument_requirement(argument, selected, *profile)) {
      case forms::ArgumentRequirement::kRequired:
        text += "required";
        break;
      case forms::ArgumentRequirement::kAlternative:
        text += "choose path";
        break;
      case forms::ArgumentRequirement::kOptional:
        text += "optional";
        break;
      }
      break;
    case forms::ArgumentState::kComputed:
      text = "[-] " + std::string(argument.label) + "  computed";
      break;
    case forms::ArgumentState::kBlocked:
      text = "[-] " + std::string(argument.label) + "  invalid";
      break;
    case forms::ArgumentState::kDerived:
      text = "    " + std::string(argument.label) + "  derived";
      break;
    case forms::ArgumentState::kInactive:
      break;
    }
    if (s.form_active_pane == 1 && i == s.form_arg_sel) {
      (void)attron(A_REVERSE);
    } else if (state == forms::ArgumentState::kBlocked || state == forms::ArgumentState::kComputed || state == forms::ArgumentState::kDerived) {
      (void)attron(A_DIM);
    }
    (void)mvaddnstr(arg_row, right_x, text.c_str(), right_width - 1);
    if (s.form_active_pane == 1 && i == s.form_arg_sel) {
      (void)attroff(A_REVERSE);
    } else if (state == forms::ArgumentState::kBlocked || state == forms::ArgumentState::kComputed || state == forms::ArgumentState::kDerived) {
      (void)attroff(A_DIM);
    }
    (void)move(arg_row, left_width);
    (void)addch('|');
    ++arg_row;
    ++shown_arguments;
  }
  if (shown_arguments == 0 && arg_row < 1 + out_rows) {
    (void)mvaddnstr(arg_row, right_x, "No argument inputs for this operation.",
                    right_width - 1);
    (void)move(arg_row, left_width);
    (void)addch('|');
  }
  for (int y = 1; y < 1 + out_rows; ++y) {
    (void)move(y, left_width);
    (void)addch('|');
  }
}

} // namespace

void draw_form_pane(State &s) {
  const int cols = s.cols;
  const int out_rows = s.out_rows;
  const std::size_t fi = (s.open_form >= 0) ? static_cast<std::size_t>(s.open_form) : 0;
  const std::size_t sfi = fi < s.form_defs.size() ? fi : 0;
  (void)sync_form_profile(s, sfi);
  const forms::FormDef &def = s.form_defs[sfi];
  if (!def.arguments.empty()) {
    draw_dynamic_form(s, sfi);
    return;
  }
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
  const bool running_here = s.async.busy() && s.async_origin == AsyncOrigin::kForm && s.async_index == sfi;
  if (row < 1 + out_rows) {
    const std::string sep = running_here ? "--- running (Ctrl-C cancels) ---"
                                         : (s.form_has[sfi] ? ("--- result rc=" + std::to_string(s.form_res[sfi].rc) + " ---") : "--- result (Enter to run) ---");
    (void)mvaddnstr(row, 0, sep.c_str(), cols - 1);
    (void)wclrtoeol(stdscr);
    ++row;
  }
  std::vector<std::string> res_lines;
  if (running_here) {
    res_lines.push_back("running `" + s.async.line() + "`");
    res_lines.push_back("input stays live -- Ctrl-C cancels");
  } else if (!s.form_has[sfi]) {
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
  // Result tail reflows on resize: wrap to the current width and page in
  // display-row space (highlight stays on the logical captured text).
  const std::size_t res_width = cols > 1 ? static_cast<std::size_t>(cols - 1) : std::size_t{1};
  const views::Wrapped res_wrapped = views::wrap_lines(res_lines, res_width);
  const std::size_t res_total = res_wrapped.rows.size();
  if (res_total == 0) {
    first_res = 0;
  } else if (first_res >= res_total) {
    first_res = res_total - 1;
  }
  s.form_res_first[sfi] = first_res;
  for (std::size_t i = first_res; i < res_total && row < 1 + out_rows; ++i) {
    const std::size_t logical = res_wrapped.logical_for_row[i];
    const views::LineKind kind = logical < res_lines.size() ? views::classify_line(res_lines[logical]) : views::LineKind::kNormal;
    const int pair = views::color_pair_for(kind);
    if (s.use_color && pair != 0) {
      (void)attron(COLOR_PAIR(pair));
    } else if (!s.use_color && (kind == views::LineKind::kFail || kind == views::LineKind::kError)) {
      (void)attron(A_BOLD);
    }
    (void)mvaddnstr(row, 0, res_wrapped.rows[i].c_str(), cols - 1);
    if (s.use_color && pair != 0) {
      (void)attroff(COLOR_PAIR(pair));
    } else if (!s.use_color && (kind == views::LineKind::kFail || kind == views::LineKind::kError)) {
      (void)attroff(A_BOLD);
    }
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
      s.form_active_pane = 0;
      s.form_arg_sel = 0;
      s.form_pane_prefix = false;
      const forms::FormDef &def = s.form_defs[s.forms_sel];
      const forms::ArgumentProfile *profile = sync_form_profile(s, s.forms_sel);
      if (profile != nullptr) {
        reset_form_argument_cursor(s, s.forms_sel, *profile);
      }
      if (!def.arguments.empty() && profile != nullptr) {
        while (s.form_field < def.fields.size() &&
               !form_field_visible(def, *profile, s.form_field,
                                   s.form_arg_selected[s.forms_sel])) {
          ++s.form_field;
        }
      }
      s.form_col = s.form_field < s.form_vals[s.forms_sel].size() ? s.form_vals[s.forms_sel][s.form_field].size() : 0;
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
  const forms::ArgumentProfile *profile =
      sfi < s.form_defs.size() ? sync_form_profile(s, sfi) : nullptr;
  const bool dynamic = profile != nullptr && !s.form_defs[sfi].arguments.empty();

  if (s.form_pane_prefix) {
    if (ch == KEY_LEFT) {
      s.form_active_pane = 0;
      s.form_pane_prefix = false;
      s.status_msg = "command builder pane";
    } else if (ch == KEY_RIGHT) {
      s.form_active_pane = 1;
      s.form_pane_prefix = false;
      s.status_msg = "argument choices pane (Space selects)";
    } else if (ch == kEsc) {
      s.form_pane_prefix = false;
      s.status_msg = "pane switch cancelled";
    }
    return;
  }
  if (dynamic && ch == kCtrlB) {
    s.form_pane_prefix = true;
    s.status_msg = "Ctrl+B then Left/Right to switch panes";
    return;
  }

  if (dynamic && s.form_active_pane == 1) {
    const forms::FormDef &def = s.form_defs[sfi];
    if (ch == KEY_UP) {
      s.form_arg_sel = step_form_argument(def, *profile, s.form_arg_selected[sfi],
                                          s.form_arg_sel, -1);
      return;
    }
    if (ch == KEY_DOWN) {
      s.form_arg_sel = step_form_argument(def, *profile, s.form_arg_selected[sfi],
                                          s.form_arg_sel, 1);
      return;
    }
    if (ch == ' ' && s.form_arg_sel < def.arguments.size()) {
      const forms::ArgumentDef &argument = def.arguments[s.form_arg_sel];
      const forms::ArgumentState state = forms::argument_state(
          argument, s.form_arg_selected[sfi], *profile);
      if (state == forms::ArgumentState::kSelected) {
        s.form_arg_selected[sfi] &= ~argument.bit;
        if (s.form_field < nfields &&
            !form_field_visible(def, *profile, s.form_field,
                                s.form_arg_selected[sfi])) {
          s.form_field = 0;
          while (s.form_field < nfields &&
                 !form_field_visible(def, *profile, s.form_field,
                                     s.form_arg_selected[sfi])) {
            ++s.form_field;
          }
          if (s.form_field == nfields) {
            s.form_field = nfields;
          }
        }
        s.status_msg = std::string(argument.label) + " deselected";
      } else if (state == forms::ArgumentState::kAvailable) {
        s.form_arg_selected[sfi] |= argument.bit;
        s.form_field = argument.field_index;
        s.form_col = s.form_vals[sfi][s.form_field].size();
        s.status_msg = std::string(argument.label) + " selected";
      } else {
        if (state == forms::ArgumentState::kComputed) {
          s.status_msg = std::string(argument.label) + " is computed from the selected inputs";
        } else if (state == forms::ArgumentState::kDerived) {
          s.status_msg = std::string(argument.label) + " is a derived output";
        } else {
          s.status_msg = std::string(argument.label) + " is not valid with this selection";
        }
      }
      return;
    }
    if (ch != kEsc && ch != KEY_F(1) && ch != KEY_F(2) && ch != KEY_F(3) &&
        ch != KEY_F(4) && ch != KEY_PPAGE && ch != KEY_NPAGE &&
        ch != '\n' && ch != '\r' && ch != KEY_ENTER) {
      return;
    }
  }

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
      if (dynamic) {
        do {
          --s.form_field;
        } while (s.form_field > 0 && s.form_field < nfields &&
                 !form_field_visible(s.form_defs[sfi], *profile, s.form_field,
                                     s.form_arg_selected[sfi]));
      } else {
        --s.form_field;
      }
      if (s.form_field < nfields && sfi < s.form_vals.size() &&
          s.form_field < s.form_vals[sfi].size()) {
        s.form_col = s.form_vals[sfi][s.form_field].size();
      } else {
        s.form_col = 0;
      }
    }
  } else if (ch == KEY_DOWN || ch == '\t') {
    if (s.form_field <= nfields) {
      if (dynamic) {
        do {
          ++s.form_field;
        } while (s.form_field < nfields &&
                 !form_field_visible(s.form_defs[sfi], *profile, s.form_field,
                                     s.form_arg_selected[sfi]));
      } else {
        ++s.form_field;
        if (s.form_field > nfields) {
          s.form_field = nfields;
        }
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
