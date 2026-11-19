#pragma once

/**
 * @file
 * AP63205 CLI glue: the chip CommandSpec declaration for the `ic`
 * command (kName is the lowercase part number `ap63205`). The pure
 * solver lives in solvers/ic/ap63205.hpp (datasheet formulas only, no
 * CLI types); the implementation lives in
 * src/cli/commands/ic/ap63205.cpp; registration lives in
 * wengine/cli/ic_registry.hpp. Consumers: ic::dispatch (two-level
 * routing) and the `ic` completion entry. Dependency direction: cli to
 * solvers, never the reverse.
 */

// AP63205WU-7 CLI glue (spec declaration). Implementation:
// `src/cli/commands/ic/ap63205.cpp`. Registered in `ic_registry.hpp`.

#include "wengine/cli/args.hpp"

#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wengine::cli::ic {

/**
 * @brief AP63205 buck-design chip spec (inductor, COUT, UVLO, delay, thermal).
 */
struct Ap63205Spec {
  /**
   * @brief Chip name token (`ap63205`, the lowercase part number).
   */
  static constexpr std::string_view kName = "ap63205";
  /**
   * @brief One-line summary for the `ic` chip list.
   */
  static constexpr std::string_view kDescription =
      "AP63205WU-7 buck design (inductor, COUT, UVLO, EN delay, thermal)";
  /**
   * @brief Full usage text (printed with usage errors and chip `--help`).
   */
  static constexpr std::string_view kUsage =
      "ic ap63205 <op> [options]   (fixed 5V / 1.1MHz part; DS41326 Rev. 3-2)\n"
      "  ic ap63205 inductor --vin 12V [--iout 2A] [--ripple 0.4|--delta-il 0.8A]\n"
      "  ic ap63205 cout --vin 12V [--iout 2A] [--ripple 0.4|--delta-il 0.8A]\n"
      "        --esr 5mOhm [--overshoot 0.25V]\n"
      "  ic ap63205 uvlo --von 8V --voff 7V\n"
      "  ic ap63205 delay (--ts 10ms | --cd 12.7nF)\n"
      "  ic ap63205 thermal (--pd 0.5W | --vin 12V [--iout 2A]) [--ta 85]\n"
      "  ic ap63205 spec\n"
      "Ripple is a fraction of the 2A max load (datasheet 0.3..0.5,\n"
      "  default 0.4); --delta-il overrides it in A/mA. Temperatures in C.";

  /**
   * @brief Run one AP63205 subcommand (inductor/cout/uvlo/delay/thermal).
   * @param[in,out] args Subcommand plus named options (tokens after `ic`
   *            and the chip name; the chip token is already consumed).
   * @param[out] out Stream receiving the design row or the error text.
   * @return 0 with the result printed; 2 on a bad subcommand, bad
   *         arguments, or stray positionals; 1 on a check failure.
   */
  static int run(ArgCursor &args, std::ostream &out);
  /**
   * @brief Complete AP63205 subcommands and option names.
   * @param[in] prior_args Tokens after the chip name before the partial.
   * @param[in] partial Partial token under the cursor.
   * @param[out] out Candidates appended without trailing space.
   */
  static void complete(std::span<const std::string_view> prior_args, std::string_view partial,
                       std::vector<std::string> &out);
};

} // namespace wengine::cli::ic
