#include "common.hpp"

#include "wengine/cli/logic_registry.hpp"
#include "wengine/cli/status.hpp"
#include "wengine/core/status.hpp"
#include "wengine/logic/netlist.hpp"

#include <cstdint>
#include <fstream>
#include <iterator>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wengine::cli::logic {

using wengine::logic::Netlist;

int LogicLoadSpec::run(ArgCursor &args, std::ostream &out) {
  if (args.take_flag({"--help", "-h"})) {
    out << kName << " - " << kDescription << "\nUsage:\n" << kUsage << '\n';
    return core::kExitOk;
  }
  std::optional<std::string_view> path = args.next_positional();
  if (!path) {
    return cli::usage_error(out, "load needs a file path (e.g. load work.yaml)", kUsage);
  }
  if (args.has_remaining()) {
    return cli::unexpected_error(out, *args.first_remaining(), kUsage);
  }
  std::ifstream f(std::string(*path), std::ios::binary);
  if (!f) {
    return cli::runtime_error(out, std::string("cannot open file '") + std::string(*path) + "'");
  }
  std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (!f.eof() && !f) {
    return cli::runtime_error(out, std::string("cannot read file '") + std::string(*path) + "'");
  }
  Netlist tmp{};
  std::string err;
  if (!support::yaml::load_text(text, tmp, err)) {
    return cli::runtime_error(out, std::string("cannot load '") + std::string(*path) + "': " + err);
  }
  const std::uint16_t gates = tmp.live_count;
  const std::size_t wires = support::wire_count(tmp);
  workspace() = tmp;
  out << "loaded " << gates << " gates " << wires << " wires <- '" << *path << "'\n";
  return core::kExitOk;
}

void LogicLoadSpec::complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out) {
  (void)prior_args;
  if (partial.empty() || partial.front() != '-') {
    return; // file path: nothing to complete
  }
  constexpr std::string_view kOpts[] = {"--help", "-h"};
  support::push_matches(partial, kOpts, out);
}

} // namespace wengine::cli::logic
