#pragma once

/**
 * @file
 * Small user-preference file for the ncurses TUI theme: resolves the
 * config path, loads the saved theme name, and saves the F9 selection as
 * the startup default. Kept separate from the theme palette
 * (`wengine/tui/theme.hpp`) so persistence can be tested without starting
 * curses. The file lives at `$XDG_CONFIG_HOME/wengine/tui-theme`
 * (fallback `~/.config/wengine/tui-theme`). Consumers: the F9 apply path
 * and startup in `src/tui/`, `tests/test_tui_views.cpp`.
 */

#include "wengine/tui/theme.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

namespace wengine::tui::themes {

/**
 * @brief Resolve the theme preference file path.
 *
 * @return `$XDG_CONFIG_HOME/wengine/tui-theme` when `XDG_CONFIG_HOME` is
 *         set and non-empty, else `$HOME/.config/wengine/tui-theme`;
 *         empty when neither variable is set (persistence disabled).
 */
[[nodiscard]] inline std::string preference_path() {
  const char *xdg = std::getenv("XDG_CONFIG_HOME");
  if (xdg != nullptr && *xdg != '\0') {
    return (std::filesystem::path{xdg} / "wengine" / "tui-theme").string();
  }
  const char *home = std::getenv("HOME");
  if (home != nullptr && *home != '\0') {
    return (std::filesystem::path{home} / ".config" / "wengine" / "tui-theme").string();
  }
  return {};
}

/**
 * @brief Read the saved startup theme name.
 *
 * @param[in] path Preference path (from `preference_path()`).
 * @return The saved theme name when the file exists and the name is a
 *         known theme; empty on any failure (missing/unreadable file,
 *         unknown name) — callers treat empty as "use the terminal
 *         theme".
 */
[[nodiscard]] inline std::string load_preference(const std::string &path) {
  if (path.empty()) {
    return {};
  }
  std::ifstream in(path);
  std::string name;
  if (!in || !std::getline(in, name) || !contains(name)) {
    return {};
  }
  return name;
}

/**
 * @brief Persist the selected theme as the startup default.
 *
 * @param[in] path Preference path (from `preference_path()`).
 * @param[in] name Theme key to save (must be a known theme).
 * @param[out] error Human-readable failure reason on false.
 * @return True when the name was saved; false with `error` set when
 *         there is no config directory, the name is unknown, the
 *         directory cannot be created, or the write/rename fails.
 * @note Writes to a sibling temporary file and renames it into place so
 *       an interrupted TUI cannot leave a partially written theme
 *       setting.
 */
inline bool save_preference(const std::string &path, std::string_view name, std::string &error) {
  if (path.empty()) {
    error = "no config directory (set XDG_CONFIG_HOME or HOME)";
    return false;
  }
  if (!contains(name)) {
    error = "unknown theme '" + std::string(name) + "'";
    return false;
  }

  const std::filesystem::path target{path};
  const std::filesystem::path parent = target.parent_path();
  std::error_code ec;
  if (!parent.empty()) {
    std::filesystem::create_directories(parent, ec);
  }
  if (ec) {
    error = "cannot create config directory '" + parent.string() + "'";
    return false;
  }

  const std::filesystem::path temporary = target.string() + ".tmp";
  {
    std::ofstream out(temporary, std::ios::out | std::ios::trunc);
    if (!out) {
      error = "cannot write theme preference '" + temporary.string() + "'";
      return false;
    }
    out << name << '\n';
    out.flush();
    if (!out) {
      error = "cannot write theme preference '" + temporary.string() + "'";
      return false;
    }
  }

  std::filesystem::rename(temporary, target, ec);
  if (ec) {
    error = "cannot replace theme preference '" + path + "'";
    return false;
  }
  return true;
}

}  // namespace wengine::tui::themes
