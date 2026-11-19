#pragma once

/**
 * @file
 * INA219 CLI glue: the chip CommandSpec declaration for the `ic`
 * command (kName is the lowercase part number `ina219`). The pure solver
 * lives in solvers/ic/ina219.hpp (TI SBOS448G formulas only, no CLI
 * types); the implementation lives in src/cli/commands/ic/ina219.cpp;
 * registration lives in wengine/cli/ic_registry.hpp. Consumers:
 * ic::dispatch (two-level routing) and the `ic` completion entry.
 * Dependency direction: cli to solvers, never the reverse.
 */

// INA219 CLI glue (spec declaration). Implementation:
// `src/cli/commands/ic/ina219.cpp`. Registered in `ic_registry.hpp`.

#include "wengine/cli/args.hpp"

#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wengine::cli::ic {

/**
 * @brief INA219 monitor chip spec (calibration, codes, readout, trim).
 */
struct Ina219Spec {
  /**
   * @brief Chip name token (`ina219`, the lowercase part number).
   */
  static constexpr std::string_view kName = "ina219";
  /**
   * @brief One-line summary for the `ic` chip list.
   */
  static constexpr std::string_view kDescription =
      "INA219 current/power monitor (calibration, shunt/bus codes, readout, trim)";
  /**
   * @brief Full usage text (printed with usage errors and chip `--help`).
   */
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

  /**
   * @brief Run one INA219 subcommand (cal/shunt/bus/decode/correct/adc).
   * @param[in,out] args Subcommand plus named options (tokens after `ic`
   *            and the chip name; the chip token is already consumed).
   * @param[out] out Stream receiving the result row or the error text.
   * @return 0 with the result printed; 2 on a bad subcommand, bad
   *         arguments, or stray positionals; 1 on a check failure.
   */
  static int run(ArgCursor &args, std::ostream &out);
  /**
   * @brief Complete INA219 subcommands and option names.
   * @param[in] prior_args Tokens after the chip name before the partial.
   * @param[in] partial Partial token under the cursor.
   * @param[out] out Candidates appended without trailing space.
   */
  static void complete(std::span<const std::string_view> prior_args, std::string_view partial,
                       std::vector<std::string> &out);
};

} // namespace wengine::cli::ic
