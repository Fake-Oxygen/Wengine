#pragma once

/**
 * @file
 * The `led` guided-form definition (F3 browser entry): series-resistor
 * sizing from supply/forward voltage and forward current, with an
 * optional explicit resistor (empty selects the E12 nearest value) and
 * power-rating field. The form composes one `led ...` argv line for the
 * shared CLI path and never validates: empty fields are omitted and the
 * CLI owns every error, so unit text is parsed by the same `units::`
 * suffix tables as the REPL. Consumers: the registry in
 * `wengine/tui/forms.hpp`, the form pane in
 * `src/tui/panes_forms.cpp`.
 */

#include "wengine/cli/commands.hpp"
#include "wengine/tui/forms/types.hpp"

namespace wengine::tui::forms::definitions {

/**
 * @brief Build the `led` guided-form definition.
 *
 * @return The FormDef for `led`: a single profile requiring VS/VF/IF
 *         with the explicit-resistor and power-rating fields optional.
 */
[[nodiscard]] inline FormDef led() {
  using cli::commands::LedSpec;
  using definition::argument;
  using definition::bit;
  using definition::profile;
  using definition::rule;

  return {
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
      {
          argument("vs", "Supply voltage (VS)", 0, 0),
          argument("vf", "Forward voltage (VF)", 1, 1),
          argument("if", "Forward current (IF)", 2, 2),
          argument("rs", "Series resistor (Rs)", 3, 3),
          argument("prating", "Resistor power rating", 4, 4),
      },
      {profile("led", {}, {rule(bit(0) | bit(1) | bit(2), bit(3) | bit(4))},
               bit(0) | bit(1) | bit(2))},
  };
}

} // namespace wengine::tui::forms::definitions
