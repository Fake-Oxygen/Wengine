// Theme settings pane and ncurses palette installation.

#include "state.hpp"

#include "wengine/tui/theme_config.hpp"
#include "wengine/tui/views.hpp"

#include <curses.h>

#include <algorithm>
#include <array>
#include <cstddef>

namespace wengine::tui {

namespace {

void commit_theme(State &s) {
  s.theme_index = s.settings_selection;
  std::string error;
  if (themes::save_preference(s.theme_preference_path, themes::kThemes[s.theme_index].name, error)) {
    s.status_msg = "theme saved: " + std::string(themes::kThemes[s.theme_index].label);
  } else {
    s.status_msg = "theme active, but could not save: " + error;
  }
  s.overlay = s.settings_return_overlay;
}

struct Rgb {
  int r;
  int g;
  int b;
};

[[nodiscard]] Rgb xterm_rgb(int color) noexcept {
  constexpr std::array<Rgb, 16> kAnsi{{
      {0, 0, 0}, {205, 0, 0}, {0, 205, 0}, {205, 205, 0},
      {0, 0, 238}, {205, 0, 205}, {0, 205, 205}, {229, 229, 229},
      {127, 127, 127}, {255, 0, 0}, {0, 255, 0}, {255, 255, 0},
      {92, 92, 255}, {255, 0, 255}, {0, 255, 255}, {255, 255, 255},
  }};
  if (color < 16) {
    return kAnsi[static_cast<std::size_t>(color)];
  }
  if (color < 232) {
    constexpr std::array<int, 6> kLevels{0, 95, 135, 175, 215, 255};
    const int index = color - 16;
    return {kLevels[static_cast<std::size_t>(index / 36)],
            kLevels[static_cast<std::size_t>((index / 6) % 6)],
            kLevels[static_cast<std::size_t>(index % 6)]};
  }
  const int gray = 8 + (color - 232) * 10;
  return {gray, gray, gray};
}

[[nodiscard]] short terminal_color(int color) noexcept {
  if (color < COLORS) {
    return static_cast<short>(color);
  }
  const Rgb wanted = xterm_rgb(color);
  const int candidates = std::min(COLORS, 16);
  int best = 0;
  int best_distance = 3 * 256 * 256;
  for (int i = 0; i < candidates; ++i) {
    const Rgb candidate = xterm_rgb(i);
    const int dr = wanted.r - candidate.r;
    const int dg = wanted.g - candidate.g;
    const int db = wanted.b - candidate.b;
    const int distance = dr * dr + dg * dg + db * db;
    if (distance < best_distance) {
      best = i;
      best_distance = distance;
    }
  }
  return static_cast<short>(best);
}

}  // namespace

void install_theme(const themes::Theme &theme, bool default_colors) {
  const auto init = [default_colors](short pair, themes::PairStyle style) {
    const short foreground = style.foreground == themes::kDefaultColor
                                 ? static_cast<short>(default_colors ? -1 : COLOR_WHITE)
                                 : terminal_color(style.foreground);
    const short background = style.background == themes::kDefaultColor
                                 ? static_cast<short>(default_colors ? -1 : COLOR_BLACK)
                                 : terminal_color(style.background);
    (void)init_pair(pair, foreground, background);
  };
  init(1, theme.title);
  init(2, theme.success_status);
  init(3, theme.error_status);
  init(views::kPassPair, theme.pass);
  init(views::kFailPair, theme.fail);
  init(views::kPromptPair, theme.prompt);
  init(7, theme.background);
  if (COLOR_PAIRS > views::kDialogShadowPair) {
    // Floating choice-dialog styling, derived from the theme so every
    // palette gets a matching modal: solid accent body, inverted-accent
    // selection, black drop shadow. Chrome stays ASCII (spaces only), so
    // this is exact on any locale; no-color terminals fall back to
    // reverse-video blocks in the draw layer.
    init(views::kDialogPair, theme.title);
    init(views::kDialogSelPair, {theme.title.background, theme.title.foreground});
    const auto shadow = [](short pair) {
      (void)init_pair(pair, COLOR_BLACK, COLOR_BLACK);
    };
    shadow(views::kDialogShadowPair);
  }
  (void)bkgd(COLOR_PAIR(7));
}

void draw_settings_pane(State &s) {
  const int cols = s.cols;
  (void)mvaddnstr(1, 0, "Settings -- choose a color theme | Up/Down: preview | Enter: apply | Esc: cancel", cols - 1);
  (void)wclrtoeol(stdscr);
  const std::string active = "Active: " + std::string(themes::kThemes[s.theme_index].label);
  (void)mvaddnstr(2, 0, active.c_str(), cols - 1);
  (void)wclrtoeol(stdscr);

  const int page = s.out_rows > 4 ? s.out_rows - 4 : 0;
  const std::size_t first = page > 0 && s.settings_selection >= static_cast<std::size_t>(page)
                                ? s.settings_selection - static_cast<std::size_t>(page) + 1 : 0;
  for (int i = 0; i < page; ++i) {
    const std::size_t index = first + static_cast<std::size_t>(i);
    if (index >= themes::kThemes.size()) {
      break;
    }
    const themes::Theme &theme = themes::kThemes[index];
    const std::string row = std::string(index == s.theme_index ? "  [*] " : "  [ ] ") + std::string(theme.label) + " - " + std::string(theme.description);
    if (index == s.settings_selection) {
      (void)attron(A_REVERSE);
    }
    (void)mvaddnstr(4 + i, 0, row.c_str(), cols - 1);
    (void)wclrtoeol(stdscr);
    if (index == s.settings_selection) {
      (void)attroff(A_REVERSE);
    }
  }
}

void settings_key(State &s, int ch) {
  if (ch == kEsc || ch == kCtrlC) {
    s.theme_index = s.settings_original_theme;
    if (s.use_color) {
      install_theme(themes::kThemes[s.theme_index], s.default_colors);
    }
    s.overlay = s.settings_return_overlay;
    s.status_msg = "theme preview cancelled";
  } else if (ch == KEY_F(9)) {
    commit_theme(s);
  } else if (ch == KEY_UP || ch == KEY_DOWN) {
    if (ch == KEY_UP && s.settings_selection > 0) {
      --s.settings_selection;
    } else if (ch == KEY_DOWN && s.settings_selection + 1 < themes::kThemes.size()) {
      ++s.settings_selection;
    }
    s.theme_index = s.settings_selection;
    if (s.use_color) {
      install_theme(themes::kThemes[s.theme_index], s.default_colors);
    }
  } else if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
    commit_theme(s);
  }
}

}  // namespace wengine::tui
