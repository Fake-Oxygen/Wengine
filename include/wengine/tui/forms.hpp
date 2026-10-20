#pragma once

// Guided forms for the closed-form families (TUI-3).
//
// The TUI must never reimplement parsing or validation (D-TUI-1): every
// form only *composes an argv string* which is submitted through
// `tui::TuiExecutor` (i.e. `cli::Shell::execute`). The CLI owns all
// validation, so invalid input surfaces the CLI usage-error text verbatim.
//
// This header is ncurses-free and unit-testable (see
// `tests/test_tui_forms.cpp`). The ncurses panes in `src/tui/` own
// only field editing + result display.
//
// Field hints come from each spec's `kUsage`/completion vocabulary:
// - `FormDef::usage` aliases the spec's `kUsage` verbatim (no copy).
// - `FormField::key` spellings match the `complete` option tables, so Tab
//   in the main input line and the form fields share one vocabulary.
// - `ic` chips/ops derive from `IcRegistry` (never hardcoded): chip names
//   come from the registry and op names from each chip's `complete`.

#include "wengine/cli/command_registry.hpp"
#include "wengine/cli/commands.hpp"
#include "wengine/cli/ic_registry.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace wengine::tui::forms {

enum class FormKind : int { kOhm = 0, kS0, kI2c, kLdo, kLed, kIc };

inline constexpr int kFormCount = 6;

// One editable field. `key` is the CLI flag (e.g. "--u"); empty `key`
// means a positional slot (mode/chip/op). `label` is the short row name,
// `hint` is the unit/example hint shown beside the editor.
struct FormField {
  std::string_view key;
  std::string_view label;
  std::string_view hint;
};

// A command argument can be tied to an editable field or be a derived
// output. `bit` identifies it in the selected-input mask; accepted masks
// below describe the valid combinations without teaching the TUI solver
// semantics.
struct ArgumentDef {
  static constexpr std::size_t kNoField = std::numeric_limits<std::size_t>::max();

  std::string_view name;
  std::string_view label;
  std::size_t field_index;
  std::uint64_t bit;
  bool user_input;
};

enum class ArgumentState : std::uint8_t {
  kSelected,
  kAvailable,
  kBlocked,
  kComputed,
  kDerived,
};

struct FormDef {
  FormKind kind{};
  std::string_view command;
  std::string_view description;
  std::string_view usage;
  std::vector<FormField> fields;
  std::vector<ArgumentDef> arguments;
  std::vector<std::uint64_t> valid_argument_sets;
};

[[nodiscard]] inline ArgumentState argument_state(const ArgumentDef &argument, std::uint64_t selected, const std::vector<std::uint64_t> &valid_sets) noexcept {
  if (!argument.user_input) {
    return ArgumentState::kDerived;
  }
  if ((selected & argument.bit) != 0U) {
    return ArgumentState::kSelected;
  }
  for (const std::uint64_t valid : valid_sets) {
    if ((selected & ~valid) == 0U && ((selected | argument.bit) & ~valid) == 0U) {
      return ArgumentState::kAvailable;
    }
  }
  for (const std::uint64_t valid : valid_sets) {
    if (selected == valid) {
      return ArgumentState::kComputed;
    }
  }
  return ArgumentState::kBlocked;
}

// Registry-derived chip names for the `ic` form (`IcRegistry` order).
[[nodiscard]] inline std::vector<std::string> ic_chip_names() {
  std::vector<std::string> out;
  for (const auto &c : cli::IcRegistry::all()) {
    out.emplace_back(c.name);
  }
  return out;
}

// Registry-derived op names for one chip: the chip's own `complete` with
// an empty prior (positional slot), minus option-like entries. Empty for
// unknown chips or chips without completion.
[[nodiscard]] inline std::vector<std::string> ic_op_names(std::string_view chip) {
  const cli::CommandDef *def = cli::IcRegistry::find(chip);
  if (def == nullptr || def->complete == nullptr) {
    return {};
  }
  std::vector<std::string> raw;
  const std::span<const std::string_view> empty{};
  def->complete(empty, "", raw);
  std::vector<std::string> ops;
  for (const auto &s : raw) {
    if (!s.empty() && s.front() != '-') {
      ops.push_back(s);
    }
  }
  return ops;
}

// Registry-derived option names for one chip: the chip's `complete` for
// the "--" prefix. Used to prove the form covers the chip vocabulary.
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

// All six closed-form definitions. `usage` aliases the live spec text;
// `fields` use the canonical `complete` spellings (aliases such as
// `--voltage`/`-U` map to the same slot and stay available on the input
// line — the form emits the canonical spelling).
[[nodiscard]] inline std::vector<FormDef> form_defs() {
  using cli::commands::I2cSpec;
  using cli::commands::IcSpec;
  using cli::commands::LdoSpec;
  using cli::commands::LedSpec;
  using cli::commands::OhmSpec;
  using cli::commands::S0Spec;
  constexpr std::uint64_t kOhmVoltage = std::uint64_t{1} << 0U;
  constexpr std::uint64_t kOhmCurrent = std::uint64_t{1} << 1U;
  constexpr std::uint64_t kOhmResistance = std::uint64_t{1} << 2U;
  constexpr std::uint64_t kOhmPower = std::uint64_t{1} << 3U;
  std::vector<FormDef> defs;
  defs.push_back({
      FormKind::kOhm,
      OhmSpec::kName,
      OhmSpec::kDescription,
      OhmSpec::kUsage,
      {
          {"--u", "U", "voltage, e.g. 12V"},
          {"--i", "I", "current, e.g. 0.54A"},
          {"--r", "R", "resistance, e.g. 4Ohm"},
          {"--from", "from", "bare-number order ui|ur|ir|iu|ru|ri"},
      },
      {
          {"voltage", "Voltage (U)", 0, kOhmVoltage, true},
          {"current", "Current (I)", 1, kOhmCurrent, true},
          {"resistance", "Resistance (R)", 2, kOhmResistance, true},
          {"power", "Power (P)", ArgumentDef::kNoField, kOhmPower, false},
      },
      {kOhmVoltage | kOhmCurrent, kOhmVoltage | kOhmResistance, kOhmCurrent | kOhmResistance},
  });
  defs.push_back({
      FormKind::kS0,
      S0Spec::kName,
      S0Spec::kDescription,
      S0Spec::kUsage,
      {
          {"", "mode", "rate|power|energy|pulses|convert|full (empty=auto)"},
          {"--power", "power", "e.g. 2kW"},
          {"--freq", "freq", "e.g. 0.555Hz"},
          {"--pulses", "pulses", "count, e.g. 500"},
          {"--const", "const", "e.g. 1000imp/kWh"},
          {"--time", "time", "e.g. 1h"},
          {"--energy", "energy", "e.g. 2kWh"},
          {"--u", "U", "voltage, e.g. 230V (full)"},
          {"--i", "I", "current, e.g. 10A (full)"},
          {"--pf", "pf", "power factor (0,1], e.g. 0.9"},
      },
      {},
      {},
  });
  defs.push_back({
      FormKind::kI2c,
      I2cSpec::kName,
      I2cSpec::kDescription,
      I2cSpec::kUsage,
      {
          {"", "mode", "pullup|series|full"},
          {"--vdd", "VDD", "supply, e.g. 3.3V"},
          {"--cb", "Cb", "bus cap, e.g. 200pF"},
          {"--speed", "speed", "std|fast|fmp (default fast)"},
          {"--vol", "VOL", "low threshold V (empty=auto)"},
          {"--iol", "IOL", "low current, e.g. 3mA (empty=auto)"},
          {"--tr", "tr", "rise time, e.g. 300ns (empty=auto)"},
          {"--rp", "Rp", "pull-up, e.g. 1.5kOhm (empty=auto)"},
          {"--rs", "Rs", "series, e.g. 100Ohm (empty=auto)"},
      },
      {},
      {},
  });
  defs.push_back({
      FormKind::kLdo,
      LdoSpec::kName,
      LdoSpec::kDescription,
      LdoSpec::kUsage,
      {
          {"--vin", "VIN", "input, e.g. 5V (required)"},
          {"--vout", "VOUT", "output, e.g. 3.3V (required)"},
          {"--iout", "IOUT", "load, e.g. 500mA (required)"},
          {"--iq", "Iq", "quiescent, e.g. 50uA (default 0)"},
          {"--vdo", "Vdo", "dropout at load, e.g. 0.3V"},
          {"--ta", "TA", "ambient C, e.g. 85 (default 25)"},
          {"--rth", "Rth", "C/W, e.g. 100 (default 0)"},
          {"--tjmax", "Tjmax", "limit C, e.g. 125 (default none)"},
      },
      {},
      {},
  });
  defs.push_back({
      FormKind::kLed,
      LedSpec::kName,
      LedSpec::kDescription,
      LedSpec::kUsage,
      {
          {"--vs", "VS", "supply, e.g. 5V (required)"},
          {"--vf", "VF", "forward, e.g. 2V (required)"},
          {"--if", "IF", "forward current, e.g. 20mA (required)"},
          {"--rs", "Rs", "explicit resistor, e.g. 100Ohm (empty=E12)"},
          {"--prating", "Prating", "resistor rating, e.g. 0.25W"},
      },
      {},
      {},
  });
  defs.push_back({
      FormKind::kIc,
      IcSpec::kName,
      IcSpec::kDescription,
      IcSpec::kUsage,
      {
          {"", "chip", "registry chip, e.g. ap63205"},
          {"", "op", "per-chip op, e.g. inductor|cout|uvlo|delay|thermal|spec"},
          {"--vin", "VIN", "e.g. 12V"},
          {"--iout", "IOUT", "e.g. 2A"},
          {"--ripple", "ripple", "fraction 0.3..0.5, e.g. 0.4"},
          {"--delta-il", "dIL", "explicit ripple current, e.g. 0.8A"},
          {"--esr", "ESR", "e.g. 5mOhm (cout)"},
          {"--overshoot", "overshoot", "e.g. 0.25V (cout)"},
          {"--von", "VON", "e.g. 8V (uvlo)"},
          {"--voff", "VOFF", "e.g. 7V (uvlo)"},
          {"--ts", "ts", "e.g. 10ms (delay)"},
          {"--cd", "Cd", "e.g. 12.7nF (delay)"},
          {"--pd", "PD", "e.g. 0.5W (thermal)"},
          {"--ta", "TA", "ambient C, e.g. 85 (thermal)"},
      },
      {},
      {},
  });
  return defs;
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
// tokenizer keeps it a single argument. Unit values never need this;
// the fallback (both quote kinds present) leaves the token raw and lets
// the CLI report the usage error.
[[nodiscard]] inline std::string quote_token(std::string_view t) {
  const bool need =
      t.find_first_of(" \t'\"") != std::string_view::npos;
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

} // namespace detail

// Compose the CLI line for `def` with `values[i]` in each field.
// Empty-after-trim fields are omitted (missing option); nothing is
// validated here — the executor (CLI) reports all errors.
// Compose with an explicit selected-argument mask. Non-argument form
// fields (for example Ohm's --from ordering hint) remain ordinary optional
// fields and are not controlled by this mask.
[[nodiscard]] inline std::string compose_line(const FormDef &def, const std::vector<std::string> &values, std::uint64_t selected_arguments) {
  std::string line(def.command);
  for (std::size_t i = 0; i < def.fields.size() && i < values.size(); ++i) {
    bool argument_selected = true;
    for (const ArgumentDef &argument : def.arguments) {
      if (argument.user_input && argument.field_index == i) {
        argument_selected = (selected_arguments & argument.bit) != 0U;
        break;
      }
    }
    if (!argument_selected) {
      continue;
    }
    const std::string_view v = detail::trim(values[i]);
    if (v.empty()) {
      continue;
    }
    line.push_back(' ');
    if (!def.fields[i].key.empty()) {
      line += def.fields[i].key;
      line.push_back(' ');
    }
    line += detail::quote_token(v);
  }
  return line;
}

[[nodiscard]] inline std::string compose_line(const FormDef &def, const std::vector<std::string> &values) {
  return compose_line(def, values, std::numeric_limits<std::uint64_t>::max());
}

} // namespace wengine::tui::forms
