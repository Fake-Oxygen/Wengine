#pragma once

// Small user-preference file for the ncurses TUI. Kept separate from the
// theme palette so persistence can be tested without starting curses.

#include "wengine/tui/theme.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

namespace wengine::tui::themes {

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

// Write to a sibling temporary file and rename it into place so an
// interrupted TUI cannot leave a partially written theme setting.
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
