// Shared line editing for the main view and the pinned help pane (same
// keys, same history, same completion).

#include "state.hpp"

#include "wengine/cli/complete.hpp"

#include <curses.h>

#include <algorithm>

namespace wengine::tui {

bool edit_key(State &s, int edit_ch) {
  if (edit_ch == KEY_UP || edit_ch == KEY_DOWN) {
    if (!s.history.empty()) {
      if (s.hist_pos == -1) {
        s.draft = s.input;
      }
      const int count = static_cast<int>(s.history.size());
      if (edit_ch == KEY_UP) {
        s.hist_pos = (s.hist_pos == -1) ? count - 1 : std::max(0, s.hist_pos - 1);
      } else {
        s.hist_pos = (s.hist_pos == -1) ? -1 : s.hist_pos + 1;
        if (s.hist_pos >= count) {
          s.hist_pos = -1;
        }
      }
      s.input = (s.hist_pos == -1) ? s.draft : s.history.at(static_cast<std::size_t>(s.hist_pos));
      s.cursor = s.input.size();
    }
    return true;
  }
  if (edit_ch == KEY_LEFT) {
    if (s.cursor > 0) {
      --s.cursor;
    }
    return true;
  }
  if (edit_ch == KEY_RIGHT) {
    if (s.cursor < s.input.size()) {
      ++s.cursor;
    }
    return true;
  }
  if (edit_ch == KEY_HOME) {
    s.cursor = 0;
    return true;
  }
  if (edit_ch == KEY_END) {
    s.cursor = s.input.size();
    return true;
  }
  if (edit_ch == KEY_BACKSPACE || edit_ch == kBackspaceAscii || edit_ch == 8) {
    if (s.cursor > 0) {
      s.input.erase(s.cursor - 1, 1);
      --s.cursor;
      s.hist_pos = -1;
    }
    return true;
  }
  if (edit_ch == KEY_DC) {
    if (s.cursor < s.input.size()) {
      s.input.erase(s.cursor, 1);
      s.hist_pos = -1;
    }
    return true;
  }
  if (edit_ch == kCtrlU || edit_ch == kCtrlC) {
    s.input.clear();
    s.cursor = 0;
    s.hist_pos = -1;
    return true;
  }
  if (edit_ch == '\t') {
    const std::vector<std::string> cand = s.exec.complete(s.input, s.cursor);
    if (cand.empty()) {
      s.complete_names.clear();
      s.status_msg = "no completions";
    } else if (cand.size() == 1) {
      std::size_t begin = 0;
      std::size_t end = 0;
      token_bounds(s.input, s.cursor, begin, end);
      s.input.replace(begin, end - begin, cand[0]);
      s.cursor = begin + cand[0].size();
      if (s.cursor == s.input.size()) {
        s.input.push_back(' ');
        ++s.cursor;
      }
      s.hist_pos = -1;
      s.complete_names.clear();
      s.status_msg = "completed";
    } else {
      const std::string pre = cli::common_prefix(cand);
      std::size_t begin = 0;
      std::size_t end = 0;
      token_bounds(s.input, s.cursor, begin, end);
      const std::string cur_tok = s.input.substr(begin, end - begin);
      if (pre.size() > cur_tok.size()) {
        s.input.replace(begin, end - begin, pre);
        s.cursor = begin + pre.size();
        s.hist_pos = -1;
        s.complete_names.clear();
        s.status_msg = std::to_string(cand.size()) + " candidates -- prefix extended";
      } else {
        // Multi-candidate list goes to the transient popup above the
        // input line, never into the scrollback.
        s.complete_names = cand;
        s.status_msg = std::to_string(cand.size()) + " completions above -- keep typing to dismiss";
      }
    }
    return true;
  }
  if (edit_ch >= 32 && edit_ch < 127) {
    s.input.insert(s.cursor, 1, static_cast<char>(edit_ch));
    ++s.cursor;
    s.hist_pos = -1;
    return true;
  }
  return false;
}

} // namespace wengine::tui
