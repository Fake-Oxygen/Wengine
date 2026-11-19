#pragma once

/**
 * @file
 * The `s0` guided-form definition (F3 browser entry): S0 meter pulse math
 * over the rate/power/energy/pulses/convert/full modes. The form composes
 * one `s0 ...` argv line for the shared CLI path and never validates:
 * empty fields are omitted and the CLI owns every error, so unit text is
 * parsed by the same `units::` suffix tables as the REPL. Consumers: the
 * registry in `wengine/tui/forms.hpp`, the form pane in
 * `src/tui/panes_forms.cpp`.
 */

#include "wengine/cli/commands.hpp"
#include "wengine/tui/forms/types.hpp"

namespace wengine::tui::forms::definitions {

/**
 * @brief Build the `s0` guided-form definition.
 *
 * @return The FormDef for `s0`: the mode positional picker
 *         (Space-opened choice dialog) plus power/freq/pulses/const/
 *         time/energy/U/I/pf fields, with one profile per mode and an
 *         `auto` catch-all fallback.
 */
[[nodiscard]] inline FormDef s0() {
  using cli::commands::S0Spec;
  using definition::all_arguments;
  using definition::argument;
  using definition::bit;
  using definition::profile;
  using definition::rule;

  const std::vector<ArgumentDef> args{
          argument("power", "Power", 1, 0),
          argument("frequency", "Frequency", 2, 1),
          argument("pulses", "Pulse count", 3, 2),
          argument("constant", "Meter constant", 4, 3),
          argument("time", "Time", 5, 4),
          argument("energy", "Energy", 6, 5),
          argument("voltage", "Voltage (U)", 7, 6),
          argument("current", "Current (I)", 8, 7),
          argument("power-factor", "Power factor", 9, 8),
  };
  return {
      FormKind::kS0,
      S0Spec::kName,
      S0Spec::kDescription,
      S0Spec::kUsage,
      {
          {"", "mode", "Space: choose rate|power|energy|pulses|convert|full (empty=auto)",
           {"rate", "power", "energy", "pulses", "convert", "full"}},
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
      args,
      {
          profile("rate", {{0, "rate"}}, {rule(bit(0) | bit(3))},
                  bit(0) | bit(3)),
          profile("power", {{0, "power"}}, {rule(bit(1) | bit(3))},
                  bit(1) | bit(3)),
          profile("energy", {{0, "energy"}}, {rule(bit(2) | bit(3))},
                  bit(2) | bit(3)),
          profile("pulses", {{0, "pulses"}},
                  {rule(bit(3) | bit(5)), rule(bit(3) | bit(0) | bit(4))},
                  bit(3) | bit(0) | bit(4)),
          profile("convert", {{0, "convert"}}, {rule(bit(3))}, bit(3)),
          profile("full", {{0, "full"}},
                  {rule(bit(3) | bit(0), bit(6) | bit(7) | bit(8) | bit(4)),
                   rule(bit(3) | bit(6) | bit(7), bit(0) | bit(8) | bit(4))},
                  bit(3) | bit(6) | bit(7)),
          profile("auto", {}, {rule({}, all_arguments(args))}, {}, true),
      },
  };
}

} // namespace wengine::tui::forms::definitions
