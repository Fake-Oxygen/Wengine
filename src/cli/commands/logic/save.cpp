#include "common.hpp"

#include "wengine/cli/logic_registry.hpp"
#include "wengine/cli/status.hpp"
#include "wengine/core/status.hpp"

#include <fstream>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wengine::cli::logic {

using wengine::logic::Netlist;

int LogicSaveSpec::run(ArgCursor &args, std::ostream &out) {
  if (args.take_flag({"--help", "-h"})) {
    out << kName << " - " << kDescription << "\nUsage:\n" << kUsage << '\n';
    return core::kExitOk;
  }
  std::optional<std::string_view> path = args.next_positional();
  if (!path) {
    return cli::usage_error(out, "save needs a file path (e.g. save work.yaml)", kUsage);
  }
  if (args.has_remaining()) {
    return cli::unexpected_error(out, *args.first_remaining(), kUsage);
  }
  const Netlist &nl = workspace();
  std::ofstream f(std::string(*path), std::ios::binary | std::ios::trunc);
  if (!f) {
    return cli::runtime_error(out, std::string("cannot open file '") + std::string(*path) + "'");
  }
  support::save_yaml(f, nl);
  f.close();
  if (!f) {
    return cli::runtime_error(out, std::string("cannot write file '") + std::string(*path) + "'");
  }
  out << "saved " << nl.live_count << " gates " << support::wire_count(nl) << " wires -> '" << *path << "'\n";
  return core::kExitOk;
}

void LogicSaveSpec::complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out) {
  (void)prior_args;
  if (partial.empty() || partial.front() != '-') {
    return; // file path: nothing to complete
  }
  constexpr std::string_view kOpts[] = {"--help", "-h"};
  support::push_matches(partial, kOpts, out);
}

} // namespace wengine::cli::logic
