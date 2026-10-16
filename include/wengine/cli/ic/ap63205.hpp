#pragma once

// AP63205WU-7 CLI glue (spec declaration). Implementation:
// `src/cli/commands/ic/ap63205.cpp`. Registered in `ic_registry.hpp`.

#include "wengine/cli/args.hpp"

#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wengine::cli::ic {

struct Ap63205Spec {
  static constexpr std::string_view kName = "ap63205";
  static constexpr std::string_view kDescription =
      "AP63205WU-7 buck design (inductor, COUT, UVLO, EN delay, thermal)";
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

  static int run(ArgCursor &args, std::ostream &out);
  static void complete(std::span<const std::string_view> prior_args, std::string_view partial,
                       std::vector<std::string> &out);
};

} // namespace wengine::cli::ic
