#include "wengine/cli/commands.hpp"
#include "wengine/cli/status.hpp"
#include "wengine/core/run_config.hpp"
#include "wengine/emit/csv_sink.hpp"
#include "wengine/emit/signal_sink.hpp"
#include "wengine/link/frame.hpp"
#include "wengine/link/shm_publisher.hpp"
#include "wengine/link/socket_publisher.hpp"
#include "wengine/signal/types.hpp"
#include "wengine/solvers/configured_run.hpp"
#include "wengine/solvers/paced_run.hpp"
#include "wengine/solvers/run.hpp"
#include "wengine/solvers/solver_registry.hpp"

#include <array>
#include <cstdint>
#include <csignal>
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

volatile std::sig_atomic_t g_sigint_requested = 0;

void request_sigint(int) noexcept { g_sigint_requested = 1; }

bool sigint_requested() noexcept { return g_sigint_requested != 0; }

constexpr std::array<std::string_view, 21> kSimOptions{{
    "--steps", "-n", "--dt", "--solver", "--gravity", "-g", "--particles", "-p", "--x0",
    "--y0", "--vx0", "--vy0", "--emit-csv", "--emit-shm", "--emit-tcp", "--emit-udp",
    "--emit-every", "--paced", "--rate", "--help", "-h",
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
  const bool paced = args.take_flag({"--paced"});
  const auto o_rate = args.take_option({"--rate"});
  std::optional<std::string_view> csv_path;
  std::optional<std::string_view> shm_name;
  std::optional<std::string_view> tcp_endpoint;
  std::optional<std::string_view> udp_endpoint;
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
  tcp_endpoint = args.take_option({"--emit-tcp"});
  udp_endpoint = args.take_option({"--emit-udp"});
  if (auto o = args.take_option({"--solver"})) {
    solver_name = *o;
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
  // Paced mode: explicit opt-in wall-clock spacing (default off = fast
  // batch). --rate is wall steps/s; without it paced means real-time.
  double rate_hz = 0.0;
  if (o_rate && !paced) {
    return usage_error(out, "--rate needs --paced");
  }
  if (paced) {
    if (o_rate) {
      const auto p = parse_frequency(*o_rate);
      if (!p.ok()) {
        out << "Error: invalid wall-clock rate '" << *o_rate << "'\n";
        out << kUsage << '\n';
        return core::kExitUsage;
      }
      rate_hz = p.value;
    } else {
      rate_hz = 1.0 / dt;
    }
    if (!solvers::paced::validate(rate_hz, 1).ok()) {
      return usage_error(out, "invalid wall-clock rate (want finite Hz > 0)");
    }
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
  // NOTE: no solver-scoped options beyond --gravity live here by policy:
  // signal sources are tuned in the sig command. With solver signal, sim
  // runs one default 1 Hz sine (RunConfig defaults) with zero particles.
  const bool is_projectile = sdef->name == solvers::ProjectileSolver::kName;
  if (!is_projectile && has_gravity) {
    return usage_error(out, "--gravity only applies to solver 'projectile'");
  }
  // Domain dispatch for outputs only (no solver vocabulary): signal runs
  // log the signals table and publish signal channels.
  const bool is_signal = sdef->name == solvers::SignalSolver::kName;

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

  // Socket publishers ride the same emit path (best-effort ISinks owned
  // here). Endpoint shape errors are usage errors; connect/DNS failures are
  // runtime errors — same split as SHM names.
  std::optional<link::TcpPublisher> tcp;
  if (tcp_endpoint) {
    if (!link::parse_endpoint(*tcp_endpoint).ok()) {
      return usage_error(out, "invalid TCP endpoint (want host:port)");
    }
    const link::FrameAdapter adapter = is_signal ? &link::frame_from_signal_snapshot
                                                 : &link::frame_from_snapshot;
    tcp.emplace(*tcp_endpoint, solver_name, adapter);
    if (!tcp->ok()) {
      out << "Error: cannot connect TCP '" << *tcp_endpoint << "'\n";
      return core::kExitRuntime;
    }
  }
  std::optional<link::UdpPublisher> udp;
  if (udp_endpoint) {
    if (!link::parse_endpoint(*udp_endpoint).ok()) {
      return usage_error(out, "invalid UDP endpoint (want host:port)");
    }
    const link::FrameAdapter adapter = is_signal ? &link::frame_from_signal_snapshot
                                                 : &link::frame_from_snapshot;
    udp.emplace(*udp_endpoint, solver_name, adapter);
    if (!udp->ok()) {
      out << "Error: cannot open UDP '" << *udp_endpoint << "'\n";
      return core::kExitRuntime;
    }
  }

  // Persistent configured run (PHYSI-35): one build, then either a fast
  // batch or paced chunks against absolute deadlines (PHYSI-36). Sinks stay
  // caller-owned and are attached once; the engine never sleeps.
  solvers::ConfiguredRun run;
  {
    const core::Status built = solvers::ConfiguredRun::build(
        cfg, std::span<core::ISolver *>{}, false, run, nullptr);
    if (!built.ok()) {
      out << "Error: simulation step failed\n";
      return core::kExitRuntime;
    }
  }
  world::State s;
  std::array<emit::ISink *, 5> sink_arr{};
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
  if (tcp) {
    sink_arr[n_sinks++] = &*tcp;
  }
  if (udp) {
    sink_arr[n_sinks++] = &*udp;
  }
  for (std::size_t k = 0; k < n_sinks; ++k) {
    run.add_sink(*sink_arr[k]);
  }
  core::Status st = core::Status::success();
  solvers::paced::Stats paced_stats;
  const std::uint64_t total_steps = static_cast<std::uint64_t>(steps);
  using SignalHandler = void (*)(int);
  SignalHandler previous_sigint = nullptr;
  if (paced) {
    g_sigint_requested = 0;
    previous_sigint = std::signal(SIGINT, request_sigint);
    if (previous_sigint == SIG_ERR) {
      out << "Error: cannot install Ctrl-C handler\n";
      return core::kExitRuntime;
    }
    const auto epoch = solvers::paced::SteadyClock::now();
    auto idle = [&] {
      if (tcp) {
        (void)tcp->service_pending();
      }
    };
    st = solvers::paced::run_paced<solvers::paced::SteadyClock>(
        run, total_steps, rate_hz, 1, epoch, sigint_requested, idle, paced_stats);
    (void)std::signal(SIGINT, previous_sigint);
  } else {
    st = run.run(total_steps);
  }
  s = run.state();
  // Explicit bounded stream finish (PHYSI-32): drain a partially accepted
  // TCP frame without generating a dummy snapshot. The destructor never
  // waits, so an undrained tail would otherwise be abandoned silently.
  core::Status tcp_finish = core::Status::success();
  if (tcp) {
    tcp_finish = tcp->finish(link::kTcpFinishTimeoutMs);
  }
  if (!st.ok()) {
    out << "Error: simulation step failed\n";
    return core::kExitRuntime;
  }

  out << "sim run: solver=" << solver_name;
  if (is_projectile) {
    out << " g=" << gravity;
  }
  out << '\n';
  if (paced && paced_stats.steps_done < total_steps) {
    out << "steps: interrupted by Ctrl-C at " << s.step_index << " x " << dt
        << "s -> t=" << s.sim_time << "s\n";
  } else {
    out << "steps: " << steps << " x " << dt << "s -> t=" << s.sim_time << "s\n";
  }
  if (paced) {
    out << "paced: rate=" << rate_hz << "Hz steps=" << paced_stats.steps_done
        << " late=" << paced_stats.late_steps
        << " max_late_us=" << paced_stats.max_late_s * 1.0e6 << '\n';
  }
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
      out << "s" << s.signals.serial[i] << ": " << kind_name << " value=" << s.signals.value[i]
          << ' ' << signal::quantity_suffix(qty) << '\n';
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
  if (tcp) {
    if (!tcp_finish.ok() || tcp->has_pending_frame() || !tcp->ok()) {
      out << "Error: TCP publish incomplete on '" << tcp->endpoint() << "' (pending_frames="
          << (tcp->has_pending_frame() ? 1 : 0) << " pending_bytes=" << tcp->pending_bytes()
          << " published=" << tcp->stats().published << " dropped="
          << tcp->stats().dropped_send + tcp->stats().dropped_over_bound << " errno="
          << tcp->last_errno() << ")\n";
      return core::kExitRuntime;
    }
    out << "tcp: " << tcp->endpoint() << " (" << tcp->stats().published << " frames, "
        << tcp->stats().dropped_send + tcp->stats().dropped_over_bound << " dropped)\n";
  }
  if (udp) {
    if (!udp->ok()) {
      out << "Error: UDP publish failed on '" << udp->endpoint() << "'\n";
      return core::kExitRuntime;
    }
    out << "udp: " << udp->endpoint() << " (" << udp->stats().published << " frames, "
        << udp->stats().dropped_send + udp->stats().dropped_over_bound << " dropped)\n";
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
