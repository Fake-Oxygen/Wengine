#pragma once

/**
 * @file
 * Built-in theme palettes for the ncurses TUI (F9 settings pane). Theme
 * palettes use xterm's indexed 256-color palette; the ncurses adapter
 * (`src/tui/panes_settings.cpp` install_theme) maps these indices to the
 * best colors available on the current terminal — no hard-coded ANSI
 * escapes, nearest-color fallback on terminals with fewer colors. The
 * selected palette styles the full-screen background, title, status,
 * prompt, and semantic output highlights. Consumers: the draw layer in
 * `src/tui/`, the persistence file `wengine/tui/theme_config.hpp`, and
 * `tests/test_tui_views.cpp` (selection + color gates). ncurses-free
 * and unit-testable without a terminal.
 */

#include <array>
#include <cstddef>
#include <string_view>

namespace wengine::tui::themes {

/**
 * @brief Sentinel for "use the terminal's default color" (ncurses -1
 *        default pair semantics).
 */
inline constexpr int kDefaultColor = -1;

/**
 * @brief One foreground/background color pair in xterm indexed-color
 *        space.
 */
struct PairStyle {
  /**
   * @brief Foreground index (0..255, or `kDefaultColor`).
   */
  int foreground;
  /**
   * @brief Background index (0..255, or `kDefaultColor`).
   */
  int background;
};

/**
 * @brief One complete TUI theme: identity plus the color pair for each
 *        UI surface.
 */
struct Theme {
  /**
   * @brief Lookup key (lowercase, hyphenated; `default` aliases the
   *        terminal theme).
   */
  std::string_view name;
  /**
   * @brief Human-readable label shown in the F9 settings pane.
   */
  std::string_view label;
  /**
   * @brief One-line description shown in the F9 settings pane.
   */
  std::string_view description;
  /**
   * @brief Full-screen background pair.
   */
  PairStyle background;
  /**
   * @brief Title bar pair.
   */
  PairStyle title;
  /**
   * @brief Success-status bar pair (last exit code 0).
   */
  PairStyle success_status;
  /**
   * @brief Error-status bar pair (last exit code non-zero).
   */
  PairStyle error_status;
  /**
   * @brief PASS/OK output highlight pair.
   */
  PairStyle pass;
  /**
   * @brief FAIL/Error/WARN output highlight pair.
   */
  PairStyle fail;
  /**
   * @brief Echoed `> ` prompt-line pair.
   */
  PairStyle prompt;
};

/**
 * @brief "Terminal" theme: inherit the terminal's own colors (default
 *        pair everywhere, xterm colors 2/1 for pass/fail highlights).
 */
inline constexpr Theme kTerminal{
    "terminal", "Terminal", "Use your terminal's colors",
    {kDefaultColor, kDefaultColor}, {7, 4}, {0, 2}, {7, 1}, {2, kDefaultColor}, {1, kDefaultColor}, {6, kDefaultColor},
};

/**
 * @brief "Catppuccin Mocha" theme: dark background with mauve and blue
 *        accents.
 */
inline constexpr Theme kCatppuccinMocha{
    "catppuccin-mocha", "Catppuccin Mocha", "Mocha background with mauve and blue accents",
    {189, 235}, {189, 236}, {151, 236}, {210, 236}, {151, 235}, {210, 235}, {111, 235},
};

/**
 * @brief "Dracula" theme: dark purple with vivid pink and cyan accents.
 */
inline constexpr Theme kDracula{
    "dracula", "Dracula", "Dark purple with vivid pink and cyan accents",
    {253, 236}, {141, 236}, {84, 236}, {203, 236}, {84, 236}, {203, 236}, {117, 236},
};

/**
 * @brief "Nord" theme: cool arctic blues with muted contrast.
 */
inline constexpr Theme kNord{
    "nord", "Nord", "Cool arctic blues with muted contrast",
    {110, 235}, {110, 235}, {108, 235}, {174, 235}, {108, 235}, {174, 235}, {109, 235},
};

/**
 * @brief "Gruvbox Dark" theme: warm dark background with earthy accents.
 */
inline constexpr Theme kGruvboxDark{
    "gruvbox-dark", "Gruvbox Dark", "Warm dark background with earthy accents",
    {223, 235}, {214, 235}, {142, 235}, {167, 235}, {142, 235}, {167, 235}, {109, 235},
};

/**
 * @brief "Tokyo Night" theme: deep blue with neon lavender accents.
 */
inline constexpr Theme kTokyoNight{
    "tokyo-night", "Tokyo Night", "Deep blue with neon lavender accents",
    {189, 234}, {111, 234}, {150, 234}, {203, 234}, {150, 234}, {203, 234}, {117, 234},
};

/**
 * @brief "Light" theme: light background with high-contrast text.
 */
inline constexpr Theme kLight{
    "light", "Light", "Light background with high-contrast text",
    {239, 254}, {25, 254}, {28, 254}, {160, 254}, {28, 254}, {160, 254}, {25, 254},
};

/**
 * @brief Every built-in theme, in F9 settings-pane order.
 */
inline constexpr std::array<Theme, 7> kThemes{
    kTerminal, kCatppuccinMocha, kDracula, kNord, kGruvboxDark, kTokyoNight, kLight,
};

/**
 * @brief Look up a theme by name.
 *
 * @param[in] name Theme key as accepted by `WENGINE_TUI_THEME` and the
 *            persisted preference file.
 * @return The named built-in theme; `default` aliases `kTerminal` and
 *         unknown names fall back to `kTerminal` (the lookup never
 *         fails, so callers can render immediately).
 * @note The result references static storage; the reference stays valid
 *       for the program lifetime.
 */
[[nodiscard]] inline const Theme &find(std::string_view name) noexcept {
  if (name == "default") {
    return kTerminal;
  }
  for (const Theme &theme : kThemes) {
    if (theme.name == name) {
      return theme;
    }
  }
  return kTerminal;
}

/**
 * @brief Index of a theme by name within `kThemes`.
 *
 * @param[in] name Theme key (case-sensitive).
 * @return The index into `kThemes`; 0 (`kTerminal`) for `default` and
 *         for unknown names.
 */
[[nodiscard]] inline std::size_t index_of(std::string_view name) noexcept {
  if (name == "default") {
    return 0;
  }
  for (std::size_t i = 0; i < kThemes.size(); ++i) {
    if (kThemes[i].name == name) {
      return i;
    }
  }
  return 0;
}

/**
 * @brief Whether `name` names a selectable theme.
 *
 * @param[in] name Theme key (case-sensitive).
 * @return True for every name in `kThemes` plus the `default` alias;
 *         false otherwise (used to validate loaded preference files).
 */
[[nodiscard]] inline bool contains(std::string_view name) noexcept {
  if (name == "default") {
    return true;
  }
  for (const Theme &theme : kThemes) {
    if (theme.name == name) {
      return true;
    }
  }
  return false;
}

}  // namespace wengine::tui::themes
