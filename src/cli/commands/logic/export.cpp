#include "common.hpp"

#include "wengine/cli/logic_registry.hpp"
#include "wengine/cli/status.hpp"
#include "wengine/core/status.hpp"
#include "wengine/logic/eval.hpp"
#include "wengine/logic/netlist.hpp"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wengine::cli::logic {

using wengine::logic::BlockPin;
using wengine::logic::Netlist;

int LogicExportSpec::run(ArgCursor &args, std::ostream &out) {
  if (args.take_flag({"--help", "-h"})) {
    out << kName << " - " << kDescription << "\nUsage:\n" << kUsage << '\n';
    return core::kExitOk;
  }
  std::optional<std::string_view> truth_path = args.take_option({"--truth"});
  std::optional<std::string_view> schema_path = args.take_option({"--schema"});
  std::optional<std::string_view> bmp_path = args.take_option({"--bmp"});
  if (args.has_remaining()) {
    return cli::unexpected_error(out, *args.first_remaining(), kUsage);
  }
  if (!truth_path) {
    return cli::usage_error(out, "export needs --truth <t.csv>", kUsage);
  }
  if (!schema_path) {
    return cli::usage_error(out, "export needs --schema <s.yaml>", kUsage);
  }
  if (bmp_path && !support::has_bmp_suffix(*bmp_path)) {
    return cli::usage_error(out, "export --bmp needs a .bmp path", kUsage);
  }
  Netlist &nl = workspace();
  std::vector<BlockPin> in_pins;
  std::vector<BlockPin> out_pins;
  support::interface_pins(nl, in_pins, out_pins);
  const std::size_t nin = in_pins.size();
  if (nin >= 31) {
    return cli::usage_error(out, "export: too many block inputs to enumerate (use truth --limit/--csv)", kUsage);
  }
  const std::uint64_t rows = 1ULL << nin;
  {
    std::ofstream f(std::string(*truth_path), std::ios::binary | std::ios::trunc);
    if (!f) {
      return cli::runtime_error(out, std::string("cannot open file '") + std::string(*truth_path) + "'");
    }
    const core::Status st = support::write_truth_csv(nl, rows, in_pins, out_pins, f);
    f.close();
    if (!st.ok() || !f) {
      return cli::runtime_error(out, std::string("cannot write file '") + std::string(*truth_path) + "'");
    }
  }
  {
    std::ofstream f(std::string(*schema_path), std::ios::binary | std::ios::trunc);
    if (!f) {
      return cli::runtime_error(out, std::string("cannot open file '") + std::string(*schema_path) + "'");
    }
    support::save_yaml(f, nl);
    f.close();
    if (!f) {
      return cli::runtime_error(out, std::string("cannot write file '") + std::string(*schema_path) + "'");
    }
  }
  out << "truth: " << rows << " rows -> '" << *truth_path << "'\n";
  out << "schema: " << nl.live_count << " gates " << support::wire_count(nl) << " wires -> '" << *schema_path << "'\n";
  if (bmp_path) {
    if (!wengine::logic::ensure_topology(nl).ok()) {
      return cli::runtime_error(out, "export: topology error");
    }
    const support::Schematic sc = support::layout_schematic(nl);
    int w = 0;
    int h = 0;
    std::string err;
    if (!support::write_bmp(std::string(*bmp_path), nl, sc, w, h, err)) {
      return cli::runtime_error(out, "export: " + err);
    }
    out << "bmp: " << w << "x" << h << " -> '" << *bmp_path << "'\n";
  }
  return core::kExitOk;
}

void LogicExportSpec::complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out) {
  if (support::starts_with(partial, "--truth=") || support::starts_with(partial, "--schema=") ||
      support::starts_with(partial, "--bmp=")) {
    return; // file paths: nothing to complete
  }
  if (!partial.empty() && partial.front() == '-') {
    constexpr std::string_view kOpts[] = {"--truth", "--schema", "--bmp", "--help", "-h"};
    support::push_matches(partial, kOpts, out);
    return;
  }
  if (!prior_args.empty()) {
    const std::string_view last = prior_args.back();
    if (last == "--truth" || last == "--schema" || last == "--bmp") {
      return;
    }
  }
}

} // namespace wengine::cli::logic
