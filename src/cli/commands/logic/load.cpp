#include "common.hpp"

#include "wengine/cli/logic_registry.hpp"
#include "wengine/cli/status.hpp"
#include "wengine/core/status.hpp"
#include "wengine/logic/netlist.hpp"
#include "wengine/logic/seq_state.hpp"

#include <array>
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
  std::array<wengine::logic::LogicValue, wengine::logic::kMaxGates> inits{};
  inits.fill(wengine::logic::LogicValue::kLow);
  std::string err;
  if (!support::yaml::load_text(text, tmp, inits, err)) {
    return cli::runtime_error(out, std::string("cannot load '") + std::string(*path) + "': " + err);
  }
  const std::uint16_t gates = tmp.live_count;
  const std::size_t wires = support::wire_count(tmp);
  // Swap first, then probe the live controls: an illegal SR net (S=R=1)
  // in the file restores the backup so the refused load keeps the
  // workspace (and pool, via the backup below) exactly as before.
  // Loading starts a new pool lifetime (006): serials restart, then the
  // pool powers on to the file's per-element inits.
  Netlist backup = workspace();
  wengine::logic::SeqState pool_backup = support::seq_state();
  workspace() = tmp;
  wengine::logic::reset_seq_state(support::seq_state());
  wengine::logic::init_seq_state(support::seq_state(), workspace());
  for (std::uint16_t s = 0; s < wengine::logic::kMaxGates; ++s) {
    if (support::seq_state().valid[s] != 0 &&
        wengine::logic::is_sequential_kind(workspace().gates[s].kind)) {
      support::seq_state().q[s] = inits[s];
    }
  }
  // Controls stored live in the file act at once; report forced flops below.
  if (!support::apply_async_and_report(workspace(), out).ok()) {
    workspace() = backup;
    support::seq_state() = pool_backup;
    return cli::usage_error(out, "load: S=1,R=1 is illegal (SR latch holds state, workspace kept)", kUsage);
  }
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
