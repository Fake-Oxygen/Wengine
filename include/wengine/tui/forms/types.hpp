#pragma once

/**
 * @file
 * Shared data types and command-line composition helpers for the F3
 * guided forms. Solver-specific definitions live beside this file, one
 * per command. The TUI is a front end: these types describe how form
 * input maps to CLI command construction — a form never parses or
 * validates anything, it only composes one argv line for the CLI
 * (`compose_line`), so unit text is parsed by the same `units::` suffix
 * tables as the REPL and every error surfaces as CLI usage text verbatim
 * (docs/tui.md TUI-3). Consumers: `wengine/tui/forms.hpp` (the registry),
 * the per-command definitions in `forms/`, the form pane in
 * `src/tui/panes_forms.cpp`, and `tests/test_tui_forms.cpp`
 * (composition + executor-vs-`Shell` parity matrix). ncurses-free and
 * unit-testable without a terminal.
 */

#include "wengine/cli/ic_registry.hpp"
#include "wengine/tui/arg_set.hpp"

#include <cstddef>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wengine::tui::forms {

/**
 * @brief Which closed-form command family a FormDef describes.
 *
 * @note Enumerators, in registry order: `kOhm` (Ohm's law from any two
 *       of U/I/R), `kS0` (S0 meter pulse math), `kI2c` (bus pull-up/
 *       series resistor sizing), `kLdo` (LDO dropout/thermal sizing),
 *       `kLed` (series resistor + E12 selection), `kIc` (registry-
 *       derived chip/op calculators).
 */
enum class FormKind : int { kOhm = 0, kS0, kI2c, kLdo, kLed, kIc };

/**
 * @brief Number of registered forms (one per FormKind).
 */
inline constexpr int kFormCount = 6;

/**
 * @brief One editable form field.
 *
 * @note `key` is the CLI flag (e.g. "--u"); an empty `key` means a
 *       positional slot (mode/chip/op) whose value is composed without
 *       a flag. `choices` lists static enum options for the Space-opened
 *       choice dialog; empty means free text or a dynamic vocabulary
 *       resolved by `form_field_choices` (ic chip/op).
 */
struct FormField {
  /**
   * @brief CLI flag for this field, or empty for a positional slot.
   */
  std::string_view key;
  /**
   * @brief Short row name shown in the pane.
   */
  std::string_view label;
  /**
   * @brief Unit/example hint shown beside the value.
   */
  std::string_view hint;
  /**
   * @brief Static enum options; empty means free text or a dynamic
   *        vocabulary via `form_field_choices`.
   */
  std::vector<std::string_view> choices = {};
};

/**
 * @brief One solver-input slot a form tracks: which field feeds it and
 *        which selection bit (ArgSet index) switches it on.
 */
struct ArgumentDef {
  /**
   * @brief Sentinel `field_index` for an argument with no editable field
   *        (a value the CLI computes from others, e.g. ohm's power).
   */
  static constexpr std::size_t kNoField = std::numeric_limits<std::size_t>::max();

  /**
   * @brief Solver input name shown in the argument list.
   */
  std::string_view name;
  /**
   * @brief Human-readable label shown in the pane.
   */
  std::string_view label;
  /**
   * @brief Index of the feeding field in `FormDef::fields`, or `kNoField`.
   */
  std::size_t field_index;
  /**
   * @brief Singleton {bit_index}: which selection slot this argument
   *        occupies.
   */
  ArgSet bit; // singleton {bit_index}: which set slot this argument occupies
  /**
   * @brief True when the user can supply it; false marks values the CLI
   *        computes from other arguments (shown as derived, never typed).
   */
  bool user_input;
};

/**
 * @brief One legal input combination of a profile: which argument slots
 *        are required, optional, computed, or derived together.
 */
struct ArgumentRule {
  /**
   * @brief Slots that must be selected for this rule to hold.
   */
  ArgSet required_mask;
  /**
   * @brief Slots the user may additionally select under this rule.
   */
  ArgSet optional_mask;
  /**
   * @brief Slots the CLI computes when exactly `required_mask` is
   *        selected.
   */
  ArgSet computed_mask;
  /**
   * @brief Slots implied (derived) by this rule rather than typed.
   */
  ArgSet derived_mask;
};

/**
 * @brief One profile-activation condition: field `field_index` must hold
 *        `value` (trimmed) for the profile to apply.
 */
struct ProfileSelector {
  /**
   * @brief Index of the deciding field in `FormDef::fields`.
   */
  std::size_t field_index;
  /**
   * @brief Required (trimmed) field value.
   */
  std::string_view value;
};

/**
 * @brief One named input shape of a form: activation selectors, legal
 *        argument combinations, and the default selection.
 */
struct ArgumentProfile {
  /**
   * @brief Profile name (usually the mode/chip-op pair, e.g. "full").
   */
  std::string_view name;
  /**
   * @brief Selectors that must all match for this profile to activate.
   */
  std::vector<ProfileSelector> selectors;
  /**
   * @brief Legal input combinations under this profile.
   */
  std::vector<ArgumentRule> rules;
  /**
   * @brief Argument slots selected when the profile activates.
   */
  ArgSet default_selected;
  /**
   * @brief Whether this is the catch-all profile for unknown selector
   *        values (the form never refuses to render).
   */
  bool fallback = false;
};

/**
 * @brief UI state of one argument slot under the current selection.
 */
enum class ArgumentState : std::uint8_t {
  /** Slot is selected and will be composed. */
  kSelected,
  /** Slot may be selected under a compatible rule. */
  kAvailable,
  /** Slot belongs to the form/profile but no compatible rule allows it yet. */
  kBlocked,
  /** Slot is computed by the CLI when exactly a rule's required set is selected. */
  kComputed,
  /** Slot is derived (not user input) under a compatible rule. */
  kDerived,
  /** Slot never appears in this profile. */
  kInactive,
};

/**
 * @brief How insistently a profile wants an argument selected.
 */
enum class ArgumentRequirement : std::uint8_t {
  /** Every compatible rule mentioning it requires it. */
  kRequired,
  /** Required under one compatible rule, optional under another. */
  kAlternative,
  /** No compatible rule requires it. */
  kOptional,
};

/**
 * @brief One complete guided form: identity, editable fields, argument
 *        slots, and input-shape profiles.
 */
struct FormDef {
  /**
   * @brief Which command family this form drives.
   */
  FormKind kind{};
  /**
   * @brief CLI command name the composed line starts with.
   */
  std::string_view command;
  /**
   * @brief One-line description (from the CLI spec; shown in the F3 browser).
   */
  std::string_view description;
  /**
   * @brief Usage text (from the CLI spec; feeds field hints).
   */
  std::string_view usage;
  /**
   * @brief Editable fields, in pane order.
   */
  std::vector<FormField> fields;
  /**
   * @brief Argument slots (selection bits) the fields feed.
   */
  std::vector<ArgumentDef> arguments;
  /**
   * @brief Named input shapes (modes/chip-ops) with their rules.
   */
  std::vector<ArgumentProfile> profiles;
};

/**
 * @brief UI state of one argument under the current selection and
 *        active profile.
 *
 * @param[in] argument Slot to classify.
 * @param[in] selected Currently selected argument slots.
 * @param[in] profile Active input shape.
 * @return `kDerived` when the argument is not user input; `kSelected`
 *         when its slot is selected; `kAvailable` when a rule compatible
 *         with the selection allows it; `kComputed` when the selection
 *         equals a compatible rule's required set and the rule computes
 *         it; `kDerived` when any rule derives it; `kBlocked` when the
 *         profile mentions it but no compatible rule does; `kInactive`
 *         when the profile never mentions it at all.
 */
[[nodiscard]] inline ArgumentState argument_state( const ArgumentDef &argument, const ArgSet &selected, const ArgumentProfile &profile) {
  if (!argument.user_input) {
    return ArgumentState::kDerived;
  }
  if ((selected & argument.bit).any()) {
    return ArgumentState::kSelected;
  }
  bool appears_in_profile = false;
  for (const ArgumentRule &rule : profile.rules) {
    const ArgSet allowed = rule.required_mask | rule.optional_mask;
    appears_in_profile = appears_in_profile || (allowed & argument.bit).any() || (rule.computed_mask & argument.bit).any() ||
                         (rule.derived_mask & argument.bit).any();
    if (!(selected - allowed).empty()) {
      continue;
    }
    if ((allowed & argument.bit).any()) {
      return ArgumentState::kAvailable;
    }
    if (selected == rule.required_mask && (rule.computed_mask & argument.bit).any()) {
      return ArgumentState::kComputed;
    }
  }
  for (const ArgumentRule &rule : profile.rules) {
    if ((rule.derived_mask & argument.bit).any()) {
      return ArgumentState::kDerived;
    }
  }
  return appears_in_profile ? ArgumentState::kBlocked : ArgumentState::kInactive;
}

/**
 * @brief How strongly the active profile wants an argument selected.
 *
 * @param[in] argument Slot to classify.
 * @param[in] selected Currently selected argument slots.
 * @param[in] profile Active input shape.
 * @return `kRequired` when the argument is required by a rule compatible
 *         with the selection and optional in none; `kAlternative` when
 *         it is required under one compatible rule and optional under
 *         another; `kOptional` when no compatible rule requires it.
 */
[[nodiscard]] inline ArgumentRequirement argument_requirement(const ArgumentDef &argument, const ArgSet &selected,
    const ArgumentProfile &profile) {
  bool required = false;
  bool optional = false;
  for (const ArgumentRule &rule : profile.rules) {
    const ArgSet allowed = rule.required_mask | rule.optional_mask;
    if ((selected - allowed).empty() && (allowed & argument.bit).any()) {
      required = required || (rule.required_mask & argument.bit).any();
      optional = optional || (rule.optional_mask & argument.bit).any();
    }
  }
  if (required && !optional) {
    return ArgumentRequirement::kRequired;
  }
  return required ? ArgumentRequirement::kAlternative
                  : ArgumentRequirement::kOptional;
}

/**
 * @brief Declaration helpers the per-command form definitions are built
 *        from — they only assemble the plain structs above.
 */
namespace definition {

/**
 * @brief Singleton {index}: which argument slot a definition occupies.
 *
 * @param[in] index Slot index (unbounded; see arg_set.hpp). Keep indices
 *            dense per form, 0..N-1.
 * @return The singleton set for `index`.
 */
[[nodiscard]] inline ArgSet bit(unsigned index) {
  return ArgSet::single(index);
}

/**
 * @brief Union of every argument slot in a form.
 *
 * @param[in] arguments The form's argument slots.
 * @return The union of all `argument.bit` sets, so the generic catch-all
 *         profiles (unknown chip/mode) stay complete as new options
 *         arrive.
 */
[[nodiscard]] inline ArgSet all_arguments(const std::vector<ArgumentDef> &arguments) {
  ArgSet out;
  for (const ArgumentDef &argument : arguments) {
    out |= argument.bit;
  }
  return out;
}

/**
 * @brief Build one ArgumentDef.
 *
 * @param[in] name Solver input name shown in the argument list.
 * @param[in] label Human-readable label shown in the pane.
 * @param[in] field_index Feeding field index, or `ArgumentDef::kNoField`
 *            for a computed-only argument.
 * @param[in] bit_index Selection slot the argument occupies.
 * @param[in] user_input True when the user can type it; false marks
 *            values the CLI computes.
 * @return The assembled argument definition.
 */
[[nodiscard]] inline ArgumentDef argument(std::string_view name,
                                          std::string_view label,
                                          std::size_t field_index,
                                          unsigned bit_index,
                                          bool user_input = true) {
  return {name, label, field_index, bit(bit_index), user_input};
}

/**
 * @brief Build one ArgumentRule (a legal input combination).
 *
 * @param[in] required Slots that must be selected.
 * @param[in] optional Slots additionally allowed.
 * @param[in] computed Slots the CLI computes when exactly `required`
 *            is selected.
 * @param[in] derived Slots implied by the rule rather than typed.
 * @return The assembled rule.
 */
[[nodiscard]] inline ArgumentRule rule(ArgSet required,
                                          ArgSet optional = ArgSet{},
                                          ArgSet computed = ArgSet{},
                                          ArgSet derived = ArgSet{}) {
  return {required, optional, computed, derived};
}

/**
 * @brief Build one ArgumentProfile.
 *
 * @param[in] name Profile name (mode or chip-op pair).
 * @param[in] selectors Activation conditions (all must match).
 * @param[in] rules Legal input combinations under the profile.
 * @param[in] defaults Argument slots selected when the profile activates.
 * @param[in] fallback True for the catch-all profile of unknown selector
 *            values.
 * @return The assembled profile.
 */
[[nodiscard]] inline ArgumentProfile profile(
    std::string_view name, std::vector<ProfileSelector> selectors,
    std::vector<ArgumentRule> rules, ArgSet defaults,
    bool fallback = false) {
  return {name, std::move(selectors), std::move(rules), defaults, fallback};
}

} // namespace definition

/**
 * @brief Registry-derived chip names for the `ic` form.
 *
 * @return Every chip in `cli::IcRegistry` order. Chip names are never
 *         maintained in the TUI form definitions, so registering a chip
 *         updates this vocabulary with no form-side change.
 */
[[nodiscard]] inline std::vector<std::string> ic_chip_names() {
  std::vector<std::string> out;
  for (const auto &chip : cli::IcRegistry::all()) {
    out.emplace_back(chip.name);
  }
  return out;
}

/**
 * @brief Per-chip operation names for the `ic` form's `op` field.
 *
 * @param[in] chip Chip name as currently typed into the chip field.
 * @return The chip's positional-operation vocabulary (its completion
 *         output with help/option entries filtered out); empty for an
 *         unknown chip or a chip without completion. The F9 dialog
 *         derives from this, so ops are picked, never mistyped.
 */
[[nodiscard]] inline std::vector<std::string> ic_op_names(std::string_view chip) {
  const cli::CommandDef *def = cli::IcRegistry::find(chip);
  if (def == nullptr || def->complete == nullptr) {
    return {};
  }
  std::vector<std::string> raw;
  const std::span<const std::string_view> empty{};
  def->complete(empty, "", raw);
  std::vector<std::string> ops;
  for (const auto &value : raw) {
    if (!value.empty() && value.front() != '-') {
      ops.push_back(value);
    }
  }
  return ops;
}

/**
 * @brief Per-chip option-flag names for the `ic` form fields.
 *
 * @param[in] chip Chip name.
 * @return The chip's `--`-prefixed completion vocabulary (option flags
 *         plus `--help`); empty for an unknown chip or a chip without
 *         completion.
 */
[[nodiscard]] inline std::vector<std::string> ic_option_names(std::string_view chip) {
  const cli::CommandDef *def = cli::IcRegistry::find(chip);
  if (def == nullptr || def->complete == nullptr) {
    return {};
  }
  std::vector<std::string> out;
  const std::span<const std::string_view> empty{};
  def->complete(empty, "--", out);
  return out;
}

/**
 * @brief Internal text helpers shared by the composition paths (not part
 *        of the form model's public surface).
 */
namespace detail {

/**
 * @brief Strip leading and trailing spaces/tabs.
 *
 * @param[in] s Text to trim.
 * @return The trimmed view into `s`.
 */
[[nodiscard]] inline std::string_view trim(std::string_view s) noexcept {
  std::size_t begin = 0;
  while (begin < s.size() && (s[begin] == ' ' || s[begin] == '\t')) {
    ++begin;
  }
  std::size_t end = s.size();
  while (end > begin && (s[end - 1] == ' ' || s[end - 1] == '\t')) {
    --end;
  }
  return s.substr(begin, end - begin);
}

/**
 * @brief Quote one argv token when it carries whitespace/quotes so the
 *        CLI tokenizer keeps it a single argument.
 *
 * @param[in] token Raw field value after trimming.
 * @return `token` unchanged when it needs no quoting; `'...'` when it
 *         contains blanks but no apostrophe; `"..."` when it contains an
 *         apostrophe but no double quote; otherwise the raw token.
 * @note File paths with spaces need this; unit values never do.
 */
[[nodiscard]] inline std::string quote_token(std::string_view token) {
  const bool need_quotes = token.find_first_of(" \t'\"") != std::string_view::npos;
  if (!need_quotes) {
    return std::string(token);
  }
  if (token.find('\'') == std::string_view::npos) {
    return "'" + std::string(token) + "'";
  }
  if (token.find('"') == std::string_view::npos) {
    return "\"" + std::string(token) + "\"";
  }
  return std::string(token);
}

} // namespace detail

/**
 * @brief Index of the active profile for the current field values.
 *
 * @param[in] def Form being edited.
 * @param[in] values Live field values (selector fields are compared
 *            after trimming).
 * @return The index of the first profile whose selectors all match;
 *         otherwise the fallback profile's index, or `def.profiles.size()`
 *         as an out-of-range sentinel when nothing matches and no
 *         fallback is declared (callers guard with a size check).
 */
[[nodiscard]] inline std::size_t active_profile_index(
    const FormDef &def, const std::vector<std::string> &values) noexcept {
  std::size_t fallback = def.profiles.size();
  for (std::size_t i = 0; i < def.profiles.size(); ++i) {
    const ArgumentProfile &profile = def.profiles[i];
    if (profile.fallback) {
      fallback = i;
      continue;
    }
    bool matches = true;
    for (const ProfileSelector &selector : profile.selectors) {
      if (selector.field_index >= values.size() ||
          detail::trim(values[selector.field_index]) != selector.value) {
        matches = false;
        break;
      }
    }
    if (matches) {
      return i;
    }
  }
  return fallback;
}

/**
 * @brief Argument slots pre-selected by the active profile.
 *
 * @param[in] def Form being edited.
 * @param[in] values Live field values (selector fields are compared
 *            after trimming).
 * @return The active profile's `default_selected`; an empty set when no
 *         profile matches and no fallback is declared.
 */
[[nodiscard]] inline ArgSet profile_default_selection(
    const FormDef &def, const std::vector<std::string> &values) {
  const std::size_t index = active_profile_index(def, values);
  return index < def.profiles.size() ? def.profiles[index].default_selected : ArgSet{};
}

/**
 * @brief Compose the CLI argv line for `def` from the live field values.
 *
 * @param[in] def Form being submitted.
 * @param[in] values One value string per field, in field order.
 * @param[in] selected_arguments Argument slots currently selected.
 * @return The composed line: `command` followed by, per selected field,
 *         `key value` (or the bare value for positional fields), with
 *         values quoted per `quote_token` when needed.
 * @note Compose argv only. Empty-after-trim fields are omitted (missing
 *       option) and the CLI owns all parsing and validation — invalid
 *       input shows the CLI usage-error text verbatim. Fields tied to a
 *       user-input argument appear only when that argument's slot is
 *       selected; fields without one always appear.
 */
[[nodiscard]] inline std::string compose_line(
    const FormDef &def, const std::vector<std::string> &values,
    const ArgSet &selected_arguments) {
  std::string line(def.command);
  for (std::size_t i = 0; i < def.fields.size() && i < values.size(); ++i) {
    bool selected = true;
    for (const ArgumentDef &argument : def.arguments) {
      if (argument.user_input && argument.field_index == i) {
        selected = (selected_arguments & argument.bit).any();
        break;
      }
    }
    if (!selected) {
      continue;
    }
    const std::string_view value = detail::trim(values[i]);
    if (value.empty()) {
      continue;
    }
    line.push_back(' ');
    if (!def.fields[i].key.empty()) {
      line += def.fields[i].key;
      line.push_back(' ');
    }
    line += detail::quote_token(value);
  }
  return line;
}

/**
 * @brief Compose the CLI argv line with every argument slot selected.
 *
 * @param[in] def Form being submitted.
 * @param[in] values One value string per field, in field order.
 * @return The composed line (see the selected-arguments overload).
 */
[[nodiscard]] inline std::string compose_line(
    const FormDef &def, const std::vector<std::string> &values) {
  return compose_line(def, values, definition::all_arguments(def.arguments));
}

/**
 * @brief Choice options for one field.
 *
 * @param[in] def Form being edited.
 * @param[in] field_index Field whose dialog options are requested.
 * @param[in] values Live field values; value-dependent vocabularies (the
 *            `ic` op list needs the chip value) read them.
 * @return The static `choices` when declared, else the registry-derived
 *         vocabulary (`ic` chip names, per-chip ops for the chip typed so
 *         far); empty means free text — Space keeps its typing meaning
 *         there.
 * @note New enum arguments only declare `choices`, never pane logic.
 */
[[nodiscard]] inline std::vector<std::string> form_field_choices(const FormDef &def, std::size_t field_index, const std::vector<std::string> &values) {
  if (field_index >= def.fields.size()) {
    return {};
  }
  if (!def.fields[field_index].choices.empty()) {
    std::vector<std::string> out;
    out.reserve(def.fields[field_index].choices.size());
    for (std::string_view c : def.fields[field_index].choices) {
      out.emplace_back(c);
    }
    return out;
  }
  if (def.command == "ic") {
    const std::string_view label = def.fields[field_index].label;
    if (label == "chip") {
      return ic_chip_names();
    }
    if (label == "op" && !values.empty()) {
      return ic_op_names(detail::trim(values[0]));
    }
  }
  return {};
}

/**
 * @brief Whether Space opens a choice dialog on this field.
 *
 * @param[in] def Form being edited.
 * @param[in] field_index Field to probe.
 * @param[in] values Live field values (needed for value-dependent
 *            vocabularies).
 * @return True when `form_field_choices` yields options; false means
 *         Space keeps its typing meaning.
 */
[[nodiscard]] inline bool form_field_has_choices(const FormDef &def, std::size_t field_index, const std::vector<std::string> &values) {
  return !form_field_choices(def, field_index, values).empty();
}

} // namespace wengine::tui::forms
