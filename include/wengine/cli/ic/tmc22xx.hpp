#pragma once

// TMC2202/TMC2208/TMC2224 CLI glue (spec declaration). Implementation:
// `src/cli/commands/ic/tmc22xx.cpp`. Registered in `ic_registry.hpp`.

#include "wengine/cli/args.hpp"

#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wengine::cli::ic {

struct Tmc22xxSpec {
  static constexpr std::string_view kName = "tmc22xx";
  static constexpr std::string_view kDescription = "TMC2202/2208/2224 stepper driver (current, chopper, StealthChop, standby)";
  static constexpr std::string_view kUsage =
      "ic tmc22xx <op> [options]   (datasheet Rev. 1.14; fCLK default 12MHz)\n"
      "  ic tmc22xx current --rsense 220mOhm [--irun 31] [--ihold 16]\n"
      "        [--vref 2.5V] [--vsense 0|1]\n"
      "  ic tmc22xx rsense --imax 1A [--vref 2.5V] [--vsense 0|1]\n"
      "  ic tmc22xx rdson (--rref 10kOhm | --ipeak 1.5A | --irms 1A) [--vsense 0|1]\n"
      "  ic tmc22xx chop (--toff 3 | --freq 25kHz [--duty 0.5]) --fclk 12MHz [--tbl 2]\n"
      "  ic tmc22xx hyst --hend 6 --hstrt 0\n"
      "  ic tmc22xx pwmfreq --fclk 12MHz [--sel 0|1|2|3]\n"
      "  ic tmc22xx velocity (--vactual 1000 | --tstep 16777 | --tpwmthrs 500 |\n"
      "        --vel 24kHz|28rpm|0.5rps) [--fclk 12MHz] [--usc 256] [--fsc 200]\n"
      "  ic tmc22xx stealth --vm 24V --rcoil 5Ohm --icoil 0.5A [--fclk 12MHz]\n"
      "        [--tbl 1] [--pwmfreq 0]\n"
      "  ic tmc22xx grad (--cbemf 0.2 | --torque 0.4 --icoilnom 1A)\n"
      "        --vm 24V [--fclk 12MHz] [--mspr 51200]\n"
      "  ic tmc22xx standby --tpowerdown 20 --fclk 12MHz [--irun 31]\n"
      "        [--ihold 16] [--iholddelay 4]\n"
      "  ic tmc22xx thermal --irms 1A [--ta 25] [--rja 30]\n"
      "  ic tmc22xx mres (--code 4 | --usteps 16 |\n"
      "        --pins 220x|222x --ms1 0|1 --ms2 0|1)\n"
      "  ic tmc22xx otp (--pwmgrad 18 | --tpwmthrs 500 | --ihold 16 | --iholddelay 4)\n"
      "  ic tmc22xx uart --fclk 12MHz [--senddelay 0..15]\n"
      "  ic tmc22xx crc 05 00 6C 10 00 00 53   (hex bytes, 0x prefix optional)\n"
      "  ic tmc22xx spec\n"
      "Currents are RMS per coil; ratings: 1.2A cont, 1.4A duty-limited, 2A peak.";

  static int run(ArgCursor &args, std::ostream &out);
  static void complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out);
};

} // namespace wengine::cli::ic
