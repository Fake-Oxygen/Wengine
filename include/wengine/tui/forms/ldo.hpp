#pragma once

#include "wengine/cli/commands.hpp"
#include "wengine/tui/forms/types.hpp"

namespace wengine::tui::forms::definitions {

[[nodiscard]] inline FormDef ldo() {
  using cli::commands::LdoSpec;
  using definition::argument;
  using definition::bit;
  using definition::profile;
  using definition::rule;

  return {
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
      {
          argument("vin", "VIN", 0, 0),
          argument("vout", "VOUT", 1, 1),
          argument("iout", "IOUT", 2, 2),
          argument("iq", "Iq", 3, 3),
          argument("vdo", "Dropout voltage", 4, 4),
          argument("ta", "Ambient temperature", 5, 5),
          argument("rth", "Thermal resistance", 6, 6),
          argument("tjmax", "Tj limit", 7, 7),
      },
      {profile("ldo", {},
               {rule(bit(0) | bit(1) | bit(2),
                     bit(3) | bit(4) | bit(5) | bit(6) | bit(7))},
               bit(0) | bit(1) | bit(2))},
  };
}

} // namespace wengine::tui::forms::definitions
