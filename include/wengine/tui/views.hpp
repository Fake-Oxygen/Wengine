#pragma once

// Output views for the TUI (TUI-6): PASS/FAIL + Error highlighting,
// scrollback search, save-pane-to-file, solid resize reflow, TERM=dumb /
// NO_COLOR fallback, and a visible scroll position.
//
// All helpers are ncurses-free and operate on captured CLI text only —
// there is no per-command view code by construction (D-TUI-1 holds: every
// pane renders `cli::Shell::execute` output verbatim). The ncurses layer
// in `src/tui/` owns only attribute application + key handling; this
// header owns classification, search, wrapping, scroll math, and saving
// so everything stays unit-testable (see `tests/test_tui_views.cpp`).

#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wengine::tui::views {

// Color-pair ids for highlighted output lines (plus the title/status pairs
// 1..3 owned by `src/tui/run.cpp`). The draw layer installs these when
// `use_color` is true; the model never touches ncurses.
inline constexpr int kPassPair = 4;    // green text: PASS / OK verdicts
inline constexpr int kFailPair = 5;    // red text: FAIL / Error / WARN
inline constexpr int kPromptPair = 6;  // cyan text: echoed `> ` input lines

// One captured-text line's highlight class. Classification is purely
// textual (substring match, no command knowledge) so new solvers need no
// view-side change.
enum class LineKind : int { kNormal = 0, kPrompt, kPass, kFail, kError, kTruncation };

[[nodiscard]] inline LineKind classify_line(std::string_view line) noexcept {
  if (line.size() >= 2 && line[0] == '>' && line[1] == ' ') {
    return LineKind::kPrompt;
  }
  if (line.size() >= 4 && line.substr(0, 4) == "[...") {
    return LineKind::kTruncation;
  }
  // Documentation mentions ("PASS/FAIL" in usage text) are not verdicts.
  if (line.find("PASS/FAIL") != std::string_view::npos) {
    return LineKind::kNormal;
  }
  if (line.find("Error:") != std::string_view::npos || line.find("WARN") != std::string_view::npos ||
      line.find("Unknown command") != std::string_view::npos) {
    return LineKind::kError;
  }
  if (line.find("FAIL") != std::string_view::npos) {
    return LineKind::kFail;
  }
  if (line.find("PASS") != std::string_view::npos || line.find(" OK") != std::string_view::npos) {
    return LineKind::kPass;
  }
  return LineKind::kNormal;
}

// ncurses color pair for a kind (0 = default attributes).
[[nodiscard]] inline int color_pair_for(LineKind kind) noexcept {
  switch (kind) {
    case LineKind::kPass:
      return kPassPair;
    case LineKind::kFail:
    case LineKind::kError:
      return kFailPair;
    case LineKind::kPrompt:
      return kPromptPair;
    case LineKind::kNormal:
    case LineKind::kTruncation:
      return 0;
  }
  return 0;
}

// Color gate for unit tests: TERM=dumb (or empty/missing) and NO_COLOR
// disable colors even when the terminal reports color support.
[[nodiscard]] inline bool colors_allowed(std::string_view term, bool no_color_set) noexcept {
  if (no_color_set) {
    return false;
  }
  if (term.empty() || term == "dumb" || term == "unknown") {
    return false;
  }
  return true;
}

// Full-screen gate: minimal terminals without cursor addressing cannot host
// the pane layout (verified via PTY: `cup`/`el` are no-ops, function keys
// are undefined, flow-control swallows Ctrl-Q). The TUI refuses loudly
// there instead of garbling; NO_COLOR alone still runs full-screen.
[[nodiscard]] inline bool full_screen_supported(std::string_view term) noexcept {
  if (term.empty() || term == "dumb" || term == "unknown") {
    return false;
  }
  return true;
}

// Runtime gate: reads TERM + NO_COLOR from the environment.
[[nodiscard]] inline bool colors_allowed_env() {
  const char *term = std::getenv("TERM");
  const char *no_color = std::getenv("NO_COLOR");
  const std::string_view term_view = term != nullptr ? std::string_view(term) : std::string_view{};
  return colors_allowed(term_view, no_color != nullptr);
}

// Indices of logical lines containing `query` (case-sensitive substring).
// Empty queries match nothing so `/` + Enter never highlights the world.
[[nodiscard]] inline std::vector<std::size_t> find_matches(const std::vector<std::string> &lines, std::string_view query) {
  std::vector<std::size_t> out;
  if (query.empty()) {
    return out;
  }
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (lines[i].find(query) != std::string::npos) {
      out.push_back(i);
    }
  }
  return out;
}

// All non-overlapping match spans of `query` inside one line (for
// reverse-video match highlighting). Empty when absent/empty.
[[nodiscard]] inline std::vector<std::pair<std::size_t, std::size_t>> match_spans(std::string_view line, std::string_view query) {
  std::vector<std::pair<std::size_t, std::size_t>> out;
  if (query.empty()) {
    return out;
  }
  std::size_t pos = 0;
  while (pos <= line.size()) {
    const std::size_t at = line.find(query, pos);
    if (at == std::string_view::npos) {
      break;
    }
    out.emplace_back(at, query.size());
    pos = at + (query.empty() ? 1 : query.size());
  }
  return out;
}

// Wrapped display model: each logical line hard-wraps at `width` cells
// (ASCII-only CLI output, so byte == cell). Empty logical lines stay one
// empty display row so blank lines survive. `width < 1` is clamped to 1
// so resize can never corrupt panes.
struct Wrapped {
  std::vector<std::string> rows;
  std::vector<std::size_t> logical_for_row;
  std::vector<std::size_t> row_start_for_logical;
};

[[nodiscard]] inline Wrapped wrap_lines(const std::vector<std::string> &logical, std::size_t width) {
  Wrapped w;
  if (width < 1) {
    width = 1;
  }
  w.row_start_for_logical.resize(logical.size());
  for (std::size_t li = 0; li < logical.size(); ++li) {
    w.row_start_for_logical[li] = w.rows.size();
    const std::string &line = logical[li];
    if (line.empty()) {
      w.rows.emplace_back();
      w.logical_for_row.push_back(li);
      continue;
    }
    std::size_t pos = 0;
    while (pos < line.size()) {
      const std::size_t n = (line.size() - pos > width) ? width : line.size() - pos;
      w.rows.push_back(line.substr(pos, n));
      w.logical_for_row.push_back(li);
      pos += n;
    }
  }
  return w;
}

// Scroll math lives in display-row space so wrapped lines reflow on
// resize: `scroll` counts display rows above the follow position
// (0 = follow/tail). `page` is the visible row budget.
[[nodiscard]] inline int clamp_scroll(int scroll, std::size_t total, std::size_t page) noexcept {
  if (total <= page || page == 0) {
    return 0;
  }
  const int max_scroll = static_cast<int>(total - page);
  if (scroll < 0) {
    return 0;
  }
  if (scroll > max_scroll) {
    return max_scroll;
  }
  return scroll;
}

[[nodiscard]] inline std::size_t display_first(std::size_t total, std::size_t page, int scroll) noexcept {
  if (total <= page || page == 0) {
    return 0;
  }
  const int s = clamp_scroll(scroll, total, page);
  return total - page - static_cast<std::size_t>(s);
}

// Scroll offset that brings display row `target` on screen (parks it at
// the top when there is room above, otherwise tails the bottom).
[[nodiscard]] inline int scroll_to_show(std::size_t target, std::size_t total, std::size_t page) noexcept {
  if (total <= page || page == 0) {
    return 0;
  }
  if (target >= total) {
    return 0;
  }
  const std::size_t max_first = total - page;
  const std::size_t first = target < max_first ? target : max_first;
  return static_cast<int>(max_first - first);
}

// Visible scroll position for the status bar, e.g. "12-30/120 (25%)".
// Empty logs report "0/0".
[[nodiscard]] inline std::string scroll_indicator(std::size_t first, std::size_t visible, std::size_t total) {
  if (total == 0 || visible == 0) {
    return "0/0";
  }
  const std::size_t last = first + visible < total ? first + visible : total;
  const std::size_t pct = (last * 100) / total;
  return std::to_string(first + 1) + "-" + std::to_string(last) + "/" + std::to_string(total) + " (" + std::to_string(pct) + "%)";
}

// Save one pane's logical lines to `path` (one `\n` per line, trailing
// newline when non-empty). Returns false + `error` on open/write failure;
// never throws for the TUI key path.
inline bool save_lines(const std::vector<std::string> &lines, const std::string &path, std::string &error) {
  std::ofstream out(path, std::ios::out | std::ios::trunc);
  if (!out) {
    error = "cannot open '" + path + "'";
    return false;
  }
  for (const auto &l : lines) {
    out << l << '\n';
    if (!out) {
      error = "write failed '" + path + "'";
      return false;
    }
  }
  out.flush();
  if (!out) {
    error = "write failed '" + path + "'";
    return false;
  }
  return true;
}

}  // namespace wengine::tui::views
