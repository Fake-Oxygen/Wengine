#pragma once

// Theme palettes use xterm's indexed 256-color palette. The ncurses adapter
// maps these indices to the best colors available on the current terminal.

#include <array>
#include <cstddef>
#include <string_view>

namespace wengine::tui::themes {

inline constexpr int kDefaultColor = -1;

struct PairStyle {
  int foreground;
  int background;
};

struct Theme {
  std::string_view name;
  std::string_view label;
  std::string_view description;
  PairStyle background;
  PairStyle title;
  PairStyle success_status;
  PairStyle error_status;
  PairStyle pass;
  PairStyle fail;
  PairStyle prompt;
};

inline constexpr Theme kTerminal{
    "terminal", "Terminal", "Use your terminal's colors",
    {kDefaultColor, kDefaultColor}, {7, 4}, {0, 2}, {7, 1}, {2, kDefaultColor}, {1, kDefaultColor}, {6, kDefaultColor},
};

inline constexpr Theme kCatppuccinMocha{
    "catppuccin-mocha", "Catppuccin Mocha", "Mocha background with mauve and blue accents",
    {189, 235}, {189, 236}, {151, 236}, {210, 236}, {151, 235}, {210, 235}, {111, 235},
};

inline constexpr Theme kDracula{
    "dracula", "Dracula", "Dark purple with vivid pink and cyan accents",
    {253, 236}, {141, 236}, {84, 236}, {203, 236}, {84, 236}, {203, 236}, {117, 236},
};

inline constexpr Theme kNord{
    "nord", "Nord", "Cool arctic blues with muted contrast",
    {110, 235}, {110, 235}, {108, 235}, {174, 235}, {108, 235}, {174, 235}, {109, 235},
};

inline constexpr Theme kGruvboxDark{
    "gruvbox-dark", "Gruvbox Dark", "Warm dark background with earthy accents",
    {223, 235}, {214, 235}, {142, 235}, {167, 235}, {142, 235}, {167, 235}, {109, 235},
};

inline constexpr Theme kTokyoNight{
    "tokyo-night", "Tokyo Night", "Deep blue with neon lavender accents",
    {189, 234}, {111, 234}, {150, 234}, {203, 234}, {150, 234}, {203, 234}, {117, 234},
};

inline constexpr Theme kLight{
    "light", "Light", "Light background with high-contrast text",
    {239, 254}, {25, 254}, {28, 254}, {160, 254}, {28, 254}, {160, 254}, {25, 254},
};

inline constexpr std::array<Theme, 7> kThemes{
    kTerminal, kCatppuccinMocha, kDracula, kNord, kGruvboxDark, kTokyoNight, kLight,
};

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
