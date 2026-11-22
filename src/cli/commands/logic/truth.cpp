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

using wengine::logic::PinRef;
using wengine::logic::Netlist;

int LogicTruthSpec::run(ArgCursor &args, std::ostream &out) {
  if (args.take_flag({"--help", "-h"})) {
    out << kName << " - " << kDescription << "\nUsage:\n" << kUsage << '\n';
    return core::kExitOk;
  }
  std::optional<std::string_view> csv_path = args.take_option({"--csv"});
  std::optional<std::string_view> limit_text = args.take_option({"--limit"});
  const bool with_delays = args.take_flag({support::kWithDelaysFlag});
  if (args.has_remaining()) {
    return cli::unexpected_error(out, *args.first_remaining(), kUsage);
  }
  Netlist &nl = workspace();
  // (011) Four-value nets need the scalar path: refuse loudly, naming
  // the construct and pointing at solve.
  if (wengine::logic::shared_word_blocked(nl)) {
    return cli::runtime_error(out, "truth: shared nets with 2+ drivers need scalar solve (word path is 1-bit per lane)");
  }
  // (012) Buffers drive Z, which has no 1-bit representation: refuse
  // loudly, naming the construct and pointing at scalar solve.
  // Pull-only netlists stream unchanged (termination shrinks the
  // interface; pulled wired nets are pull-irrelevant).
  if (wengine::logic::has_buffer_gates(nl)) {
    return cli::runtime_error(out, "truth: enable-gated buffers need scalar solve (word path is 1-bit per lane)");
  }
  std::vector<PinRef> in_pins;
  std::vector<PinRef> out_pins;
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
  // (014) Opt-in settle column: one wheel per streamed row over the
  // same row convention (block input j takes bit (r>>j)&1). Values
  // still come from the unchanged word evaluator; the column adds
  // each assignment's critical path in whole picoseconds.
  std::vector<std::uint64_t> settle;
  const std::uint64_t *settle_ptr = nullptr;
  if (with_delays) {
    settle.assign(rows, 0);
    wengine::logic::TimingScratch scratch{};
    wengine::logic::ensure_timing_scratch(scratch, wengine::logic::slot_count(nl));
    for (wengine::logic::Slot s = 0; s < wengine::logic::slot_count(nl); ++s) {
      if (nl.slots[s].live && wengine::logic::is_sequential_kind(nl.slots[s].kind)) {
        scratch.cur[s] = wengine::logic::LogicValue::kLow;
      }
    }
    // Topology-invariant sink maps: build once, drain per row (no
    // per-row rebuild over the pool-sized flats).
    wengine::logic::timing_build_sink_maps(nl, scratch);
    std::vector<wengine::logic::LogicValue> vals(nin, wengine::logic::LogicValue::kLow);
    const std::vector<wengine::logic::LogicValue> zero(nin, wengine::logic::LogicValue::kLow);
    std::vector<wengine::logic::TimingSeed> seeds;
    for (std::uint64_t r = 0; r < rows; ++r) {
      for (std::size_t j = 0; j < nin; ++j) {
        const bool bit = (j < 64) && (((r >> j) & 1ULL) != 0ULL);
        vals[j] = bit ? wengine::logic::LogicValue::kHigh : wengine::logic::LogicValue::kLow;
      }
      // Pre-state is the all-zero vector (same row convention as
      // solve); seeds are the row's diffs against it.
      if (!wengine::logic::timing_settle_initial(nl, std::span<const wengine::logic::LogicValue>(zero.data(), nin), scratch, wengine::logic::EdgeFilter::kAll).ok()) {
        return cli::runtime_error(out, "truth failed");
      }
      support::build_block_seeds(in_pins, vals, seeds);
      wengine::logic::TimingTrace trace;
      if (!wengine::logic::run_timing_window_inner(nl, seeds, trace, scratch, false).ok()) {
        return cli::runtime_error(out, support::timing_window_refused("truth"));
      }
      settle[static_cast<std::size_t>(r)] = trace.drain_ps;
    }
    settle_ptr = settle.data();
  }
  if (csv_path) {
    std::ofstream f(std::string(*csv_path), std::ios::binary | std::ios::trunc);
    if (!f) {
      return cli::runtime_error(out, std::string("cannot open CSV file '") + std::string(*csv_path) + "'");
    }
    const core::Status st = support::write_truth_csv(nl, rows, in_pins, out_pins, f, settle_ptr);
    f.close();
    if (!st.ok() || !f) {
      return cli::runtime_error(out, std::string("cannot write CSV file '") + std::string(*csv_path) + "'");
    }
    out << "wrote " << rows << " rows to '" << *csv_path << "' (" << nin << " inputs, " << out_pins.size() << " outputs)\n";
    return core::kExitOk;
  }
  const core::Status st = support::write_truth_pretty(nl, rows, huge ? rows : total, in_pins, out_pins, out, settle_ptr);
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
    constexpr std::string_view kOpts[] = {"--csv", "--limit", "--with-delays", "--help", "-h"};
    support::push_matches(partial, kOpts, out);
    return;
  }
  if (!prior_args.empty() && (prior_args.back() == "--csv" || prior_args.back() == "--limit")) {
    return;
  }
}

} // namespace wengine::cli::logic
