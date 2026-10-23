#include "common.hpp"

#include "wengine/cli/logic_draw.hpp"
#include "wengine/cli/logic_registry.hpp"
#include "wengine/cli/status.hpp"
#include "wengine/core/status.hpp"
#include "wengine/logic/eval.hpp"
#include "wengine/logic/layout.hpp"

#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wengine::cli::logic {

using wengine::logic::Netlist;
using wengine::logic::Schematic;

int LogicDrawSpec::run(ArgCursor &args, std::ostream &out) {
  if (args.take_flag({"--help", "-h"})) {
    out << kName << " - " << kDescription << "\nUsage:\n" << kUsage << '\n';
    return core::kExitOk;
  }
  std::optional<std::string_view> out_path = args.take_option({"--out"});
  if (args.has_remaining()) {
    return cli::unexpected_error(out, *args.first_remaining(), kUsage);
  }
  if (out_path && !support::has_bmp_suffix(*out_path)) {
    return cli::usage_error(out, "draw --out needs a .bmp path", kUsage);
  }
  Netlist &nl = workspace();
  if (!wengine::logic::ensure_topology(nl).ok()) {
    return cli::runtime_error(out, "draw: topology error");
  }
  const Schematic sc = wengine::logic::layout_schematic(nl);
  if (!out_path) {
    // Interactive canvas (Dear ImGui over SDL2): close == stop.
    // Headless/unsupported fails loudly.
    if (!logic_draw::window_supported()) {
      if (!logic_draw::supported()) {
        return cli::runtime_error(out, "draw: needs an SDL2 build (rebuild with SDL2 available)");
      }
      return cli::runtime_error(out, "draw: needs an ImGui canvas build (rebuild with the imgui submodule)");
    }
    std::string err;
    if (!logic_draw::show_schematic(nl, sc, err)) {
      return cli::runtime_error(out, std::string("draw: ") + err);
    }
    out << "showed " << nl.live_count << " gates (" << sc.levels << " levels; closed by user)\n";
    return core::kExitOk;
  }
  int w = 0;
  int h = 0;
  std::string err;
  if (!logic_draw::write_bmp(std::string(*out_path), nl, sc, w, h, err)) {
    if (!logic_draw::supported()) {
      return cli::runtime_error(out, "draw: needs an SDL2 build (rebuild with SDL2 available)");
    }
    return cli::runtime_error(out, std::string("draw: ") + err);
  }
  out << "drew " << nl.live_count << " gates (" << sc.levels << " levels) -> '" << *out_path << "' (" << w << "x" << h
      << ")\n";
  return core::kExitOk;
}

void LogicDrawSpec::complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out) {
  if (support::starts_with(partial, "--out=")) {
    return; // file path: nothing to complete
  }
  if (!partial.empty() && partial.front() == '-') {
    constexpr std::string_view kOpts[] = {"--out", "--help", "-h"};
    support::push_matches(partial, kOpts, out);
    return;
  }
  if (!prior_args.empty() && prior_args.back() == "--out") {
    return;
  }
}

} // namespace wengine::cli::logic
