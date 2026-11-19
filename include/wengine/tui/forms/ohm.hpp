#pragma once

/**
 * @file
 * The `ohm` guided-form definition (F3 browser entry): Ohm's law from any
 * two of U/I/R, plus the bare-number `--from` order picker. The form
 * composes one `ohm ...` argv line for the shared CLI path and never
 * validates: empty fields are omitted and the CLI owns every error, so
 * unit text is parsed by the same `units::` suffix tables as the REPL.
 * Consumers: the registry in `wengine/tui/forms.hpp`, the form pane in
 * `src/tui/panes_forms.cpp`.
 */

#include "wengine/cli/commands.hpp"
#include "wengine/tui/forms/types.hpp"

namespace wengine::tui::forms::definitions {

/**
 * @brief Build the `ohm` guided-form definition.
 *
 * @return The FormDef for `ohm`: U/I/R fields plus the `from` positional
 *         order picker (Space-opened choice dialog); power (P) is a
 *         derived, non-user-input argument the CLI computes.
 */
[[nodiscard]] inline FormDef ohm() {
  using cli::commands::OhmSpec;
  using definition::argument;
  using definition::bit;
  using definition::profile;
  using definition::rule;

  return {
      FormKind::kOhm,
      OhmSpec::kName,
      OhmSpec::kDescription,
      OhmSpec::kUsage,
      {
          {"--u", "U", "voltage, e.g. 12V"},
          {"--i", "I", "current, e.g. 0.54A"},
          {"--r", "R", "resistance, e.g. 4Ohm"},
          {"--from", "from", "bare-number order (Space: choose)", {"ui", "ur", "ir", "iu", "ru", "ri"}},
      },
      {
          argument("voltage", "Voltage (U)", 0, 0),
          argument("current", "Current (I)", 1, 1),
          argument("resistance", "Resistance (R)", 2, 2),
          argument("power", "Power (P)", ArgumentDef::kNoField, 3, false),
      },
      {profile("ohm", {},
               {rule(bit(0) | bit(1), {}, bit(2), bit(3)),
                rule(bit(0) | bit(2), {}, bit(1), bit(3)),
                rule(bit(1) | bit(2), {}, bit(0), bit(3))},
               bit(0) | bit(1))},
  };
}

} // namespace wengine::tui::forms::definitions
