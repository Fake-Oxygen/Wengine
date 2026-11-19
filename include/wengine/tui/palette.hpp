#pragma once

/**
 * @file
 * Registry-driven command palette model for the F2 palette (no ncurses
 * here, so this stays unit-testable without a terminal — see
 * `tests/test_tui_exec.cpp`). Entries derive from `CliRegistry::all()`
 * plus the REPL builtins, so new commands appear in the palette with no
 * TUI-side change. Help text itself never comes from this table: the
 * help pane runs `help <cmd>` through `TuiExecutor`, keeping it
 * byte-identical to the CLI by construction. Consumers: the F2 palette
 * and F1 context-help wiring in `src/tui/`.
 */

#include "wengine/cli/command_registry.hpp"
#include "wengine/cli/complete.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace wengine::tui {

/**
 * @brief One palette row: a command name plus its one-line description.
 */
struct PaletteEntry {
  /**
   * @brief Command name typed into the input line on Enter.
   */
  std::string name;
  /**
   * @brief One-line description shown beside the name.
   */
  std::string description;
  /**
   * @brief Whether the entry is a REPL builtin rather than a registry
   *        command.
   */
  bool builtin = false;
};

/**
 * @brief All palette entries: registry order first (mirrors completion
 *        order), then the REPL builtins.
 *
 * @return Entries for every registered command plus `help`/`exit`/`quit`.
 * @note Descriptions for builtins match `Shell`'s help; keep the list in
 *       sync with `cli::kBuiltinCommands` (pinned by `tests/test_tui_exec`).
 */
inline std::vector<PaletteEntry> palette_entries() {
  std::vector<PaletteEntry> entries;
  for (const auto &c : cli::CliRegistry::all()) {
    entries.push_back({std::string(c.name), std::string(c.description), false});
  }
  // Keep in sync with `cli::kBuiltinCommands` (pinned by test_tui_exec).
  entries.push_back({"help", "Show help", true});
  entries.push_back({"exit", "Exit shell", true});
  entries.push_back({"quit", "Exit shell", true});
  return entries;
}

/**
 * @brief Indices of entries whose name starts with `filter`.
 *
 * @param[in] entries Palette rows to filter.
 * @param[in] filter Prefix the user typed into the palette.
 * @return Indices (into `entries`) of matching rows in order; all indices
 *         when `filter` is empty.
 */
[[nodiscard]] inline std::vector<std::size_t> filter_entries(const std::vector<PaletteEntry> &entries, std::string_view filter) {
  std::vector<std::size_t> idx;
  for (std::size_t i = 0; i < entries.size(); ++i) {
    const std::string_view name = entries[i].name;
    if (filter.empty() ||
        (name.size() >= filter.size() && name.substr(0, filter.size()) == filter)) {
      idx.push_back(i);
    }
  }
  return idx;
}

/**
 * @brief First whitespace-separated token of a line.
 *
 * @param[in] line The input-line text.
 * @return The first token after leading blanks, or an empty view for a
 *         blank/whitespace-only line.
 * @note F1 context-help target: the token names the command whose help
 *       the pane should show.
 */
[[nodiscard]] inline std::string_view first_token(std::string_view line) {
  const std::size_t start = line.find_first_not_of(" \t");
  if (start == std::string_view::npos) {
    return {};
  }
  const std::size_t end = line.find_first_of(" \t", start);
  return end == std::string_view::npos ? line.substr(start) : line.substr(start, end - start);
}

} // namespace wengine::tui
