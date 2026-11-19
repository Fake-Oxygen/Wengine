#pragma once

/**
 * @file
 * Output views for the TUI (docs/tui.md TUI-6): PASS/FAIL plus Error
 * highlighting, scrollback search, save-pane-to-file, solid resize
 * reflow, dumb-terminal refusal plus NO_COLOR fallback, and a visible
 * scroll position.
 *
 * All helpers are ncurses-free and operate on captured CLI text only —
 * there is no per-command view code by construction (D-TUI-1 holds: every
 * pane renders `cli::Shell::execute` output verbatim). The ncurses layer
 * in `src/tui/` owns only attribute application plus key handling; this
 * header owns classification, search, wrapping, scroll math, and saving
 * so everything stays unit-testable (see `tests/test_tui_views.cpp`).
 * Theme settings (TUI-8) persist through `wengine/tui/theme_config.hpp`;
 * TUI chrome strings stay ASCII-only (plain ncurses renders non-ASCII
 * bytes as control notations and miscounts their width), so wrapping math
 * here may assume one byte equals one display cell.
 */

#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wengine::tui::views {

/**
 * @brief ncurses color-pair id for PASS/OK verdict lines (green text).
 *
 * @note Color-pair ids for highlighted output lines (plus the title/status
 *       pairs 1..3 owned by `src/tui/run.cpp` and the background pair 7
 *       plus dialog pairs 8..10 owned by `install_theme` in
 *       `src/tui/panes_settings.cpp`): the draw layer installs these when
 *       color is allowed; the model never touches ncurses.
 */
inline constexpr int kPassPair = 4;    // green text: PASS / OK verdicts
/**
 * @brief ncurses color-pair id for FAIL/Error/WARN lines (red text).
 */
inline constexpr int kFailPair = 5;    // red text: FAIL / Error / WARN
/**
 * @brief ncurses color-pair id for echoed `> ` input lines (cyan text).
 */
inline constexpr int kPromptPair = 6;  // cyan text: echoed `> ` input lines
/**
 * @brief ncurses color-pair id for the floating-panel body (theme accent
 *        block).
 */
inline constexpr int kDialogPair = 8;  // floating-panel body (theme accent block)
/**
 * @brief ncurses color-pair id for the selected option (inverted accent
 *        block).
 */
inline constexpr int kDialogSelPair = 9;  // selected option (inverted accent block)
/**
 * @brief ncurses color-pair id for the drop shadow (solid black block).
 */
inline constexpr int kDialogShadowPair = 10;  // drop shadow (solid black block)

/**
 * @brief One captured-text line's highlight class.
 *
 * @note Classification is purely textual (substring match, no command
 *       knowledge) so new solvers need no view-side change: kNormal is
 *       plain output; kPrompt is an echoed input line; kPass covers
 *       PASS/OK verdicts; kFail covers FAIL verdicts; kError covers
 *       Error/WARN/Unknown-command lines; kTruncation marks the
 *       cap note. Usage doc lines mentioning PASS/FAIL together stay
 *       unhighlighted.
 */
enum class LineKind : int { kNormal = 0, kPrompt, kPass, kFail, kError, kTruncation };

/**
 * @brief Classify one captured line for highlighting.
 *
 * @param[in] line One logical scrollback line.
 * @return The highlight class: kPrompt for echoed `> ` input, kTruncation
 *         for the cap note, kError for Error/WARN/Unknown-command lines,
 *         kFail for FAIL verdicts, kPass for PASS/OK verdicts, else
 *         kNormal (documentation lines mentioning PASS/FAIL together are
 *         not verdicts).
 */
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

/**
 * @brief ncurses color pair for a highlight class.
 *
 * @param[in] kind Highlight class to render.
 * @return The pair id for kPass/kFail/kError/kPrompt; 0 (default
 *         attributes) for kNormal/kTruncation.
 */
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

/**
 * @brief Color gate for unit tests.
 *
 * Color gate for unit tests: TERM dumb (or empty/missing) and NO_COLOR
 * disable colors even when the terminal reports color support.
 *
 * @param[in] term Terminal type (the TERM value).
 * @param[in] no_color_set True when NO_COLOR is set in the environment.
 * @return False on minimal terminals and under NO_COLOR; true otherwise.
 * @note NO_COLOR still runs full-screen, only without colors.
 */
[[nodiscard]] inline bool colors_allowed(std::string_view term, bool no_color_set) noexcept {
  if (no_color_set) {
    return false;
  }
  if (term.empty() || term == "dumb" || term == "unknown") {
    return false;
  }
  return true;
}

/**
 * @brief Full-screen gate: minimal terminals without cursor addressing
 *        cannot host the pane layout.
 *
 * Full-screen gate: minimal terminals without cursor addressing cannot
 * host the pane layout (verified via PTY: `cup`/`el` are no-ops, function
 * keys are undefined, flow-control swallows Ctrl-Q). The TUI refuses
 * loudly there instead of garbling; NO_COLOR alone still runs
 * full-screen.
 *
 * @param[in] term Terminal type (the TERM value).
 * @return False for empty/dumb/unknown terminals; true otherwise.
 */
[[nodiscard]] inline bool full_screen_supported(std::string_view term) noexcept {
  if (term.empty() || term == "dumb" || term == "unknown") {
    return false;
  }
  return true;
}

/**
 * @brief Runtime color gate: reads TERM plus NO_COLOR from the
 *        environment.
 *
 * @return The `colors_allowed` verdict for the live environment.
 */
[[nodiscard]] inline bool colors_allowed_env() {
  const char *term = std::getenv("TERM");
  const char *no_color = std::getenv("NO_COLOR");
  const std::string_view term_view = term != nullptr ? std::string_view(term) : std::string_view{};
  return colors_allowed(term_view, no_color != nullptr);
}

/**
 * @brief Indices of logical lines containing `query`.
 *
 * Indices of logical lines containing `query` (case-sensitive
 * substring). Empty queries match nothing so `/` plus Enter never
 * highlights the world.
 *
 * @param[in] lines Logical scrollback lines to search.
 * @param[in] query Search text from the status-line prompt.
 * @return Indices (into `lines`) of matching rows in order; empty when
 *         the query is empty or absent.
 */
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

/**
 * @brief All non-overlapping match spans of `query` inside one line (for
 *        reverse-video match highlighting).
 *
 * @param[in] line One logical scrollback line.
 * @param[in] query Search text from the status-line prompt.
 * @return (offset, length) spans of each non-overlapping occurrence;
 *         empty when the query is empty or absent.
 */
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

/**
 * @brief Wrapped display model: each logical line hard-wraps at `width`
 *        cells.
 *
 * Wrapped display model: each logical line hard-wraps at `width` cells
 * (ASCII-only CLI output, so byte == cell). Empty logical lines stay one
 * empty display row so blank lines survive. `width` below 1 is clamped
 * to 1 so resize can never corrupt panes.
 */
struct Wrapped {
  /**
   * @brief Display rows in order (hard-wrapped logical lines).
   */
  std::vector<std::string> rows;
  /**
   * @brief Logical line index for each display row.
   */
  std::vector<std::size_t> logical_for_row;
  /**
   * @brief First display row for each logical line.
   */
  std::vector<std::size_t> row_start_for_logical;
};

/**
 * @brief Hard-wrap logical lines into display rows.
 *
 * @param[in] logical Logical scrollback lines.
 * @param[in] width Wrap width in display cells (clamped to at least 1).
 * @return The wrapped rows plus both index maps.
 */
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

/**
 * @brief Clamp a scroll offset to the valid range.
 *
 * Scroll math lives in display-row space so wrapped lines reflow on
 * resize: `scroll` counts display rows above the follow position
 * (0 = follow/tail). `page` is the visible row budget.
 *
 * @param[in] scroll Requested scroll offset in display rows.
 * @param[in] total Total display rows.
 * @param[in] page Visible row budget.
 * @return The clamped offset (0 when everything fits or the page is
 *         empty).
 */
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

/**
 * @brief First visible display row for a scroll offset.
 *
 * @param[in] total Total display rows.
 * @param[in] page Visible row budget.
 * @param[in] scroll Clamped scroll offset in display rows.
 * @return Index of the first visible display row (0 when everything
 *         fits).
 */
[[nodiscard]] inline std::size_t display_first(std::size_t total, std::size_t page, int scroll) noexcept {
  if (total <= page || page == 0) {
    return 0;
  }
  const int s = clamp_scroll(scroll, total, page);
  return total - page - static_cast<std::size_t>(s);
}

/**
 * @brief Scroll offset that brings display row `target` on screen.
 *
 * Scroll offset that brings display row `target` on screen (parks it at
 * the top when there is room above, otherwise tails the bottom).
 *
 * @param[in] target Display row to reveal (e.g. a search hit).
 * @param[in] total Total display rows.
 * @param[in] page Visible row budget.
 * @return The scroll offset showing `target` (0 when everything fits or
 *         the target is out of range).
 */
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

/**
 * @brief Visible scroll position for the status bar.
 *
 * Visible scroll position for the status bar, e.g. "12-30/120 (25%)".
 * Empty logs report "0/0".
 *
 * @param[in] first Index of the first visible display row.
 * @param[in] visible Number of visible display rows.
 * @param[in] total Total display rows.
 * @return The "first-last/total (pct)" indicator text.
 */
[[nodiscard]] inline std::string scroll_indicator(std::size_t first, std::size_t visible, std::size_t total) {
  if (total == 0 || visible == 0) {
    return "0/0";
  }
  const std::size_t last = first + visible < total ? first + visible : total;
  const std::size_t pct = (last * 100) / total;
  return std::to_string(first + 1) + "-" + std::to_string(last) + "/" + std::to_string(total) + " (" + std::to_string(pct) + "%)";
}

/**
 * @brief Save one pane's logical lines to `path`.
 *
 * Save one pane's logical lines to `path` (one newline per line, trailing
 * newline when non-empty). Returns false plus `error` on open/write
 * failure; never throws for the TUI key path.
 *
 * @param[in] lines Logical scrollback lines to write.
 * @param[in] path Destination file path.
 * @param[out] error Human-readable failure reason on false.
 * @return True when every line landed on disk; false with `error` set on
 *         open or write failure.
 */
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
