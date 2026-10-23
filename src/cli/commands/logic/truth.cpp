#include "common.hpp"

#include "wengine/cli/logic_registry.hpp"
#include "wengine/cli/status.hpp"
#include "wengine/core/status.hpp"
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

int LogicTruthSpec::run(ArgCursor &args, std::ostream &out) {
  if (args.take_flag({"--help", "-h"})) {
    out << kName << " - " << kDescription << "\nUsage:\n" << kUsage << '\n';
    return core::kExitOk;
  }
  std::optional<std::string_view> csv_path = args.take_option({"--csv"});
  std::optional<std::string_view> limit_text = args.take_option({"--limit"});
  if (args.has_remaining()) {
    return cli::unexpected_error(out, *args.first_remaining(), kUsage);
  }
  Netlist &nl = workspace();
  std::vector<BlockPin> in_pins;
  std::vector<BlockPin> out_pins;
  support::interface_pins(nl, in_pins, out_pins);
  const std::size_t nin = in_pins.size();
  bool have_limit = false;
  std::uint64_t limit = 0;
  if (limit_text) {
    const auto p = cli::parse_i64(*limit_text);
    if (!p.ok() || p.value < 1) {
      return cli::usage_error(out, "truth --limit takes an integer >= 1", kUsage);
    }
    have_limit = true;
    limit = static_cast<std::uint64_t>(p.value);
  }
  const bool huge = (nin >= 63);
  std::uint64_t total = 0;
  if (!huge) {
    total = 1ULL << nin;
  }
  if (nin > 16 && !have_limit && !csv_path) {
    return cli::usage_error(out, "truth: too many block inputs for the terminal (need --limit or --csv)", kUsage);
  }
  if (nin >= 31 && !have_limit) {
    return cli::usage_error(out, "truth: too many block inputs to enumerate (need --limit)", kUsage);
  }
  std::uint64_t rows = huge ? limit : total;
  if (have_limit && !huge && limit < rows) {
    rows = limit;
  }
  if (rows == 0) {
    rows = huge ? limit : total;
  }
  if (csv_path) {
    std::ofstream f(std::string(*csv_path), std::ios::binary | std::ios::trunc);
    if (!f) {
      return cli::runtime_error(out, std::string("cannot open CSV file '") + std::string(*csv_path) + "'");
    }
    const core::Status st = support::write_truth_csv(nl, rows, in_pins, out_pins, f);
    f.close();
    if (!st.ok() || !f) {
      return cli::runtime_error(out, std::string("cannot write CSV file '") + std::string(*csv_path) + "'");
    }
    out << "wrote " << rows << " rows to '" << *csv_path << "' (" << nin << " inputs, " << out_pins.size() << " outputs)\n";
    return core::kExitOk;
  }
  const core::Status st = support::write_truth_csv(nl, rows, in_pins, out_pins, out);
  if (!st.ok()) {
    return cli::runtime_error(out, "truth failed");
  }
  return core::kExitOk;
}

void LogicTruthSpec::complete(std::span<const std::string_view> prior_args, std::string_view partial, std::vector<std::string> &out) {
  if (support::starts_with(partial, "--csv=") || support::starts_with(partial, "--limit=")) {
    return; // file path / number: nothing to complete
  }
  if (!partial.empty() && partial.front() == '-') {
    constexpr std::string_view kOpts[] = {"--csv", "--limit", "--help", "-h"};
    support::push_matches(partial, kOpts, out);
    return;
  }
  if (!prior_args.empty() && (prior_args.back() == "--csv" || prior_args.back() == "--limit")) {
    return;
  }
}

} // namespace wengine::cli::logic
