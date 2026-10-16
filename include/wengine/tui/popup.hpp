#pragma once

// Transient Tab-completion popup model (no ncurses here, unit-testable).
//
// The popup shows completion candidates just above the input line instead
// of dumping them into the scrollback. It is transient by contract: any
// key other than Tab dismisses it (owned by the run loop, never stored).

#include <cstddef>
#include <string>
#include <vector>

namespace wengine::tui {

// Greedily wrap `names` into at most `max_rows` display rows of `width`
// cells. Overflow ends the last row with a loud "+N more" note instead of
// silently dropping names. Empty when there is nothing to show.
[[nodiscard]] inline std::vector<std::string> wrap_popup_lines(const std::vector<std::string> &names, std::size_t width, std::size_t max_rows) {
  std::vector<std::string> rows;
  if (max_rows == 0 || width == 0 || names.empty()) {
    return rows;
  }
  std::string cur;
  std::size_t shown = 0;
  bool capped = false;
  for (const auto &name : names) {
    if (!cur.empty() && cur.size() + 1 + name.size() > width) {
      rows.push_back(cur);
      cur.clear();
      if (rows.size() == max_rows) {
        capped = true;
        break;
      }
    }
    if (!cur.empty()) {
      cur.push_back(' ');
    }
    cur += name;
    ++shown;
  }
  if (capped) {
    rows.back() += " ... (+" + std::to_string(names.size() - shown) + " more)";
  } else if (!cur.empty()) {
    rows.push_back(cur);
  }
  return rows;
}

} // namespace wengine::tui
