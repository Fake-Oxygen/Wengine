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

} // namespace wengine::cli::commands
