#pragma once

/**
 * @file
 * TMC22xx CLI glue: the chip CommandSpec declaration for the `ic`
 * command (kName `tmc22xx` covers the TMC2202/2208/2224 family). The pure
 * solver lives in solvers/ic/tmc22xx.hpp (datasheet Rev. 1.14 formulas
 * only, no CLI types); the implementation lives in
 * src/cli/commands/ic/tmc22xx.cpp; registration lives in
 * wengine/cli/ic_registry.hpp. Consumers: ic::dispatch (two-level
 * routing) and the `ic` completion entry. Dependency direction: cli to
 * solvers, never the reverse.
 */

// TMC2202/TMC2208/TMC2224 CLI glue (spec declaration). Implementation:
// `src/cli/commands/ic/tmc22xx.cpp`. Registered in `ic_registry.hpp`.

#include "wengine/cli/args.hpp"

#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wengine::cli::ic {

/**
 * @brief TMC2202/2208/2224 stepper-driver chip spec (family-shared ops).
 */
struct Tmc22xxSpec {
  /**
   * @brief Chip name token (`tmc22xx`, the family-shared command name).
   */
  static constexpr std::string_view kName = "tmc22xx";
  /**
   * @brief One-line summary for the `ic` chip list.
   */
  static constexpr std::string_view kDescription = "TMC2202/2208/2224 stepper driver (current, chopper, StealthChop, standby)";
  /**
   * @brief Full usage text (printed with usage errors and chip `--help`).
   */
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

  /**
   * @brief Run one TMC22xx subcommand (current/chop/hyst/pwm/velocity).
   * @param[in,out] args Subcommand plus named options (tokens after `ic`
   *            and the chip name; the chip token is already consumed).
   * @param[out] out Stream receiving the result row or the error text.
   * @return 0 with the result printed; 2 on a bad subcommand, bad
   *         arguments, or stray positionals; 1 on a check failure.
   */
  static int run(ArgCursor &args, std::ostream &out);
  /**
   * @brief Complete TMC22xx subcommands and option names.
   * @param[in] prior_args Tokens after the chip name before the partial.
   * @param[in] partial Partial token under the cursor.
   * @param[out] out Candidates appended without trailing space.
   */
  static void complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out);
};

} // namespace wengine::cli::ic
