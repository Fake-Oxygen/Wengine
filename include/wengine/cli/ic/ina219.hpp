#pragma once

// INA219 CLI glue (spec declaration). Implementation:
// `src/cli/commands/ic/ina219.cpp`. Registered in `ic_registry.hpp`.

#include "wengine/cli/args.hpp"

#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wengine::cli::ic {

struct Ina219Spec {
  static constexpr std::string_view kName = "ina219";
  static constexpr std::string_view kDescription =
      "INA219 current/power monitor (calibration, shunt/bus codes, readout, trim)";
  static constexpr std::string_view kUsage =
      "ic ina219 <op> [options]   (TI SBOS448G; LSBs: shunt 10uV, bus 4mV)\n"
      "  ic ina219 cal --imax 15A --rshunt 2mOhm [--lsb 1mA] [--vbus 12V]\n"
      "  ic ina219 shunt (--vshunt 20mV | --current 10A --rshunt 2mOhm)\n"
      "  ic ina219 bus --vbus 11.98V\n"
      "  ic ina219 decode --shunt 0x07D0 --bus 0x5D98 --cal 20480 --lsb 1mA\n"
      "        (register codes accept 0x hex or decimal)\n"
      "  ic ina219 correct --cal 20480 --imeas 10A --iina 9.9A\n"
      "  ic ina219 adc (--bits 9|10|11|12 | --avg 1|2|4|8|16|32|64|128)\n"
      "  ic ina219 config [--brng 16|32] [--pga 40|80|160|320]\n"
      "        [--bits 9|10|11|12 | --avg 1|2|4|8|16|32|64|128]\n"
      "        [--mode powerdown|shunt-trig|bus-trig|both-trig|adcoff|shunt|bus|both]\n"
      "  ic ina219 spec\n"
      "Defaults: --vbus 26V max check, config defaults to reset 399Fh\n"
      "  (32V, /8, 12-bit, continuous shunt+bus).";

  static int run(ArgCursor &args, std::ostream &out);
  static void complete(std::span<const std::string_view> prior_args, std::string_view partial,
                       std::vector<std::string> &out);
};

} // namespace wengine::cli::ic
