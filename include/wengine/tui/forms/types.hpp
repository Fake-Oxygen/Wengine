#pragma once

// Shared data types and command-line composition helpers for ncurses forms.
// Solver-specific definitions live beside this file, one per command.

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

enum class FormKind : int { kOhm = 0, kS0, kI2c, kLdo, kLed, kIc };

inline constexpr int kFormCount = 6;

// One editable field. `key` is the CLI flag (e.g. "--u"); empty `key`
// means a positional slot (mode/chip/op). `choices` lists static enum
// options for the Space-opened choice dialog; empty means free text or a
// dynamic vocabulary resolved by `form_field_choices` (ic chip/op).
struct FormField {
  std::string_view key;
  std::string_view label;
  std::string_view hint;
  std::vector<std::string_view> choices = {};
};

struct ArgumentDef {
  static constexpr std::size_t kNoField = std::numeric_limits<std::size_t>::max();

  std::string_view name;
  std::string_view label;
  std::size_t field_index;
  ArgSet bit; // singleton {bit_index}: which set slot this argument occupies
  bool user_input;
};

struct ArgumentRule {
  ArgSet required_mask;
  ArgSet optional_mask;
  ArgSet computed_mask;
  ArgSet derived_mask;
};

struct ProfileSelector {
  std::size_t field_index;
  std::string_view value;
};

struct ArgumentProfile {
  std::string_view name;
  std::vector<ProfileSelector> selectors;
  std::vector<ArgumentRule> rules;
  ArgSet default_selected;
  bool fallback = false;
};

enum class ArgumentState : std::uint8_t {
  kSelected,
  kAvailable,
  kBlocked,
  kComputed,
  kDerived,
  kInactive,
};

enum class ArgumentRequirement : std::uint8_t {
  kRequired,
  kAlternative,
  kOptional,
};

struct FormDef {
  FormKind kind{};
  std::string_view command;
  std::string_view description;
  std::string_view usage;
  std::vector<FormField> fields;
  std::vector<ArgumentDef> arguments;
  std::vector<ArgumentProfile> profiles;
};

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

namespace definition {

// Singleton {index}: which argument slot a definition occupies. Indices
// are unbounded (see arg_set.hpp); keep them dense per form, 0..N-1.
[[nodiscard]] inline ArgSet bit(unsigned index) {
  return ArgSet::single(index);
}

// Union of every argument slot in a form: the generic catch-all profiles
// (unknown chip/mode) stay complete as new options arrive.
[[nodiscard]] inline ArgSet all_arguments(const std::vector<ArgumentDef> &arguments) {
  ArgSet out;
  for (const ArgumentDef &argument : arguments) {
    out |= argument.bit;
  }
  return out;
}

[[nodiscard]] inline ArgumentDef argument(std::string_view name,
                                          std::string_view label,
                                          std::size_t field_index,
                                          unsigned bit_index,
                                          bool user_input = true) {
  return {name, label, field_index, bit(bit_index), user_input};
}

[[nodiscard]] inline ArgumentRule rule(ArgSet required,
                                          ArgSet optional = ArgSet{},
                                          ArgSet computed = ArgSet{},
                                          ArgSet derived = ArgSet{}) {
  return {required, optional, computed, derived};
}

[[nodiscard]] inline ArgumentProfile profile(
    std::string_view name, std::vector<ProfileSelector> selectors,
    std::vector<ArgumentRule> rules, ArgSet defaults,
    bool fallback = false) {
  return {name, std::move(selectors), std::move(rules), defaults, fallback};
}

} // namespace definition

// Registry-derived chip names and per-chip vocabulary. Chip names and ops
// are never maintained in the TUI form definitions.
[[nodiscard]] inline std::vector<std::string> ic_chip_names() {
  std::vector<std::string> out;
  for (const auto &chip : cli::IcRegistry::all()) {
    out.emplace_back(chip.name);
  }
  return out;
}

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

namespace detail {

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

[[nodiscard]] inline ArgSet profile_default_selection(
    const FormDef &def, const std::vector<std::string> &values) {
  const std::size_t index = active_profile_index(def, values);
  return index < def.profiles.size() ? def.profiles[index].default_selected : ArgSet{};
}

// Compose argv only. The CLI owns all parsing and validation.
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

[[nodiscard]] inline std::string compose_line(
    const FormDef &def, const std::vector<std::string> &values) {
  return compose_line(def, values, definition::all_arguments(def.arguments));
}

// Choice options for one field: static `choices` when declared, else the
// registry-derived vocabulary (ic chip/op). Empty means free text — Space
// keeps its typing meaning there. Value-dependent entries (op needs the
// chip value) read `values`; pass the live field values.
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

[[nodiscard]] inline bool form_field_has_choices(const FormDef &def, std::size_t field_index, const std::vector<std::string> &values) {
  return !form_field_choices(def, field_index, values).empty();
}

} // namespace wengine::tui::forms
