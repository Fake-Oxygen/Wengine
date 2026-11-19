#pragma once

/**
 * @file
 * Scalable enum-choice dialog model for the TUI forms/builders
 * (ncurses-free, unit-testable without a terminal). The TUI stays a
 * pure front end: this model stores dialog state only — picking a value
 * writes the picked string back into a form/builder field, and the CLI
 * still owns every verdict on the composed line.
 *
 * Enum-valued fields declare their options once in the form/builder
 * definition (`FormField::choices` / `BatchField::choices` for static
 * vocabularies, plus `form_field_choices` / `batch_field_choices` for
 * registry-derived or value-dependent ones). The ncurses layer only
 * calls into this model: Space on a field with choices opens the
 * dialog, the dialog owns Up/Down/Enter/Esc, and confirm writes the
 * picked string back into the field value. New commands/arguments get
 * the dialog for free by declaring `choices` — no pane logic changes
 * (pinned by `tests/test_tui_choice.cpp`, including a synthetic future
 * enum field).
 */

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace wengine::tui {

/**
 * @brief State of the Space-opened enum-choice dialog (one dialog at a
 *        time, owned by the active form/builder).
 */
struct ChoiceDialog {
  /**
   * @brief Whether the dialog is currently shown.
   */
  bool open = false;
  /**
   * @brief Dialog caption (usually the field label).
   */
  std::string title;
  /**
   * @brief Pickable option strings (never empty while open).
   */
  std::vector<std::string> options;
  /**
   * @brief Highlighted option index into `options`.
   */
  std::size_t selected = 0;
  /**
   * @brief Target field index in the active form/builder whose value the
   *        confirmed pick updates.
   */
  std::size_t field = 0; // target field index in the active form/builder
};

/**
 * @brief Open the dialog for `field` with `options`.
 *
 * @param[out] dialog Dialog state to (re)initialize.
 * @param[in] title Dialog caption.
 * @param[in] options Pickable strings; the call is refused when empty.
 * @param[in] field Target field index in the active form/builder.
 * @param[in] current Current field value, used to place the initial
 *            highlight.
 * @return True when the dialog opened; false (stays closed) when there
 *         are no options.
 * @note The initial highlight tracks `current` when it matches an
 *       option, else the first option.
 */
[[nodiscard]] inline bool open_choice(ChoiceDialog &dialog, std::string_view title,
                                      const std::vector<std::string> &options,
                                      std::size_t field, std::string_view current) {
  if (options.empty()) {
    dialog.open = false;
    return false;
  }
  dialog.open = true;
  dialog.title = std::string(title);
  dialog.options = options;
  dialog.field = field;
  dialog.selected = 0;
  for (std::size_t i = 0; i < options.size(); ++i) {
    if (options[i] == current) {
      dialog.selected = i;
      break;
    }
  }
  return true;
}

/**
 * @brief Move the dialog highlight (Up/Down arrow keys).
 *
 * @param[in,out] dialog Open dialog; no-op when closed or empty.
 * @param[in] direction Negative moves up, non-negative moves down;
 *            clamps at the first/last option.
 */
inline void choice_move(ChoiceDialog &dialog, int direction) noexcept {
  if (!dialog.open || dialog.options.empty()) {
    return;
  }
  if (direction < 0) {
    if (dialog.selected > 0) {
      --dialog.selected;
    }
  } else if (dialog.selected + 1 < dialog.options.size()) {
    ++dialog.selected;
  }
}

/**
 * @brief The confirmed pick (Enter).
 *
 * @param[in] dialog Open dialog with a valid highlight.
 * @return The highlighted option string; empty when the dialog is
 *         closed or the highlight is out of range (nothing to confirm).
 */
[[nodiscard]] inline std::string choice_confirmed(const ChoiceDialog &dialog) {
  if (!dialog.open || dialog.selected >= dialog.options.size()) {
    return {};
  }
  return dialog.options[dialog.selected];
}

/**
 * @brief Close the dialog (Esc) and release its option storage.
 *
 * @param[out] dialog Dialog to close; safe when already closed.
 */
inline void close_choice(ChoiceDialog &dialog) noexcept {
  dialog.open = false;
  dialog.options.clear();
  dialog.selected = 0;
}

} // namespace wengine::tui
