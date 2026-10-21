#pragma once

#include "wengine/cli/commands.hpp"
#include "wengine/tui/forms/types.hpp"

namespace wengine::tui::forms::definitions {

[[nodiscard]] inline FormDef i2c() {
  using cli::commands::I2cSpec;
  using definition::all_arguments;
  using definition::argument;
  using definition::bit;
  using definition::profile;
  using definition::rule;

  const std::vector<ArgumentDef> args{
          argument("vdd", "VDD", 1, 0),
          argument("capacitance", "Bus capacitance", 2, 1),
          argument("speed", "Speed", 3, 2),
          argument("vol", "VOL", 4, 3),
          argument("iol", "IOL", 5, 4),
          argument("rise-time", "Rise time", 6, 5),
          argument("rp", "Rp", 7, 6),
          argument("rs", "Rs", 8, 7),
  };
  return {
      FormKind::kI2c,
      I2cSpec::kName,
      I2cSpec::kDescription,
      I2cSpec::kUsage,
      {
          {"", "mode", "Space: choose pullup|series|full", {"pullup", "series", "full"}},
          {"--vdd", "VDD", "supply, e.g. 3.3V"},
          {"--cb", "Cb", "bus cap, e.g. 200pF"},
          {"--speed", "speed", "Space: choose std|fast|fmp (default fast)", {"std", "fast", "fmp"}},
          {"--vol", "VOL", "low threshold V (empty=auto)"},
          {"--iol", "IOL", "low current, e.g. 3mA (empty=auto)"},
          {"--tr", "tr", "rise time, e.g. 300ns (empty=auto)"},
          {"--rp", "Rp", "pull-up, e.g. 1.5kOhm (empty=auto)"},
          {"--rs", "Rs", "series, e.g. 100Ohm (empty=auto)"},
      },
      args,
      {
          profile("pullup", {{0, "pullup"}}, {rule(bit(0) | bit(1), bit(2) | bit(3) | bit(4) | bit(5) | bit(6))}, bit(0) | bit(1)),
          profile("series", {{0, "series"}}, {rule(bit(0) | bit(1), bit(2) | bit(3) | bit(4) | bit(5) | bit(6) | bit(7))}, bit(0) | bit(1)),
          profile("full", {{0, "full"}}, {rule(bit(0) | bit(1), bit(2) | bit(3) | bit(4) | bit(5) | bit(6) | bit(7))}, bit(0) | bit(1)),
          profile("auto", {}, {rule({}, all_arguments(args))}, {}, true),
      },
  };
}

} // namespace wengine::tui::forms::definitions
