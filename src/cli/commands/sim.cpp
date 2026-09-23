#include "wengine/cli/commands.hpp"
#include "wengine/cli/status.hpp"
#include "wengine/core/run_config.hpp"
#include "wengine/emit/csv_sink.hpp"
#include "wengine/emit/signal_sink.hpp"
#include "wengine/link/frame.hpp"
#include "wengine/link/shm_publisher.hpp"
#include "wengine/signal/types.hpp"
#include "wengine/solvers/run.hpp"
#include "wengine/solvers/solver_registry.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wengine::cli::commands {

namespace {

[[nodiscard]] constexpr bool is_sim_mode(std::string_view s) noexcept {
  return s == "run";
}

int usage_error(std::ostream &out, std::string_view msg) {
  return cli::usage_error(out, msg, SimSpec::kUsage);
}

int unexpected_error(std::ostream &out, std::string_view tok) {
  return cli::unexpected_error(out, tok, SimSpec::kUsage);
}

void push_matches(std::string_view partial, std::span<const std::string_view> options,
                  std::vector<std::string> &out) {
  for (std::string_view o : options) {
    if (o.substr(0, partial.size()) == partial) {
      out.emplace_back(o);
    }
  }
}

constexpr std::array<std::string_view, 1> kSimModes{{"run"}};

constexpr std::array<std::string_view, 26> kSimOptions{{
    "--steps", "-n", "--dt", "--solver", "--gravity", "-g", "--particles", "-p", "--x0",
    "--y0", "--vx0", "--vy0", "--emit-csv", "--emit-shm", "--emit-every", "--help", "-h",
    "--sig-kind", "--sig-freq", "--sig-amp", "--sig-offset", "--sig-phase", "--sig-duty",
    "--sig-seed", "--sig-quantity", "--sig-count",
}};

} // namespace

int SimSpec::run(ArgCursor &args, std::ostream &out) {
  if (args.take_flag({"--help", "-h"})) {
    out << kName << " - " << kDescription << "\nUsage:\n" << kUsage << '\n';
    return core::kExitOk;
  }

  long long steps = 100;
  double dt = 0.01;
  double gravity = 9.81;
  bool has_gravity = false;
  long long n_particles = 1;
  double x0 = 0.0, y0 = 0.0, vx0 = 10.0, vy0 = 10.0;
  long long emit_every = 1;
  std::optional<std::string_view> csv_path;
  std::optional<std::string_view> shm_name;
  std::string_view solver_name = solvers::ProjectileSolver::kName;

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
  auto take_num = [&](std::initializer_list<std::string_view> names, const char *what,
                      double &slot, core::Result<double> (*parse)(std::string_view)) -> bool {
    if (auto o = args.take_option(names)) {
      auto p = parse(*o);
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
  if (!take_num({"--dt"}, "time step", dt, parse_time)) {
    return core::kExitUsage;
  }
  if (auto o = args.take_option({"--gravity", "-g"})) {
    auto p = parse_double(*o);
    if (!p.ok()) {
      out << "Error: invalid gravity '" << *o << "'\n";
      out << kUsage << '\n';
      return core::kExitUsage;
    }
    gravity = p.value;
    has_gravity = true;
  }
  const auto o_particles = args.take_option({"--particles", "-p"});
  const bool particles_given = o_particles.has_value();
  if (o_particles) {
    const auto p = parse_i64(*o_particles);
    if (!p.ok()) {
      out << "Error: invalid particle count '" << *o_particles << "'\n";
      out << kUsage << '\n';
      return core::kExitUsage;
    }
    n_particles = p.value;
  }
  if (!take_num({"--x0"}, "x0", x0, parse_double)) {
    return core::kExitUsage;
  }
  if (!take_num({"--y0"}, "y0", y0, parse_double)) {
    return core::kExitUsage;
  }
  if (!take_num({"--vx0"}, "vx0", vx0, parse_double)) {
    return core::kExitUsage;
  }
  if (!take_num({"--vy0"}, "vy0", vy0, parse_double)) {
    return core::kExitUsage;
  }
  if (!take_i64({"--emit-every"}, "emit stride", emit_every)) {
    return core::kExitUsage;
  }
  csv_path = args.take_option({"--emit-csv"});
  shm_name = args.take_option({"--emit-shm"});
  if (auto o = args.take_option({"--solver"})) {
    solver_name = *o;
  }

  // Signal-scoped options (raw first: amp/offset parse against --sig-quantity).
  const auto o_sig_kind = args.take_option({"--sig-kind"});
  const auto o_sig_freq = args.take_option({"--sig-freq"});
  const auto o_sig_amp = args.take_option({"--sig-amp"});
  const auto o_sig_offset = args.take_option({"--sig-offset"});
  const auto o_sig_phase = args.take_option({"--sig-phase"});
  const auto o_sig_duty = args.take_option({"--sig-duty"});
  const auto o_sig_seed = args.take_option({"--sig-seed"});
  const auto o_sig_quantity = args.take_option({"--sig-quantity"});
  const auto o_sig_count = args.take_option({"--sig-count"});
  const bool has_signal_opts = o_sig_kind || o_sig_freq || o_sig_amp || o_sig_offset ||
                               o_sig_phase || o_sig_duty || o_sig_seed || o_sig_quantity ||
                               o_sig_count;
  std::string_view sig_kind = "sine";
  std::string_view sig_quantity = "U";
  double sig_freq = 1.0;
  double sig_amp = 1.0;
  double sig_offset = 0.0;
  double sig_phase = 0.0;
  double sig_duty = 0.5;
  long long sig_seed = 1;
  long long sig_count = 1;
  if (o_sig_kind) {
    sig_kind = *o_sig_kind;
  }
  if (o_sig_quantity) {
    sig_quantity = *o_sig_quantity;
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
  if (n_pos != 1 || !is_sim_mode(pos[0])) {
    if (n_pos > 1 && is_sim_mode(pos[0])) {
      return unexpected_error(out, pos[1]); // all params are named
    }
    return usage_error(out, "missing mode (run)");
  }

  if (steps < 1) {
    return usage_error(out, "steps must be >= 1");
  }
  if (!(dt > 0.0)) {
    return usage_error(out, "dt must be > 0");
  }
  if (n_particles < 0) {
    return usage_error(out, "particle count must be >= 0");
  }
  if (emit_every < 0) {
    return usage_error(out, "emit stride must be >= 0");
  }

  const solvers::SolverDef *sdef = solvers::SolverRegistry::find(solver_name);
  if (sdef == nullptr) {
    out << "Error: unknown solver '" << solver_name << "'. Available:";
    for (const auto &d : solvers::SolverRegistry::all()) {
      out << ' ' << d.name;
    }
    out << '\n';
    return core::kExitUsage;
  }
  // --gravity applies to the projectile solver specifically. This message
  // stays CLI-owned; the engine driver re-validates for all frontends.
  const bool is_projectile = sdef->name == solvers::ProjectileSolver::kName;
  if (!is_projectile && has_gravity) {
    return usage_error(out, "--gravity only applies to solver 'projectile'");
  }
  // --sig-* apply to the signal solver specifically (same pattern).
  const bool is_signal = sdef->name == solvers::SignalSolver::kName;
  if (!is_signal && has_signal_opts) {
    return usage_error(out, "--sig-* only applies to solver 'signal'");
  }
  signal::SignalKind sig_kind_parsed = signal::SignalKind::kSine;
  signal::QuantityId sig_qty_parsed = signal::QuantityId::kVoltage;
  if (is_signal) {
    const auto kind = signal::parse_kind(sig_kind);
    if (!kind.ok()) {
      return usage_error(out, "unknown signal kind (want sine|square|triangle|saw|dc|pulse|noise)");
    }
    sig_kind_parsed = kind.value;
    const auto qty = signal::parse_quantity(sig_quantity);
    if (!qty.ok()) {
      return usage_error(out, "unknown signal quantity (want U|I)");
    }
    sig_qty_parsed = qty.value;
    // Amp/offset parse against the stamped quantity (V or A suffixes).
    auto take_qty = [&](std::optional<std::string_view> o, const char *what,
                        double &slot) -> bool {
      if (o) {
        const auto p = (sig_qty_parsed == signal::QuantityId::kCurrent)
                           ? parse_current(*o)
                           : parse_voltage(*o);
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
      sig_freq = p.value;
    }
    if (!take_qty(o_sig_amp, "signal amplitude", sig_amp) ||
        !take_qty(o_sig_offset, "signal offset", sig_offset)) {
      return core::kExitUsage;
    }
    if (o_sig_phase) {
      const auto p = parse_double(*o_sig_phase);
      if (!p.ok()) {
        out << "Error: invalid signal phase '" << *o_sig_phase << "'\n";
        out << kUsage << '\n';
        return core::kExitUsage;
      }
      sig_phase = p.value;
    }
    if (o_sig_duty) {
      const auto p = parse_double(*o_sig_duty);
      if (!p.ok()) {
        out << "Error: invalid signal duty '" << *o_sig_duty << "'\n";
        out << kUsage << '\n';
        return core::kExitUsage;
      }
      sig_duty = p.value;
    }
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
    // Engine re-validates (freq/duty); the CLI reports them as usage errors.
    signal::SignalParams check{sig_kind_parsed,
                               sig_qty_parsed,
                               sig_freq,
                               sig_amp,
                               sig_offset,
                               sig_phase,
                               sig_duty,
                               static_cast<std::uint64_t>(sig_seed < 0 ? 0 : sig_seed)};
    if (sig_seed < 0 || sig_count < 0 || !signal::validate_params(check).ok()) {
      return usage_error(out, "invalid signal params (freq > 0, duty in 0..1, seed/count >= 0)");
    }
  }

  // Headless run goes through the engine seam: the CLI builds a RunConfig
  // (plain data, no engine wiring here) and the driver owns the rest.
  // Signal runs default to zero particles unless explicitly requested.
  if (is_signal && !particles_given) {
    n_particles = 0;
  }
  core::RunConfig cfg;
  cfg.loop.dt = dt;
  cfg.loop.emit_every = static_cast<std::uint64_t>(emit_every);
  cfg.steps = static_cast<std::uint64_t>(steps);
  cfg.solver = solver_name;
  cfg.gravity = gravity;
  cfg.has_gravity = has_gravity;
  cfg.particles = static_cast<std::uint64_t>(n_particles);
  cfg.x0 = x0;
  cfg.y0 = y0;
  cfg.vx0 = vx0;
  cfg.vy0 = vy0;
  cfg.sig_kind = sig_kind;
  cfg.sig_freq = sig_freq;
  cfg.sig_amp = sig_amp;
  cfg.sig_offset = sig_offset;
  cfg.sig_phase = sig_phase;
  cfg.sig_duty = sig_duty;
  cfg.sig_seed = static_cast<std::uint64_t>(sig_seed);
  cfg.sig_quantity = sig_quantity;
  cfg.signals = static_cast<std::uint64_t>(sig_count);
  cfg.has_signal_opts = has_signal_opts;

  // Signal runs log the signals table (D2b-A); particle runs keep CsvSink.
  std::optional<emit::CsvSink> csv;
  std::optional<emit::SignalsCsvSink> sig_csv;
  if (csv_path) {
    if (is_signal) {
      sig_csv.emplace(std::string{*csv_path});
      if (!sig_csv->ok()) {
        out << "Error: cannot open CSV file '" << *csv_path << "'\n";
        return core::kExitRuntime;
      }
    } else {
      csv.emplace(std::string{*csv_path});
      if (!csv->ok()) {
        out << "Error: cannot open CSV file '" << *csv_path << "'\n";
        return core::kExitRuntime;
      }
    }
  }

  // SHM publish rides the same emit path as CSV (both are best-effort
  // ISinks owned here, never in RunConfig). Domain tag = solver name so
  // readers pick the matching adapter; the signal solver publishes through
  // the signal Frame adapter (same class, no subclassing, no engine change).
  std::optional<link::ShmPublisher> shm;
  if (shm_name) {
    const link::FrameAdapter adapter = is_signal ? &link::frame_from_signal_snapshot
                                                 : &link::frame_from_snapshot;
    shm.emplace(*shm_name, solver_name, adapter);
    if (!shm->ok()) {
      if (shm->status().code == core::StatusCode::kInvalidArgument) {
        return usage_error(out, "invalid SHM segment name (want /name)");
      }
      out << "Error: cannot open SHM segment '" << *shm_name << "'\n";
      return core::kExitRuntime;
    }
  }

  world::State s;
  std::array<emit::ISink *, 3> sink_arr{};
  std::size_t n_sinks = 0;
  if (csv) {
    sink_arr[n_sinks++] = &*csv;
  }
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

  out << "sim run: solver=" << solver_name;
  if (is_projectile) {
    out << " g=" << gravity;
  }
  if (is_signal) {
    out << " kind=" << sig_kind;
  }
  out << '\n';
  out << "steps: " << steps << " x " << dt << "s -> t=" << s.sim_time << "s\n";
  out << "particles: " << s.particles.live_count << '\n';
  if (s.particles.live_count > 0) {
    std::uint32_t first = 0;
    while (!s.particles.has(first)) {
      ++first;
    }
    out << "p0: x=" << s.particles.pos_x[first] << " y=" << s.particles.pos_y[first]
        << " vx=" << s.particles.vel_x[first] << " vy=" << s.particles.vel_y[first] << '\n';
  }
  if (is_signal) {
    out << "signals: " << s.signals.live_count << '\n';
    for (std::uint32_t i = 0; i < s.signals.capacity(); ++i) {
      if (!s.signals.has(i)) {
        continue;
      }
      const std::size_t kind_idx = s.signals.kind[i];
      const std::string_view kind_name =
          kind_idx < signal::kKindNames.size() ? signal::kKindNames[kind_idx] : "?";
      const auto qty = static_cast<signal::QuantityId>(s.signals.quantity[i]);
      out << "s" << i << ": " << kind_name << " value=" << s.signals.value[i] << ' '
          << signal::quantity_suffix(qty) << '\n';
    }
  }
  if (csv) {
    if (!csv->flush().ok()) {
      out << "Error: failed writing CSV file '" << csv->path() << "'\n";
      return core::kExitRuntime;
    }
    out << "csv: " << csv->path() << " (" << csv->rows_written() << " rows)\n";
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

void SimSpec::complete(std::span<const std::string_view> prior_args, std::string_view partial,
                       std::vector<std::string> &out) {
  // Values for a preceding "--solver".
  if (!prior_args.empty() && prior_args.back() == "--solver") {
    for (const auto &d : solvers::SolverRegistry::all()) {
      if (d.name.substr(0, partial.size()) == partial) {
        out.emplace_back(d.name);
      }
    }
    return;
  }
  constexpr std::string_view kSolverEq = "--solver=";
  if (partial.substr(0, kSolverEq.size()) == kSolverEq) {
    const std::string_view sub = partial.substr(kSolverEq.size());
    for (const auto &d : solvers::SolverRegistry::all()) {
      if (d.name.substr(0, sub.size()) == sub) {
        out.emplace_back(std::string{kSolverEq} + std::string{d.name});
      }
    }
    return;
  }
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
    push_matches(partial, kSimOptions, out);
    return;
  }
  if (prior_args.empty()) {
    push_matches(partial, kSimModes, out); // first positional = mode
  }
  // All parameter values are numeric/file paths: nothing to complete.
}

} // namespace wengine::cli::commands
