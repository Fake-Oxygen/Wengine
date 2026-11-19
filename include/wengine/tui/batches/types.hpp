#pragma once

/**
 * @file
 * Shared data types and command-line composition helpers for the F4 run
 * builders (docs/tui.md TUI-4). Builder-specific definitions live beside
 * this file, one per command (`batches/sim.hpp`, `batches/sig.hpp`); the
 * registry in `wengine/tui/batches.hpp` lists them in F4 browser order.
 * The TUI is a front end: a builder never parses or validates anything,
 * it only composes one argv line for the CLI (`compose_line`), so unit
 * text is parsed by the same `units::` suffix tables as the REPL and every
 * error surfaces as CLI usage text verbatim. Enum-valued fields declare
 * `choices` once; registry-derived vocabularies (solver names, signal
 * kinds) resolve through `batch_field_choices`, so new solvers/kinds need
 * no builder-side change. Consumers: the batch pane in
 * `src/tui/panes_batches.cpp` and `tests/test_tui_batches.cpp`
 * (composition + executor-vs-`Shell` parity matrix). ncurses-free and
 * unit-testable without a terminal.
 */

#include "wengine/signal/types.hpp"
#include "wengine/solvers/solver_registry.hpp"
#include "wengine/tui/arg_set.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace wengine::tui::batches {

/**
 * @brief Which stepped-run command family a BatchDef describes.
 *
 * @note kSim drives `sim run`; kSig drives `sig run`. The value selects
 *       the composed argv line's command word; it never touches engine
 *       state.
 */
enum class BatchKind : int { kSim = 0, kSig };

/**
 * @brief Number of registered run builders (one per BatchKind).
 */
inline constexpr int kBatchCount = 2;

/**
 * @brief One editable builder field.
 *
 * @note `key` is the CLI flag; `flag` marks a bare flag (emitted with no
 *       value when enabled). `choices` lists static enum options for the
 *       Space-opened choice dialog; empty means free text or a dynamic
 *       vocabulary resolved by `batch_field_choices`.
 */
struct BatchField {
  /**
   * @brief CLI flag for this field (e.g. "--steps").
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
   * @brief True for a bare flag (no value composed); false for a
   *        key-value option.
   */
  bool flag = false;
  /**
   * @brief Static enum options; empty means free text or a dynamic
   *        vocabulary via `batch_field_choices`.
   */
  std::vector<std::string_view> choices = {};
};

/**
 * @brief One builder argument slot: which field feeds it and which
 *        selection bit (ArgSet index) switches it on.
 */
struct BatchArgument {
  /**
   * @brief Index of the feeding field in `BatchDef::fields`.
   */
  std::size_t field_index;
  /**
   * @brief Singleton {bit_index}: which selection slot this argument
   *        occupies.
   */
  ArgSet bit; // singleton {bit_index}: which set slot this argument occupies
  /**
   * @brief Argument slots that must be selected before this one becomes
   *        available.
   */
  ArgSet requires_mask;
};

/**
 * @brief One named input shape of a builder: which argument slots are
 *        active, required, or pre-selected under one profile value.
 */
struct BatchProfile {
  /**
   * @brief Profile value (solver or signal kind, e.g. "projectile").
   */
  std::string_view kind;
  /**
   * @brief Argument slots editable under this profile.
   */
  ArgSet active_mask;
  /**
   * @brief Argument slots that must be selected under this profile.
   */
  ArgSet required_mask;
  /**
   * @brief Argument slots pre-selected when the profile activates.
   */
  ArgSet default_selected;
  /**
   * @brief False for combinations the CLI rejects (e.g. the batch
   *        `sum` kind); the pane shows them as unsupported.
   */
  bool supported = true;
  /**
   * @brief True for the catch-all profile of unknown selector values
   *        (the builder never refuses to render).
   */
  bool fallback = false;
};

/**
 * @brief UI state of one builder argument slot under the current
 *        selection.
 */
enum class ArgumentState : std::uint8_t {
  /**
   * @brief Slot is selected and will be composed.
   */
  kSelected,
  /**
   * @brief Slot may be selected (active, requirements satisfied).
   */
  kAvailable,
  /**
   * @brief Slot is active but its requirements are unselected.
   */
  kBlocked,
  /**
   * @brief Slot is not active under this profile.
   */
  kInactive,
};

/**
 * @brief One complete run builder: identity, editable fields, argument
 *        slots, and input-shape profiles.
 */
struct BatchDef {
  /**
   * @brief Which stepped-run command family this builder drives.
   */
  BatchKind kind{};
  /**
   * @brief CLI command name the composed line starts with ("sim"/"sig").
   */
  std::string_view command; // "sim" / "sig"
  /**
   * @brief CLI subcommand the composed line runs (always "run").
   */
  std::string_view mode;    // always "run"
  /**
   * @brief One-line description (from the CLI spec; shown in the F4
   *        browser).
   */
  std::string_view description;
  /**
   * @brief Usage text (from the CLI spec; feeds field hints).
   */
  std::string_view usage;
  /**
   * @brief Editable fields, in pane order.
   */
  std::vector<BatchField> fields;
  /**
   * @brief Argument slots (selection bits) the fields feed.
   */
  std::vector<BatchArgument> arguments;
  /**
   * @brief Named input shapes (solvers/kinds) with their masks.
   */
  std::vector<BatchProfile> profiles;
};

/**
 * @brief Registry-derived solver names for the `sim` builder.
 *
 * @return Every solver in `SolverRegistry` order, never hardcoded in the
 *         builder definitions.
 * @note New solvers appear with no builder-side change.
 */
[[nodiscard]] inline std::vector<std::string> solver_names() {
  std::vector<std::string> out;
  for (const auto &d : solvers::SolverRegistry::all()) {
    out.emplace_back(d.name);
  }
  return out;
}

/**
 * @brief Registry-derived signal kinds for the `sig` builder.
 *
 * @return Every kind in `kKindNames` order, never hardcoded in the
 *         builder definitions.
 * @note New kinds appear with no builder-side change.
 */
[[nodiscard]] inline std::vector<std::string> signal_kind_names() {
  std::vector<std::string> out;
  for (std::string_view k : signal::kKindNames) {
    out.emplace_back(k);
  }
  return out;
}

/**
 * @brief Quantity spellings for `--sig-quantity`.
 *
 * @return The single-table order U, I (matches `signal::kQuantities`).
 */
[[nodiscard]] inline std::vector<std::string> quantity_names() {
  return {"U", "I"};
}

/**
 * @brief UART wire modes for `--emit-uart-mode`.
 *
 * @return The CLI-accepted spellings ascii, cobs.
 */
[[nodiscard]] inline std::vector<std::string> uart_modes() {
  return {"ascii", "cobs"};
}

/**
 * @brief ROS delivery classes for `--ros-qos`.
 *
 * @return latest (latest-state viewing) and reliable (ordered sample
 *         streaming), per the delivery policy in docs/delivery.md.
 */
[[nodiscard]] inline std::vector<std::string> ros_qos_modes() {
  return {"latest", "reliable"};
}

namespace detail {

/**
 * @brief Strip leading and trailing spaces/tabs.
 *
 * @param[in] s Text to trim.
 * @return The trimmed view into `s`.
 */
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

/**
 * @brief Quote one argv token when it carries whitespace/quotes so the
 *        CLI tokenizer keeps it a single argument.
 *
 * @param[in] t Raw field value after trimming.
 * @return `t` unchanged when it needs no quoting; single-quoted when it
 *         carries blanks but no apostrophe; double-quoted when it carries
 *         an apostrophe but no double quote; otherwise the raw token.
 * @note File paths with spaces need this; unit values never do. Mirrors
 *       the forms composition path: quoting is presentation-level, the
 *       CLI owns every verdict.
 */
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

/**
 * @brief Bare-flag truth table: empty/0/false/no/off/n/f
 *        (case-insensitive) disables; anything else enables.
 *
 * @param[in] v Raw field value after trimming.
 * @return False for the documented disabling spellings; true otherwise.
 * @note Never validated beyond this — the CLI owns every verdict.
 */
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

/**
 * @brief Union of every builder argument slot.
 *
 * @param[in] def Builder whose argument slots are unioned.
 * @return The union of all `argument.bit` sets: the "everything
 *         selected" default that composes all non-empty fields however
 *         many options arrive.
 */
[[nodiscard]] inline ArgSet all_arguments(const BatchDef &def) {
  ArgSet out;
  for (const BatchArgument &argument : def.arguments) {
    out |= argument.bit;
  }
  return out;
}

} // namespace detail

/**
 * @brief Index of the active profile for the current selector value.
 *
 * @param[in] def Builder being edited.
 * @param[in] values Live field values; the first value names the profile
 *            (compared after trimming).
 * @return The index of the matching profile; otherwise the fallback
 *         profile's index, or `def.profiles.size()` as an out-of-range
 *         sentinel when nothing matches and no fallback is declared
 *         (callers guard with a size check).
 */
[[nodiscard]] inline std::size_t batch_profile_index(
    const BatchDef &def, const std::vector<std::string> &values) noexcept {
  std::size_t fallback = def.profiles.size();
  if (def.profiles.empty() || values.empty()) {
    return fallback;
  }
  const std::string_view kind = detail::trim(values[0]);
  for (std::size_t i = 0; i < def.profiles.size(); ++i) {
    const BatchProfile &profile = def.profiles[i];
    if (profile.fallback) {
      fallback = i;
    } else if (profile.kind == kind) {
      return i;
    }
  }
  return fallback;
}

/**
 * @brief Argument slots pre-selected by the active profile.
 *
 * @param[in] def Builder being edited.
 * @param[in] values Live field values (the first names the profile).
 * @return The active profile's `default_selected`; an empty set when no
 *         profile matches and no fallback is declared.
 */
[[nodiscard]] inline ArgSet batch_profile_default_selection(const BatchDef &def, const std::vector<std::string> &values) {
  const std::size_t index = batch_profile_index(def, values);
  return index < def.profiles.size() ? def.profiles[index].default_selected : ArgSet{};
}

/**
 * @brief UI state of one argument under the current selection and active
 *        profile.
 *
 * @param[in] argument Slot to classify.
 * @param[in] selected Currently selected argument slots.
 * @param[in] profile Active input shape.
 * @return `kSelected` when the slot is selected; `kAvailable` when it is
 *         active and its requirements are met; `kBlocked` when it is
 *         active but a requirement is unselected; `kInactive` when the
 *         profile never activates it.
 */
[[nodiscard]] inline ArgumentState argument_state(const BatchArgument &argument, const ArgSet &selected, const BatchProfile &profile) {
  if (!(profile.active_mask & argument.bit).any()) {
    return ArgumentState::kInactive;
  }
  if ((selected & argument.bit).any()) {
    return ArgumentState::kSelected;
  }
  if ((argument.requires_mask - selected).any()) {
    return ArgumentState::kBlocked;
  }
  return ArgumentState::kAvailable;
}

/**
 * @brief Whether the profile requires an argument slot.
 *
 * @param[in] argument Slot to probe.
 * @param[in] profile Active input shape.
 * @return True when the slot is in the profile's `required_mask`.
 */
[[nodiscard]] inline bool argument_required(const BatchArgument &argument, const BatchProfile &profile) {
  return (profile.required_mask & argument.bit).any();
}

/**
 * @brief Whether a field is currently shown in the pane.
 *
 * @param[in] def Builder being edited.
 * @param[in] field_index Field to probe.
 * @param[in] selected Currently selected argument slots.
 * @return True when some argument feeding the field is selected; fields
 *         with no argument at all are always visible.
 */
[[nodiscard]] inline bool field_visible(const BatchDef &def, std::size_t field_index, const ArgSet &selected) {
  for (const BatchArgument &argument : def.arguments) {
    if (argument.field_index == field_index) {
      return (selected & argument.bit).any();
    }
  }
  return true;
}

/**
 * @brief Whether the builder values request a bare flag field.
 *
 * @param[in] def Builder being edited.
 * @param[in] values Live field values.
 * @param[in] key Flag key to look up (e.g. "--forever").
 * @param[in] selected Currently selected argument slots; the
 *            flag counts only when its own slot is selected.
 * @return True when the named field is a flag, its slot is selected, and
 *         its value enables it per the bare-flag truth table.
 */
[[nodiscard]] inline bool field_flag_set(const BatchDef &def, const std::vector<std::string> &values, std::string_view key, const ArgSet &selected) {
  for (std::size_t i = 0; i < def.fields.size() && i < values.size(); ++i) {
    if (def.fields[i].key == key && def.fields[i].flag) {
      for (const BatchArgument &argument : def.arguments) {
        if (argument.field_index == i && !(selected & argument.bit).any()) {
          return false;
        }
      }
      return detail::flag_enabled(detail::trim(values[i]));
    }
  }
  return false;
}

/**
 * @brief Whether the composed line must run on the async worker.
 *
 * `--forever` must never run synchronously in the TUI: it runs until
 * Ctrl-C, which would block the builder loop exactly like a long batch
 * blocks the REPL today. The ncurses layer routes such lines to the
 * async worker (TUI-5) and never calls `execute_line` with them.
 *
 * @param[in] def Builder being submitted.
 * @param[in] values Live field values.
 * @param[in] selected Currently selected argument slots.
 * @return True when the `--forever` flag field is set.
 */
[[nodiscard]] inline bool wants_async(const BatchDef &def, const std::vector<std::string> &values, const ArgSet &selected) {
  return field_flag_set(def, values, "--forever", selected);
}

/**
 * @brief Whether the composed line must run on the async worker, with
 *        every argument slot selected.
 *
 * @param[in] def Builder being submitted.
 * @param[in] values Live field values.
 * @param[in] key Flag key to look up (e.g. "--forever").
 * @return True when the named flag field is set under the
 *         everything-selected default.
 */
[[nodiscard]] inline bool field_flag_set(const BatchDef &def, const std::vector<std::string> &values, std::string_view key) {
  return field_flag_set(def, values, key, detail::all_arguments(def));
}

/**
 * @brief Whether the composed line must run on the async worker, with
 *        every argument slot selected.
 *
 * @param[in] def Builder being submitted.
 * @param[in] values Live field values.
 * @return True when `--forever` is set under the everything-selected
 *         default; the ncurses layer routes such lines to the async
 *         worker (TUI-5).
 */
[[nodiscard]] inline bool wants_async(const BatchDef &def, const std::vector<std::string> &values) {
  return wants_async(def, values, detail::all_arguments(def));
}

/**
 * @brief Compose the CLI line for `def` with `values[i]` in each field.
 *
 * Empty-after-trim fields are omitted (missing option); bare flags emit
 * the key only when enabled; nothing is validated here — the executor
 * (CLI) reports all errors.
 *
 * @param[in] def Builder being submitted.
 * @param[in] values One value string per field, in field order.
 * @param[in] selected_arguments Currently selected argument slots; fields
 *            whose argument is unselected are skipped.
 * @return The composed line: `command mode` followed by, per selected
 *         field, `key value` (bare flags emit the key alone), with values
 *         quoted per `quote_token` when needed.
 */
[[nodiscard]] inline std::string compose_line(const BatchDef &def, const std::vector<std::string> &values, const ArgSet &selected_arguments) {
  std::string line(def.command);
  line.push_back(' ');
  line += def.mode;
  for (std::size_t i = 0; i < def.fields.size() && i < values.size(); ++i) {
    bool selected = true;
    for (const BatchArgument &argument : def.arguments) {
      if (argument.field_index == i) {
        selected = (selected_arguments & argument.bit).any();
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

/**
 * @brief Compose the CLI line with every argument slot selected.
 *
 * @param[in] def Builder being submitted.
 * @param[in] values One value string per field, in field order.
 * @return The composed line (see the selected-arguments overload).
 */
[[nodiscard]] inline std::string compose_line(const BatchDef &def, const std::vector<std::string> &values) {
  return compose_line(def, values, detail::all_arguments(def));
}

/**
 * @brief Choice options for one field.
 *
 * Choice options for one field: static `choices` when declared, else the
 * registry-derived vocabulary (`--solver` from SolverRegistry,
 * `--sig-kind` from kKindNames). Empty means free text — Space keeps its
 * typing meaning there.
 *
 * @param[in] def Builder being edited.
 * @param[in] field_index Field whose dialog options are requested.
 * @return The static `choices` when declared, else the registry-derived
 *         vocabulary for `--solver`/`--sig-kind`; empty means Space keeps
 *         its typing meaning.
 * @note The trailing field-values parameter is unnamed (reserved for
 *       value-dependent vocabularies; currently unused). New enum
 *       arguments only declare `choices`, never pane logic.
 */
[[nodiscard]] inline std::vector<std::string> batch_field_choices(const BatchDef &def, std::size_t field_index, const std::vector<std::string> & /*values*/) {
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
  const std::string_view key = def.fields[field_index].key;
  if (key == "--solver") {
    return solver_names();
  }
  if (key == "--sig-kind") {
    return signal_kind_names();
  }
  return {};
}

/**
 * @brief Whether Space opens a choice dialog on this field.
 *
 * @param[in] def Builder being edited.
 * @param[in] field_index Field to probe.
 * @param[in] values Live field values (needed for value-dependent
 *            vocabularies).
 * @return True when `batch_field_choices` yields options; false means
 *         Space keeps its typing meaning.
 */
[[nodiscard]] inline bool batch_field_has_choices(const BatchDef &def, std::size_t field_index, const std::vector<std::string> &values) {
  return !batch_field_choices(def, field_index, values).empty();
}

} // namespace wengine::tui::batches
