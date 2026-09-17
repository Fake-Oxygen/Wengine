#pragma once

#include "wengine/cli/args.hpp"
#include "wengine/cli/command.hpp"

#include <ostream>
#include <string_view>

namespace wengine::cli::commands {

struct OhmSpec {
  static constexpr std::string_view kName = "ohm";
  static constexpr std::string_view kDescription = "Solve resistor (Ohm's law)";
  static constexpr std::string_view kUsage =
      "ohm [U] [I]\n"
      "  ohm 12.4V 0.54A\n"
      "  ohm --u 12.4V --i 0.54A\n"
      "  ohm --u 12V --r 4Ohm      (solve from U+R)\n"
      "  ohm --i 2A --r 5Ohm       (solve from I+R)\n"
      "  ohm --help";

  static int run(ArgCursor &args, std::ostream &out);
};

struct S0Spec {
  static constexpr std::string_view kName = "s0";
  static constexpr std::string_view kDescription = "S0 pulse output (energy meter imp/kWh)";
  static constexpr std::string_view kUsage =
      "s0 <mode> [options]   (C = meter constant in imp/kWh)\n"
      "  s0 convert --const 1000imp/kWh\n"
      "  s0 rate --power 2kW --const 1000imp/kWh\n"
      "  s0 power --freq 0.555Hz --const 1000imp/kWh\n"
      "  s0 energy --pulses 500 --const 1000imp/kWh\n"
      "  s0 pulses --power 2kW --time 1h --const 1000imp/kWh\n"
      "  s0 pulses --energy 2kWh --const 1000imp/kWh\n"
      "  s0 full --u 230V --i 10A [--pf 1.0] --const 1000imp/kWh [--time 1h]\n"
      "Modes can also be the first positional: s0 rate 2kW ...\n"
      "Units: W/kW, J/Ws/kJ/Wh/kWh, Hz/kHz, s/ms/min/h,\n"
      "  imp/kWh (also imp/Wh, imp/J == imp/Ws), pulses as bare/imp.";

  static int run(ArgCursor &args, std::ostream &out);
};

} // namespace wengine::cli::commands
