#pragma once

#include "wengine/cli/commands.hpp"
#include "wengine/tui/forms/types.hpp"

namespace wengine::tui::forms::definitions {

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
