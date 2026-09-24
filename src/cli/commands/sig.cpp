#include "wengine/cli/commands.hpp"
#include "wengine/cli/status.hpp"
#include "wengine/core/run_config.hpp"
#include "wengine/emit/signal_sink.hpp"
#include "wengine/link/frame.hpp"
#include "wengine/link/shm_publisher.hpp"
#include "wengine/signal/table.hpp"
#include "wengine/signal/types.hpp"
#include "wengine/solvers/run.hpp"
#include "wengine/solvers/solver_registry.hpp"

#include <array>
#include <cstdint>
#include <fstream>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wengine::cli::commands {

namespace {

[[nodiscard]] constexpr bool is_sig_mode(std::string_view s) noexcept {
  return s == "run";
}

int usage_error(std::ostream &out, std::string_view msg) {
  return cli::usage_error(out, msg, SigSpec::kUsage);
}

void push_matches(std::string_view partial, std::span<const std::string_view> options,
                  std::vector<std::string> &out) {
  for (std::string_view o : options) {
    if (o.substr(0, partial.size()) == partial) {
      out.emplace_back(o);
    }
  }
}

constexpr std::array<std::string_view, 1> kSigModes{{"run"}};

constexpr std::array<std::string_view, 19> kSigOptions{{
    "--steps", "-n", "--dt", "--sig-kind", "--sig-freq", "--sig-amp", "--sig-offset",
    "--sig-phase", "--sig-duty", "--sig-seed", "--sig-quantity", "--sig-count",
    "--sig-expr", "--sig-table", "--emit-csv", "--emit-shm", "--emit-every", "--help",
    "-h",
}};

} // namespace

int SigSpec::run(ArgCursor &args, std::ostream &out) {
  if (args.take_flag({"--help", "-h"})) {
    out << kName << " - " << kDescription << "\nUsage:\n" << kUsage << '\n';
    return core::kExitOk;
  }

  long long steps = 100;
  double dt = 0.01;
  long long emit_every = 1;
  std::optional<std::string_view> csv_path;
  std::optional<std::string_view> shm_name;

  auto take_i64 = [&](std::initializer_list<std::string_view> names, const char *what,
                      long long &slot) -> bool {
    if (auto o = args.take_option(names)) {
      auto p = parse_i64(*o);
      if (!p.ok()) {
        out << "Error: invalid " << what << " '" << *o << "'\n";
        out << kUsage << '\n';
        return false;
      }
      slot = p.value;
    }
    return true;
  };

  if (!take_i64({"--steps", "-n"}, "step count", steps)) {
    return core::kExitUsage;
  }
  if (auto o = args.take_option({"--dt"})) {
    const auto p = parse_time(*o);
    if (!p.ok()) {
      out << "Error: invalid time step '" << *o << "'\n";
      out << kUsage << '\n';
      return core::kExitUsage;
    }
    dt = p.value;
  }
  if (!take_i64({"--emit-every"}, "emit stride", emit_every)) {
    return core::kExitUsage;
  }
  csv_path = args.take_option({"--emit-csv"});
  shm_name = args.take_option({"--emit-shm"});

  // Signal vocabulary lives here (never in sim run): raw views first,
  // amp/offset parse against --sig-quantity.
  const auto o_sig_kind = args.take_option({"--sig-kind"});
  const auto o_sig_freq = args.take_option({"--sig-freq"});
  const auto o_sig_amp = args.take_option({"--sig-amp"});
  const auto o_sig_offset = args.take_option({"--sig-offset"});
  const auto o_sig_phase = args.take_option({"--sig-phase"});
  const auto o_sig_duty = args.take_option({"--sig-duty"});
  const auto o_sig_seed = args.take_option({"--sig-seed"});
  const auto o_sig_quantity = args.take_option({"--sig-quantity"});
  const auto o_sig_count = args.take_option({"--sig-count"});
  const auto o_sig_expr = args.take_option({"--sig-expr"});
  const auto o_sig_table = args.take_option({"--sig-table"});

  signal::SignalParams params;
  if (o_sig_kind) {
    const auto kind = signal::parse_kind(*o_sig_kind);
    if (!kind.ok()) {
      return usage_error(out, "unknown signal kind (want sine|square|triangle|saw|dc|"
                              "pulse|noise|sum|expression|table)");
    }
    params.kind = kind.value;
  }
  if (o_sig_quantity) {
    const auto qty = signal::parse_quantity(*o_sig_quantity);
    if (!qty.ok()) {
      return usage_error(out, "unknown signal quantity (want U|I)");
    }
    params.quantity = qty.value;
  }
  auto take_qty = [&](std::optional<std::string_view> o, const char *what,
                      double &slot) -> bool {
    if (o) {
      // Quantity-aware parse through the units-owned tables (no ad-hoc
      // voltage/current switch here — see signal::kQuantities).
      const auto p = parse_quantity_value(params.quantity, *o);
      if (!p.ok()) {
        out << "Error: invalid " << what << " '" << *o << "'\n";
        out << kUsage << '\n';
        return false;
      }
      slot = p.value;
    }
    return true;
  };
  if (o_sig_freq) {
    const auto p = parse_frequency(*o_sig_freq);
    if (!p.ok()) {
      out << "Error: invalid signal frequency '" << *o_sig_freq << "'\n";
      out << kUsage << '\n';
      return core::kExitUsage;
    }
    params.freq_hz = p.value;
  }
  if (!take_qty(o_sig_amp, "signal amplitude", params.amp) ||
      !take_qty(o_sig_offset, "signal offset", params.offset)) {
    return core::kExitUsage;
  }
  if (o_sig_phase) {
    const auto p = parse_double(*o_sig_phase);
    if (!p.ok()) {
      out << "Error: invalid signal phase '" << *o_sig_phase << "'\n";
      out << kUsage << '\n';
      return core::kExitUsage;
    }
    params.phase_rad = p.value;
  }
  if (o_sig_duty) {
    const auto p = parse_double(*o_sig_duty);
    if (!p.ok()) {
      out << "Error: invalid signal duty '" << *o_sig_duty << "'\n";
      out << kUsage << '\n';
      return core::kExitUsage;
    }
    params.duty = p.value;
  }
  long long sig_seed = 1;
  long long sig_count = 1;
  if (o_sig_seed) {
    const auto p = parse_i64(*o_sig_seed);
    if (!p.ok()) {
      out << "Error: invalid signal seed '" << *o_sig_seed << "'\n";
      out << kUsage << '\n';
      return core::kExitUsage;
    }
    sig_seed = p.value;
  }
  if (o_sig_count) {
    const auto p = parse_i64(*o_sig_count);
    if (!p.ok()) {
      out << "Error: invalid signal count '" << *o_sig_count << "'\n";
      out << kUsage << '\n';
      return core::kExitUsage;
    }
    sig_count = p.value;
  }

  // Custom sources (SIG-4a): expression text compiles now (config edge),
  // table CSV loads now (app-edge file I/O — the engine never opens files).
  // Cross-kind leftovers are usage errors, never silently ignored.
  std::vector<signal::TablePoint> table_points;
  const bool is_expr = params.kind == signal::SignalKind::kExpression;
  const bool is_table = params.kind == signal::SignalKind::kTable;
  if (is_expr) {
    if (!o_sig_expr) {
      return usage_error(out, "expression kind needs --sig-expr '<math>'");
    }
    if (o_sig_table) {
      return usage_error(out, "--sig-table is table-only (no expression source to replay)");
    }
    const auto prog = signal::parse_expression(*o_sig_expr);
    if (!prog.ok()) {
      out << "Error: invalid expression '" << *o_sig_expr << "' (" << prog.error << ")\n";
      out << kUsage << '\n';
      return core::kExitUsage;
    }
    params.expr = prog.value;
  } else if (is_table) {
    if (!o_sig_table) {
      return usage_error(out, "table kind needs --sig-table <csv>");
    }
    if (o_sig_expr) {
      return usage_error(out, "--sig-expr is expression-only (no program in a table source)");
    }
  } else {
    if (o_sig_expr) {
      return usage_error(out, "--sig-expr needs --sig-kind expression");
    }
    if (o_sig_table) {
      return usage_error(out, "--sig-table needs --sig-kind table");
    }
  }

  // Mode subcommand: exactly one `run` positional; all params are named.
  std::array<std::string_view, 2> pos{};
  std::size_t n_pos = 0;
  while (auto p = args.next_positional()) {
    if (n_pos >= pos.size()) {
      return usage_error(out, "too many positional arguments");
    }
    pos[n_pos++] = *p;
  }
  if (args.has_remaining()) {
    return cli::unexpected_error(out, *args.first_remaining(), kUsage);
  }
  if (n_pos != 1 || !is_sig_mode(pos[0])) {
    return usage_error(out, "missing mode (run)");
  }

  if (steps < 1) {
    return usage_error(out, "steps must be >= 1");
  }
  if (!(dt > 0.0)) {
    return usage_error(out, "dt must be > 0");
  }
  if (emit_every < 0) {
    return usage_error(out, "emit stride must be >= 0");
  }
  // Engine re-validates (freq/duty/expression); the CLI reports them as usage errors.
  // Batch runs reject sums: identical batch sources cannot hold distinct
  // per-source operands (the SIG-6 family configures sums directly).
  params.seed = static_cast<std::uint64_t>(sig_seed < 0 ? 0 : sig_seed);
  if (params.kind == signal::SignalKind::kSum) {
    return usage_error(out, "sum needs per-source operands (not supported in batch runs)");
  }
  if (is_table) {
    std::ifstream table_in(std::string{*o_sig_table});
    if (!table_in) {
      out << "Error: cannot open table file '" << *o_sig_table << "'\n";
      return core::kExitRuntime;
    }
    std::string table_error;
    if (!signal::parse_table_csv(table_in, table_points, table_error).ok()) {
      out << "Error: invalid table file '" << *o_sig_table << "': " << table_error << '\n';
      return core::kExitRuntime;
    }
  }
  if (sig_seed < 0 || sig_count < 0 || !signal::validate_params(params).ok()) {
    return usage_error(out, "invalid signal params (freq > 0, duty in 0..1, seed/count >= 0)");
  }

  core::RunConfig cfg;
  cfg.loop.dt = dt;
  cfg.loop.emit_every = static_cast<std::uint64_t>(emit_every);
  cfg.steps = static_cast<std::uint64_t>(steps);
  cfg.solver = solvers::SignalSolver::kName;
  cfg.particles = 0;
  cfg.signal.params = params;
  cfg.signal.count = static_cast<std::uint64_t>(sig_count);
  cfg.signal.set = true;
  cfg.signal.table = std::move(table_points);

  std::optional<emit::SignalsCsvSink> sig_csv;
  if (csv_path) {
    sig_csv.emplace(std::string{*csv_path});
    if (!sig_csv->ok()) {
      out << "Error: cannot open CSV file '" << *csv_path << "'\n";
      return core::kExitRuntime;
    }
  }

  std::optional<link::ShmPublisher> shm;
  if (shm_name) {
    shm.emplace(*shm_name, cfg.solver, &link::frame_from_signal_snapshot);
    if (!shm->ok()) {
      if (shm->status().code == core::StatusCode::kInvalidArgument) {
        return usage_error(out, "invalid SHM segment name (want /name)");
      }
      out << "Error: cannot open SHM segment '" << *shm_name << "'\n";
      return core::kExitRuntime;
    }
  }

  world::State s;
  std::array<emit::ISink *, 2> sink_arr{};
  std::size_t n_sinks = 0;
  if (sig_csv) {
    sink_arr[n_sinks++] = &*sig_csv;
  }
  if (shm) {
    sink_arr[n_sinks++] = &*shm;
  }
  const core::Status st =
      solvers::run_headless(cfg, std::span(sink_arr.data(), n_sinks), s);
  if (!st.ok()) {
    out << "Error: simulation step failed\n";
    return core::kExitRuntime;
  }

  out << "sig run: kind=" << signal::kKindNames[static_cast<std::size_t>(params.kind)]
      << '\n';
  if (is_expr) {
    out << "expr: '" << *o_sig_expr << "' (" << static_cast<unsigned>(params.expr.size)
        << " nodes)\n";
  }
  if (is_table) {
    out << "table: " << *o_sig_table << " (" << cfg.signal.table.size() << " points)\n";
  }
  out << "steps: " << steps << " x " << dt << "s -> t=" << s.sim_time << "s\n";
  out << "signals: " << s.signals.live_count << '\n';
  for (std::uint32_t i = 0; i < s.signals.capacity(); ++i) {
    if (!s.signals.has(i)) {
      continue;
    }
    const std::size_t kind_idx = s.signals.kind[i];
    const std::string_view kind_name =
        kind_idx < signal::kKindNames.size() ? signal::kKindNames[kind_idx] : "?";
    const auto qty = static_cast<signal::QuantityId>(s.signals.quantity[i]);
    out << "s" << s.signals.serial[i] << ": " << kind_name << " value=" << s.signals.value[i]
        << ' ' << signal::quantity_suffix(qty) << '\n';
  }
  if (sig_csv) {
    if (!sig_csv->flush().ok()) {
      out << "Error: failed writing CSV file '" << sig_csv->path() << "'\n";
      return core::kExitRuntime;
    }
    out << "csv: " << sig_csv->path() << " (" << sig_csv->rows_written() << " rows)\n";
  }
  if (shm) {
    if (!shm->ok()) {
      out << "Error: SHM publish failed on '" << shm->name() << "'\n";
      return core::kExitRuntime;
    }
    out << "shm: " << shm->name() << " (" << shm->stats().published << " frames)\n";
  }
  return core::kExitOk;
}

void SigSpec::complete(std::span<const std::string_view> prior_args, std::string_view partial,
                       std::vector<std::string> &out) {
  // Values for a preceding "--sig-kind".
  if (!prior_args.empty() && prior_args.back() == "--sig-kind") {
    for (std::string_view k : signal::kKindNames) {
      if (k.substr(0, partial.size()) == partial) {
        out.emplace_back(k);
      }
    }
    return;
  }
  constexpr std::string_view kKindEq = "--sig-kind=";
  if (partial.substr(0, kKindEq.size()) == kKindEq) {
    const std::string_view sub = partial.substr(kKindEq.size());
    for (std::string_view k : signal::kKindNames) {
      if (k.substr(0, sub.size()) == sub) {
        out.emplace_back(std::string{kKindEq} + std::string{k});
      }
    }
    return;
  }
  // Values for a preceding "--sig-quantity".
  if (!prior_args.empty() && prior_args.back() == "--sig-quantity") {
    for (std::string_view q : {"U", "I"}) {
      if (q.substr(0, partial.size()) == partial) {
        out.emplace_back(q);
      }
    }
    return;
  }
  if (!partial.empty() && partial.front() == '-') {
    push_matches(partial, kSigOptions, out);
    return;
  }
  if (prior_args.empty()) {
    push_matches(partial, kSigModes, out); // first positional = mode
  }
  // All parameter values are numeric/file paths: nothing to complete.
}

} // namespace wengine::cli::commands
