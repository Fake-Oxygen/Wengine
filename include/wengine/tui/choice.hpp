#pragma once

// Scalable enum-choice dialog model for TUI forms/builders (ncurses-free,
// unit-testable).
//
// Enum-valued fields declare their options once in the form/builder
// definition (`FormField::choices` / `BatchField::choices` for static
// vocabularies, plus `form_field_choices` / `batch_field_choices` for
// registry-derived or value-dependent ones). The ncurses layer only calls
// into this model: Space on a field with choices opens the dialog, the
// dialog owns Up/Down/Enter/Esc, and confirm writes the picked string back
// into the field value. New commands/arguments get the dialog for free by
// declaring `choices` — no pane logic changes.

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace wengine::tui {

struct ChoiceDialog {
  bool open = false;
  std::string title;
  std::vector<std::string> options;
  std::size_t selected = 0;
  std::size_t field = 0; // target field index in the active form/builder
};

// Open the dialog for `field` with `options`. The initial highlight tracks
// `current` when it matches an option, else the first option. Returns false
// (stays closed) when there are no options.
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

[[nodiscard]] inline std::string choice_confirmed(const ChoiceDialog &dialog) {
  if (!dialog.open || dialog.selected >= dialog.options.size()) {
    return {};
  }
  return dialog.options[dialog.selected];
}

inline void close_choice(ChoiceDialog &dialog) noexcept {
  dialog.open = false;
  dialog.options.clear();
  dialog.selected = 0;
}

} // namespace wengine::tui
