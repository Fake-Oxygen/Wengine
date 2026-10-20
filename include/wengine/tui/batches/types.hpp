#pragma once

// Shared data types and command-line composition helpers for run builders.
// Builder-specific definitions live beside this file, one per command.

#include "wengine/signal/types.hpp"
#include "wengine/solvers/solver_registry.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace wengine::tui::batches {

enum class BatchKind : int { kSim = 0, kSig };

inline constexpr int kBatchCount = 2;

// One editable builder field. `key` is the CLI flag; `flag` marks a bare
// flag (no value). `label` is the short row name, `hint` the unit/example.
struct BatchField {
  std::string_view key;
  std::string_view label;
  std::string_view hint;
  bool flag = false;
};

struct BatchArgument {
  std::size_t field_index;
  std::uint64_t bit;
  std::uint64_t requires_mask = 0;
};

struct SignalProfile {
  std::string_view kind;
  std::uint64_t active_mask;
  std::uint64_t required_mask = 0;
  std::uint64_t default_selected = 0;
  bool supported = true;
  bool fallback = false;
};

enum class ArgumentState : std::uint8_t {
  kSelected,
  kAvailable,
  kBlocked,
  kInactive,
};

struct BatchDef {
  BatchKind kind{};
  std::string_view command; // "sim" / "sig"
  std::string_view mode;    // always "run"
  std::string_view description;
  std::string_view usage;
  std::vector<BatchField> fields;
  std::vector<BatchArgument> arguments;
  std::vector<SignalProfile> signal_profiles;
};

// Registry-derived solver names for the `sim` builder (`SolverRegistry`
// order, never hardcoded).
[[nodiscard]] inline std::vector<std::string> solver_names() {
  std::vector<std::string> out;
  for (const auto &d : solvers::SolverRegistry::all()) {
    out.emplace_back(d.name);
  }
  return out;
}

// Registry-derived signal kinds for the `sig` builder (`kKindNames`
// order, never hardcoded).
[[nodiscard]] inline std::vector<std::string> signal_kind_names() {
  std::vector<std::string> out;
  for (std::string_view k : signal::kKindNames) {
    out.emplace_back(k);
  }
  return out;
}

// Quantity spellings for `--sig-quantity` (single-table order U, I).
[[nodiscard]] inline std::vector<std::string> quantity_names() {
  return {"U", "I"};
}

// UART wire modes for `--emit-uart-mode`.
[[nodiscard]] inline std::vector<std::string> uart_modes() {
  return {"ascii", "cobs"};
}

namespace detail {

[[nodiscard]] inline std::string_view trim(std::string_view s) noexcept {
  std::size_t b = 0;
  while (b < s.size() && (s[b] == ' ' || s[b] == '\t')) {
    ++b;
  }
  std::size_t e = s.size();
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) {
    --e;
  }
  return s.substr(b, e - b);
}

// Quote one argv token when it carries whitespace/quotes so the CLI
// tokenizer keeps it a single argument (mirrors forms::compose_line:
// file paths with spaces need this; unit values never do).
[[nodiscard]] inline std::string quote_token(std::string_view t) {
  const bool need = t.find_first_of(" \t'\"") != std::string_view::npos;
  if (!need) {
    return std::string(t);
  }
  if (t.find('\'') == std::string_view::npos) {
    return "'" + std::string(t) + "'";
  }
  if (t.find('"') == std::string_view::npos) {
    return "\"" + std::string(t) + "\"";
  }
  return std::string(t);
}

// Bare-flag truth table: empty/0/false/no/off/n (case-insensitive first
// letter suffices for the documented spellings) disables; anything else
// enables. Never validated beyond this — the CLI owns every verdict.
[[nodiscard]] inline bool flag_enabled(std::string_view v) noexcept {
  if (v.empty()) {
    return false;
  }
  std::string low;
  low.reserve(v.size());
  for (char c : v) {
    low.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c));
  }
  return !(low == "0" || low == "false" || low == "no" || low == "off" || low == "n" || low == "f");
}

} // namespace detail

[[nodiscard]] inline std::size_t signal_profile_index(
    const BatchDef &def, const std::vector<std::string> &values) noexcept {
  std::size_t fallback = def.signal_profiles.size();
  if (def.kind != BatchKind::kSig || values.empty()) {
    return fallback;
  }
  const std::string_view kind = detail::trim(values[0]);
  for (std::size_t i = 0; i < def.signal_profiles.size(); ++i) {
    const SignalProfile &profile = def.signal_profiles[i];
    if (profile.fallback) {
      fallback = i;
    } else if (profile.kind == kind) {
      return i;
    }
  }
  return fallback;
}

[[nodiscard]] inline std::uint64_t signal_profile_default_selection(
    const BatchDef &def, const std::vector<std::string> &values) noexcept {
  const std::size_t index = signal_profile_index(def, values);
  return index < def.signal_profiles.size()
      ? def.signal_profiles[index].default_selected : 0U;
}

[[nodiscard]] inline ArgumentState argument_state(
    const BatchArgument &argument, std::uint64_t selected,
    const SignalProfile &profile) noexcept {
  if ((profile.active_mask & argument.bit) == 0U) {
    return ArgumentState::kInactive;
  }
  if ((selected & argument.bit) != 0U) {
    return ArgumentState::kSelected;
  }
  if ((selected & argument.requires_mask) != argument.requires_mask) {
    return ArgumentState::kBlocked;
  }
  return ArgumentState::kAvailable;
}

[[nodiscard]] inline bool argument_required(
    const BatchArgument &argument, const SignalProfile &profile) noexcept {
  return (profile.required_mask & argument.bit) != 0U;
}

[[nodiscard]] inline bool field_visible(const BatchDef &def,
                                        std::size_t field_index,
                                        std::uint64_t selected) noexcept {
  for (const BatchArgument &argument : def.arguments) {
    if (argument.field_index == field_index) {
      return (selected & argument.bit) != 0U;
    }
  }
  return true;
}

// True when the builder values request a bare flag field.
[[nodiscard]] inline bool field_flag_set(const BatchDef &def,
                                         const std::vector<std::string> &values,
                                         std::string_view key,
                                         std::uint64_t selected) {
  for (std::size_t i = 0; i < def.fields.size() && i < values.size(); ++i) {
    if (def.fields[i].key == key && def.fields[i].flag) {
      for (const BatchArgument &argument : def.arguments) {
        if (argument.field_index == i &&
            (selected & argument.bit) == 0U) {
          return false;
        }
      }
      return detail::flag_enabled(detail::trim(values[i]));
    }
  }
  return false;
}

// `--forever` must never run synchronously in the TUI: it runs until
// Ctrl-C, which would block the builder loop exactly like a long batch
// blocks the REPL today. The ncurses layer routes such lines to the
// async worker (TUI-5) and never calls `execute_line` with them.
[[nodiscard]] inline bool wants_async(const BatchDef &def,
                                      const std::vector<std::string> &values,
                                      std::uint64_t selected) {
  return field_flag_set(def, values, "--forever", selected);
}

[[nodiscard]] inline bool field_flag_set(const BatchDef &def,
                                         const std::vector<std::string> &values,
                                         std::string_view key) {
  return field_flag_set(def, values, key, UINT64_MAX);
}

[[nodiscard]] inline bool wants_async(const BatchDef &def,
                                      const std::vector<std::string> &values) {
  return wants_async(def, values, UINT64_MAX);
}

// Compose the CLI line for `def` with `values[i]` in each field.
// Empty-after-trim fields are omitted (missing option); bare flags emit
// the key only when enabled; nothing is validated here — the executor
// (CLI) reports all errors.
[[nodiscard]] inline std::string compose_line(const BatchDef &def,
                                              const std::vector<std::string> &values,
                                              std::uint64_t selected_arguments) {
  std::string line(def.command);
  line.push_back(' ');
  line += def.mode;
  for (std::size_t i = 0; i < def.fields.size() && i < values.size(); ++i) {
    bool selected = true;
    for (const BatchArgument &argument : def.arguments) {
      if (argument.field_index == i) {
        selected = (selected_arguments & argument.bit) != 0U;
        break;
      }
    }
    if (!selected) {
      continue;
    }
    const std::string_view v = detail::trim(values[i]);
    if (v.empty()) {
      continue;
    }
    if (def.fields[i].flag) {
      if (detail::flag_enabled(v)) {
        line.push_back(' ');
        line += def.fields[i].key;
      }
      continue;
    }
    line.push_back(' ');
    line += def.fields[i].key;
    line.push_back(' ');
    line += detail::quote_token(v);
  }
  return line;
}

[[nodiscard]] inline std::string compose_line(const BatchDef &def,
                                              const std::vector<std::string> &values) {
  return compose_line(def, values, UINT64_MAX);
}

} // namespace wengine::tui::batches
