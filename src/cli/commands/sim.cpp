#include "wengine/cli/cancel.hpp"
#include "wengine/cli/commands.hpp"
#include "wengine/cli/emit_options.hpp"
#include "wengine/cli/status.hpp"
#include "wengine/core/run_config.hpp"
#include "wengine/emit/csv_sink.hpp"
#include "wengine/emit/routing.hpp"
#include "wengine/emit/signal_sink.hpp"
#include "wengine/link/frame.hpp"
#include "wengine/link/output_report.hpp"
#include "wengine/link/protocol.hpp"
#include "wengine/link/shm_publisher.hpp"
#include "wengine/link/socket_publisher.hpp"
#include "wengine/link/uart_publisher.hpp"
#include "wengine/signal/types.hpp"
#include "wengine/solvers/configured_run.hpp"
#include "wengine/solvers/paced_run.hpp"
#include "wengine/solvers/run.hpp"
#include "wengine/solvers/solver_registry.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <csignal>
#include <iostream>
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

// TUI-5 async stop: the worker sets the global cancel atom while the UI
// stays live; headless runs never set it. Checked at step boundaries, so
// interruption never leaves a partially advanced step.
bool combined_stop() noexcept {
  return sigint_requested() || cli::cancel_requested();
}

constexpr std::array<std::string_view, 35> kSimOptions{{
    "--steps", "-n", "--dt", "--solver", "--gravity", "-g", "--particles", "-p", "--x0",
    "--y0", "--vx0", "--vy0", "--emit-csv", "--emit-shm", "--emit-tcp", "--emit-udp",
    "--emit-every", "--emit-csv-stride", "--emit-shm-stride", "--emit-tcp-stride",
    "--emit-udp-stride", "--emit-udp-budget", "--emit-csv-select", "--emit-shm-select",
    "--emit-tcp-select", "--emit-udp-select", "--emit-uart", "--emit-uart-mode",
    "--emit-uart-stride", "--emit-uart-select", "--emit-health", "--paced", "--rate",
    "--help", "-h",
}};

// Chunked live-output health (PHYSI-39): fast batches step in bounded
// chunks so a mid-run sticky failure or backpressure burst surfaces on
// stderr while the sim continues (telemetry never alters numerics).
// Chunked run(N) accumulation is bit-identical to one run(total).
inline constexpr std::uint64_t kHealthChunkSteps = 4096;

void print_live_health(std::uint64_t done_steps,
                       const std::optional<link::ShmPublisher> &shm,
                       const std::optional<link::TcpPublisher> &tcp,
                       const std::optional<link::UdpPublisher> &udp,
                       const link::uart::UartPublisher *uart, double elapsed_s,
                       double sample_rate_hz) {
  std::cerr << "live: steps=" << done_steps << " sample_rate=" << sample_rate_hz
            << "Hz elapsed=" << elapsed_s << "s";
  if (elapsed_s > 0.0) {
    if (shm) {
      std::cerr << " shm_bytes_s=" << static_cast<double>(shm->stats().bytes_accepted) / elapsed_s;
    }
    if (tcp) {
      std::cerr << " tcp_bytes_s=" << static_cast<double>(tcp->stats().bytes_accepted) / elapsed_s;
    }
    if (udp) {
      std::cerr << " udp_bytes_s=" << static_cast<double>(udp->stats().bytes_accepted) / elapsed_s;
    }
    if (uart != nullptr) {
      std::cerr << " uart_bytes_s=" << static_cast<double>(uart->stats().bytes_accepted) / elapsed_s;
    }
  }
  std::cerr << '\n';
  if (shm) {
    std::cerr << "live shm: "
              << link::format_publisher_health(shm->stats(), shm->status(), shm->last_errno(), 0, 0, 0)
              << '\n';
  }
  if (tcp) {
    const std::uint64_t pending = tcp->has_pending_frame() ? 1 : 0;
    std::cerr << "live tcp: "
              << link::format_publisher_health(tcp->stats(), tcp->status(), tcp->last_errno(), pending, tcp->pending_bytes(), pending)
              << '\n';
  }
  if (udp) {
    std::cerr << "live udp: "
              << link::format_publisher_health(udp->stats(), udp->status(), udp->last_errno(), 0, 0, 0)
              << '\n';
  }
  if (uart != nullptr) {
    std::cerr << "live uart: "
              << link::format_publisher_health(uart->stats(), uart->status(), uart->last_errno(), 0, 0, 0)
              << '\n';
  }
}

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
  const bool emit_health = args.take_flag({"--emit-health"});
  const auto o_rate = args.take_option({"--rate"});
  std::optional<std::string_view> csv_path;
  std::optional<std::string_view> shm_name;
  std::optional<std::string_view> tcp_endpoint;
  std::optional<std::string_view> udp_endpoint;
  std::optional<std::string_view> uart_device;
  std::optional<std::string_view> uart_mode_text;
  long long udp_budget = static_cast<long long>(link::kDefaultUdpPayloadBudget);
  bool udp_budget_given = false;
  std::string_view solver_name = solvers::ProjectileSolver::kName;

  auto take_i64 = [&](std::initializer_list<std::string_view> names, const char *what, long long &slot) -> bool {
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
  uart_device = args.take_option({"--emit-uart"});
  uart_mode_text = args.take_option({"--emit-uart-mode"});
  cli::EmitOutputSampling csv_sampling;
  cli::EmitOutputSampling shm_sampling;
  cli::EmitOutputSampling tcp_sampling;
  cli::EmitOutputSampling udp_sampling;
  cli::EmitOutputSampling uart_sampling;
  auto take_stride = [&](std::initializer_list<std::string_view> names, const char *what, cli::EmitOutputSampling &slot) -> bool {
    if (auto o = args.take_option(names)) {
      const auto p = cli::parse_emit_stride(*o);
      if (!p.ok()) {
        out << "Error: invalid " << what << " '" << *o << "' (want >= 0, 0 = muted)\n";
        out << kUsage << '\n';
        return false;
      }
      slot.stride = p.value;
      slot.stride_given = true;
    }
    return true;
  };
  auto take_select = [&](std::initializer_list<std::string_view> names, const char *what, cli::EmitOutputSampling &slot) -> bool {
    if (auto o = args.take_option(names)) {
      const auto p = cli::parse_emit_select(*o);
      if (!p.ok()) {
        out << "Error: invalid " << what << " '" << *o << "' (want e.g. 0,2,5)\n";
        out << kUsage << '\n';
        return false;
      }
      slot.serials = p.value;
      slot.select_given = true;
    }
    return true;
  };
  if (!take_stride({"--emit-csv-stride"}, "csv stride", csv_sampling) ||
      !take_stride({"--emit-shm-stride"}, "shm stride", shm_sampling) ||
      !take_stride({"--emit-tcp-stride"}, "tcp stride", tcp_sampling) ||
      !take_stride({"--emit-udp-stride"}, "udp stride", udp_sampling) ||
      !take_stride({"--emit-uart-stride"}, "uart stride", uart_sampling)) {
    return core::kExitUsage;
  }
  if (auto o = args.take_option({"--emit-udp-budget"})) {
    const auto p = parse_i64(*o);
    if (!p.ok() || p.value < 1 ||
        static_cast<std::uint64_t>(p.value) > link::kMaxUdpPayload) {
      out << "Error: invalid udp budget '" << *o << "' (want 1.."
          << link::kMaxUdpPayload << ")\n";
      out << kUsage << '\n';
      return core::kExitUsage;
    }
    udp_budget = p.value;
    udp_budget_given = true;
  }
  if (!take_select({"--emit-csv-select"}, "csv selection", csv_sampling) ||
      !take_select({"--emit-shm-select"}, "shm selection", shm_sampling) ||
      !take_select({"--emit-tcp-select"}, "tcp selection", tcp_sampling) ||
      !take_select({"--emit-udp-select"}, "udp selection", udp_sampling) ||
      !take_select({"--emit-uart-select"}, "uart selection", uart_sampling)) {
    return core::kExitUsage;
  }
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
  // Bare parse_double options accept inf/nan text (from_chars extension),
  // so finiteness is enforced here at the CLI edge; the configured-run
  // builder re-validates for every frontend (see configured_run.hpp).
  if (has_gravity && !std::isfinite(gravity)) {
    return usage_error(out, "gravity must be finite");
  }
  if (n_particles > 0 && (!std::isfinite(x0) || !std::isfinite(y0) || !std::isfinite(vx0) || !std::isfinite(vy0))) {
    return usage_error(out, "initial conditions must be finite");
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

  // Per-output sampling validation (PHYSI-34): a stride/select without its
  // output is a usage error, never silently ignored; selections need the
  // signal solver (stable serials exist only there).
  if (csv_sampling.stride_given && !csv_path) {
    return usage_error(out, "--emit-csv-stride needs --emit-csv");
  }
  if (shm_sampling.stride_given && !shm_name) {
    return usage_error(out, "--emit-shm-stride needs --emit-shm");
  }
  if (tcp_sampling.stride_given && !tcp_endpoint) {
    return usage_error(out, "--emit-tcp-stride needs --emit-tcp");
  }
  if (udp_sampling.stride_given && !udp_endpoint) {
    return usage_error(out, "--emit-udp-stride needs --emit-udp");
  }
  if (uart_sampling.stride_given && !uart_device) {
    return usage_error(out, "--emit-uart-stride needs --emit-uart");
  }
  if (udp_budget_given && !udp_endpoint) {
    return usage_error(out, "--emit-udp-budget needs --emit-udp");
  }
  if (csv_sampling.select_given && !csv_path) {
    return usage_error(out, "--emit-csv-select needs --emit-csv");
  }
  if (shm_sampling.select_given && !shm_name) {
    return usage_error(out, "--emit-shm-select needs --emit-shm");
  }
  if (tcp_sampling.select_given && !tcp_endpoint) {
    return usage_error(out, "--emit-tcp-select needs --emit-tcp");
  }
  if (udp_sampling.select_given && !udp_endpoint) {
    return usage_error(out, "--emit-udp-select needs --emit-udp");
  }
  if (uart_sampling.select_given && !uart_device) {
    return usage_error(out, "--emit-uart-select needs --emit-uart");
  }
  // UART carries signal rows only: with any other solver there is nothing
  // meaningful to write (refuse loudly instead of emitting zero lines).
  if (uart_device && !is_signal) {
    return usage_error(out, "--emit-uart needs solver 'signal'");
  }
  // UART wire mode is explicit (never silent); a mode without its device
  // is a usage error.
  link::uart::UartMode uart_mode = link::uart::UartMode::kAscii;
  if (uart_mode_text) {
    if (!uart_device) {
      return usage_error(out, "--emit-uart-mode needs --emit-uart");
    }
    const auto m = link::uart::parse_mode(*uart_mode_text);
    if (!m.ok()) {
      return usage_error(out, "unknown uart mode (want ascii|cobs)");
    }
    uart_mode = m.value;
  }
  if (!is_signal && (csv_sampling.select_given || shm_sampling.select_given ||
                     tcp_sampling.select_given || udp_sampling.select_given ||
                     uart_sampling.select_given)) {
    return usage_error(out, "--emit-<out>-select needs solver 'signal'");
  }
  // Known transport capacity before running: signal frames carry sig.count
  // plus one channel per selected source (1 + N <= 64); particle frames carry
  // <= 6 channels but one item per particle (<= 1024). CSV is unbounded.
  if (is_signal) {
    // sim signal runs one default sine (serial 0); a select list addresses it.
    // UART lines are unbounded (no frame cap), so only the serial range
    // applies — no channels-fit check like the frame transports.
    const cli::EmitOutputSampling *outs[4] = {&csv_sampling, &shm_sampling, &tcp_sampling, &udp_sampling};
    const char *names[4] = {"csv", "shm", "tcp", "udp"};
    const bool requested[4] = {csv_path.has_value(), shm_name.has_value(), tcp_endpoint.has_value(), udp_endpoint.has_value()};
    for (int k = 0; k < 4; ++k) {
      if (!outs[k]->select_given) {
        continue;
      }
      for (std::uint64_t id : outs[k]->serials) {
        if (id != 0) {
          out << "Error: --emit-" << names[k] << "-select serial " << id
              << " out of range (sim signal runs one source, serial 0)\n";
          out << kUsage << '\n';
          return core::kExitUsage;
        }
      }
    }
    if (uart_sampling.select_given) {
      for (std::uint64_t id : uart_sampling.serials) {
        if (id != 0) {
          out << "Error: --emit-uart-select serial " << id
              << " out of range (sim signal runs one source, serial 0)\n";
          out << kUsage << '\n';
          return core::kExitUsage;
        }
      }
    }
    for (int k = 1; k < 4; ++k) {  // transports only (csv unbounded)
      if (!requested[k] || !outs[k]->select_given) {
        continue;
      }
      if (!cli::signal_channels_fit(outs[k]->serials.size())) {
        out << "Error: --emit-" << names[k] << "-select needs "
            << (outs[k]->serials.size() + 1) << " channels, transport holds "
            << link::kMaxChannels << '\n';
        out << kUsage << '\n';
        return core::kExitUsage;
      }
    }
    if (!requested[1] && !requested[2] && !requested[3]) {
      // No transport: nothing to validate (single default source always fits).
    } else if (!csv_sampling.select_given && !shm_sampling.select_given &&
               !tcp_sampling.select_given && !udp_sampling.select_given) {
      // Unfiltered single-source signal run: 2 channels, always fits.
    }
  } else if (shm_name || tcp_endpoint || udp_endpoint) {
    if (n_particles > static_cast<long long>(link::kMaxItems)) {
      out << "Error: " << n_particles << " particles need " << n_particles
          << " items, transport holds " << link::kMaxItems << '\n';
      out << kUsage << '\n';
      return core::kExitUsage;
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
    udp.emplace(*udp_endpoint, solver_name, adapter,
                static_cast<std::size_t>(udp_budget));
    if (!udp->ok()) {
      out << "Error: cannot open UDP '" << *udp_endpoint << "'\n";
      return core::kExitRuntime;
    }
  }
  // UART serial publisher (SIG-8 HIL track, signal rows only — gated to
  // the signal solver above). Device shape errors are usage errors;
  // open/configure failures are runtime errors, like the sockets.
  std::optional<link::uart::UartPort> uart_port;
  std::optional<link::uart::UartPublisher> uart;
  if (uart_device) {
    const auto ep = link::uart::parse_uart_endpoint(*uart_device);
    if (!ep.ok()) {
      return usage_error(out, "invalid UART device (want device[,baud])");
    }
    uart_port.emplace(ep.value.device, ep.value.baud);
    if (!uart_port->ok()) {
      if (uart_port->status().code == core::StatusCode::kInvalidArgument) {
        return usage_error(out, "invalid UART device (want device[,baud])");
      }
      out << "Error: cannot open UART '" << ep.value.device << "'\n";
      return core::kExitRuntime;
    }
    uart.emplace(*uart_port, uart_mode);
    if (!uart->ok()) {
      out << "Error: cannot open UART '" << ep.value.device << "'\n";
      return core::kExitRuntime;
    }
  }

  // Per-output sampling wrappers (PHYSI-34, edge-side only — Runner keeps
  // its single global emit_every gate; each output further selects via
  // StrideSink step%stride==0 and/or SignalSubsetSink serial allowlists).
  // Chain per output: publisher <- Subset (if select) <- Stride (if != 1).
  // Wrappers borrow their inner sink and outlive the run; TCP servicing and
  // finish() stay on the inner publisher. Counters make drops loud.
  std::optional<emit::SignalSubsetSink> csv_subset;
  std::optional<emit::StrideSink> csv_stride;
  std::optional<emit::SignalSubsetSink> sig_csv_subset;
  std::optional<emit::StrideSink> sig_csv_stride;
  std::optional<emit::SignalSubsetSink> shm_subset;
  std::optional<emit::StrideSink> shm_stride;
  std::optional<emit::SignalSubsetSink> tcp_subset;
  std::optional<emit::StrideSink> tcp_stride;
  std::optional<emit::SignalSubsetSink> udp_subset;
  std::optional<emit::StrideSink> udp_stride;
  std::optional<emit::SignalSubsetSink> uart_subset;
  std::optional<emit::StrideSink> uart_stride;
  emit::ISink *csv_out = nullptr;
  emit::ISink *sig_csv_out = nullptr;
  emit::ISink *shm_out = nullptr;
  emit::ISink *tcp_out = nullptr;
  emit::ISink *udp_out = nullptr;
  emit::ISink *uart_out = nullptr;
  if (csv) {
    csv_out = &*csv;
    if (csv_sampling.stride_given && csv_sampling.stride != 1) {
      csv_stride.emplace(*csv_out, csv_sampling.stride);
      csv_out = &*csv_stride;
    }
  }
  if (sig_csv) {
    sig_csv_out = &*sig_csv;
    if (csv_sampling.select_given) {
      sig_csv_subset.emplace(*sig_csv_out);
      sig_csv_subset->set_subscription(csv_sampling.serials);
      sig_csv_out = &*sig_csv_subset;
    }
    if (csv_sampling.stride_given && csv_sampling.stride != 1) {
      sig_csv_stride.emplace(*sig_csv_out, csv_sampling.stride);
      sig_csv_out = &*sig_csv_stride;
    }
  }
  if (shm) {
    shm_out = &*shm;
    if (is_signal && shm_sampling.select_given) {
      shm_subset.emplace(*shm_out);
      shm_subset->set_subscription(shm_sampling.serials);
      shm_out = &*shm_subset;
    }
    if (shm_sampling.stride_given && shm_sampling.stride != 1) {
      shm_stride.emplace(*shm_out, shm_sampling.stride);
      shm_out = &*shm_stride;
    }
  }
  if (tcp) {
    tcp_out = &*tcp;
    if (is_signal && tcp_sampling.select_given) {
      tcp_subset.emplace(*tcp_out);
      tcp_subset->set_subscription(tcp_sampling.serials);
      tcp_out = &*tcp_subset;
    }
    if (tcp_sampling.stride_given && tcp_sampling.stride != 1) {
      tcp_stride.emplace(*tcp_out, tcp_sampling.stride);
      tcp_out = &*tcp_stride;
    }
  }
  if (udp) {
    udp_out = &*udp;
    if (is_signal && udp_sampling.select_given) {
      udp_subset.emplace(*udp_out);
      udp_subset->set_subscription(udp_sampling.serials);
      udp_out = &*udp_subset;
    }
    if (udp_sampling.stride_given && udp_sampling.stride != 1) {
      udp_stride.emplace(*udp_out, udp_sampling.stride);
      udp_out = &*udp_stride;
    }
  }
  if (uart) {
    uart_out = &*uart;
    if (is_signal && uart_sampling.select_given) {
      uart_subset.emplace(*uart_out);
      uart_subset->set_subscription(uart_sampling.serials);
      uart_out = &*uart_subset;
    }
    if (uart_sampling.stride_given && uart_sampling.stride != 1) {
      uart_stride.emplace(*uart_out, uart_sampling.stride);
      uart_out = &*uart_stride;
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
  std::array<emit::ISink *, 6> sink_arr{};
  std::size_t n_sinks = 0;
  if (csv_out) {
    sink_arr[n_sinks++] = csv_out;
  }
  if (sig_csv_out) {
    sink_arr[n_sinks++] = sig_csv_out;
  }
  if (shm_out) {
    sink_arr[n_sinks++] = shm_out;
  }
  if (tcp_out) {
    sink_arr[n_sinks++] = tcp_out;
  }
  if (udp_out) {
    sink_arr[n_sinks++] = udp_out;
  }
  if (uart_out) {
    sink_arr[n_sinks++] = uart_out;
  }
  for (std::size_t k = 0; k < n_sinks; ++k) {
    run.add_sink(*sink_arr[k]);
  }
  core::Status st = core::Status::success();
  solvers::paced::Stats paced_stats;
  const std::uint64_t total_steps = static_cast<std::uint64_t>(steps);
  const link::uart::UartPublisher *uart_live = uart ? &*uart : nullptr;
  const bool has_live = shm || tcp || udp || uart;
  const double sample_rate_hz =
      (dt > 0.0 && emit_every > 0)
          ? 1.0 / (dt * static_cast<double>(emit_every))
          : 0.0;
  const auto wall_start = std::chrono::steady_clock::now();
  auto wall_elapsed_s = [&] {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                         wall_start)
        .count();
  };
  using SignalHandler = void (*)(int);
  SignalHandler previous_sigint = nullptr;
  cli::progress_begin(total_steps);
  std::uint64_t done = 0;
  if (paced) {
    g_sigint_requested = 0;
    previous_sigint = std::signal(SIGINT, request_sigint);
    if (previous_sigint == SIG_ERR) {
      cli::progress_end();
      out << "Error: cannot install Ctrl-C handler\n";
      return core::kExitRuntime;
    }
    const auto epoch = solvers::paced::SteadyClock::now();
    auto last_report = std::chrono::steady_clock::now();
    auto idle = [&] {
      if (tcp) {
        (void)tcp->service_pending();
      }
      if (emit_health && has_live) {
        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now - last_report).count() >= 1.0) {
          last_report = now;
          print_live_health(run.state().step_index, shm, tcp, udp, uart_live,
                            wall_elapsed_s(), sample_rate_hz);
        }
      }
    };
    auto before_chunk = [&](std::uint64_t d) { cli::progress_update(d); };
    st = solvers::paced::run_paced<solvers::paced::SteadyClock>(run, total_steps, rate_hz, 1, epoch, combined_stop, idle, paced_stats, before_chunk);
    (void)std::signal(SIGINT, previous_sigint);
    done = paced_stats.steps_done;
    cli::progress_update(done);
  } else {
    // Chunked fast batch: bit-identical to one run(total_steps) while
    // surfacing mid-run sticky/backpressure/over-bound on stderr for long
    // runs when --emit-health is passed (PHYSI-39). Short runs and default
    // runs stay silent beyond the final summary. The TUI async worker
    // (TUI-5) stops here at a step boundary via the global cancel atom.
    while (done < total_steps) {
      if (cli::cancel_requested()) {
        break;
      }
      std::uint64_t n = kHealthChunkSteps;
      if (n > total_steps - done) {
        n = total_steps - done;
      }
      st = run.run(n, &combined_stop);
      if (!st.ok()) {
        break;
      }
      done = run.state().step_index;
      cli::progress_update(done);
      if (emit_health && has_live && done < total_steps) {
        print_live_health(done, shm, tcp, udp, uart_live, wall_elapsed_s(), sample_rate_hz);
      }
    }
  }
  cli::progress_end();
  s = run.state();
  // Explicit bounded stream finish (PHYSI-32): drain a partially accepted
  // TCP frame without generating a dummy snapshot. The destructor never
  // waits, so an undrained tail would otherwise be abandoned silently.
  core::Status tcp_finish = core::Status::success();
  if (tcp) {
    tcp_finish = tcp->finish(link::kTcpFinishTimeoutMs);
  }
  if (!st.ok()) {
    out << "Error: simulation step failed (partial step state is not a completed tick)\n";
    return core::kExitRuntime;
  }

  out << "sim run: solver=" << solver_name;
  if (is_projectile) {
    out << " g=" << gravity;
  }
  out << '\n';
  if (done < total_steps) {
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
    out << "csv: " << csv->path() << " (" << csv->rows_written() << " rows";
    if (csv_stride) {
      out << ", stride=" << csv_stride->stride() << " matched=" << csv_stride->matched()
          << " dropped=" << csv_stride->dropped();
    }
    out << ")\n";
  }
  if (sig_csv) {
    if (!sig_csv->flush().ok()) {
      out << "Error: failed writing CSV file '" << sig_csv->path() << "'\n";
      return core::kExitRuntime;
    }
    out << "csv: " << sig_csv->path() << " (" << sig_csv->rows_written() << " rows";
    if (csv_sampling.select_given) {
      out << ", select=" << cli::format_emit_select(csv_sampling.serials);
      if (sig_csv_subset) {
        out << " matched=" << sig_csv_subset->matched()
            << " dropped=" << sig_csv_subset->dropped()
            << " forwarded_rows=" << sig_csv_subset->forwarded_rows();
      }
    }
    if (sig_csv_stride) {
      out << ", stride=" << sig_csv_stride->stride() << " matched=" << sig_csv_stride->matched()
          << " dropped=" << sig_csv_stride->dropped();
    }
    out << ")\n";
  }
  if (shm) {
    if (!shm->ok()) {
      out << "Error: SHM publish failed on '" << shm->name() << "' ("
          << link::format_publisher_health(shm->stats(), shm->status(),
                                           shm->last_errno(), 0, 0, 0)
          << ")\n";
      return core::kExitRuntime;
    }
    out << "shm: " << shm->name() << " (" << shm->stats().published << " frames";
    if (emit_health) {
      out << ", " << link::format_publisher_health(shm->stats(), shm->status(),
                                                   shm->last_errno(), 0, 0, 0);
    }
    if (shm_sampling.select_given) {
      out << ", select=" << cli::format_emit_select(shm_sampling.serials);
      if (shm_subset) {
        out << " matched=" << shm_subset->matched() << " dropped=" << shm_subset->dropped()
            << " forwarded_rows=" << shm_subset->forwarded_rows();
      }
    }
    if (shm_stride) {
      out << ", stride=" << shm_stride->stride() << " matched=" << shm_stride->matched()
          << " dropped=" << shm_stride->dropped();
    }
    out << ")\n";
  }
  if (tcp) {
    const std::uint64_t pending = tcp->has_pending_frame() ? 1 : 0;
    if (!tcp_finish.ok() || tcp->has_pending_frame() || !tcp->ok()) {
      out << "Error: TCP publish incomplete on '" << tcp->endpoint() << "' ("
          << link::format_publisher_health(tcp->stats(), tcp->status(),
                                           tcp->last_errno(), pending,
                                           tcp->pending_bytes(), pending)
          << ")\n";
      return core::kExitRuntime;
    }
    out << "tcp: " << tcp->endpoint() << " (" << tcp->stats().published << " frames, "
        << tcp->stats().dropped_over_bound + tcp->stats().dropped_send +
               tcp->stats().dropped_pending_blocked + tcp->stats().dropped_queue_full
        << " dropped";
    if (emit_health) {
      out << ", " << link::format_publisher_health(tcp->stats(), tcp->status(),
                                                   tcp->last_errno(), pending,
                                                   tcp->pending_bytes(), 0);
    }
    if (tcp_sampling.select_given) {
      out << ", select=" << cli::format_emit_select(tcp_sampling.serials);
      if (tcp_subset) {
        out << " matched=" << tcp_subset->matched() << " dropped=" << tcp_subset->dropped()
            << " forwarded_rows=" << tcp_subset->forwarded_rows();
      }
    }
    if (tcp_stride) {
      out << ", stride=" << tcp_stride->stride() << " matched=" << tcp_stride->matched()
          << " dropped=" << tcp_stride->dropped();
    }
    out << ")\n";
  }
  if (udp) {
    if (!udp->ok()) {
      out << "Error: UDP publish failed on '" << udp->endpoint() << "' ("
          << link::format_publisher_health(udp->stats(), udp->status(),
                                           udp->last_errno(), 0, 0, 0)
          << " budget=" << udp->payload_budget()
          << " need=" << udp->last_over_bound_bytes() << ")\n";
      return core::kExitRuntime;
    }
    out << "udp: " << udp->endpoint() << " (" << udp->stats().published << " frames, "
        << udp->stats().dropped_over_bound + udp->stats().dropped_send +
               udp->stats().dropped_pending_blocked + udp->stats().dropped_queue_full
        << " dropped, budget=" << udp->payload_budget();
    if (emit_health) {
      out << ", " << link::format_publisher_health(udp->stats(), udp->status(),
                                                   udp->last_errno(), 0, 0, 0);
    }
    if (udp_sampling.select_given) {
      out << ", select=" << cli::format_emit_select(udp_sampling.serials);
      if (udp_subset) {
        out << " matched=" << udp_subset->matched() << " dropped=" << udp_subset->dropped()
            << " forwarded_rows=" << udp_subset->forwarded_rows();
      }
    }
    if (udp_stride) {
      out << ", stride=" << udp_stride->stride() << " matched=" << udp_stride->matched()
          << " dropped=" << udp_stride->dropped();
    }
    out << ")\n";
  }
  if (uart) {
    if (!uart->ok()) {
      out << "Error: UART publish failed on '" << uart->device() << "' ("
          << link::format_publisher_health(uart->stats(), uart->status(),
                                           uart->last_errno(), 0, 0, 0)
          << " mode=" << (uart->mode() == link::uart::UartMode::kCobs ? "cobs" : "ascii")
          << " baud=" << uart->baud() << ")\n";
      return core::kExitRuntime;
    }
    out << "uart: " << uart->device() << " (" << uart->stats().published << " frames, "
        << uart->lines_written() << " lines, " << uart->lines_dropped() << " lines dropped"
        << ", mode=" << (uart->mode() == link::uart::UartMode::kCobs ? "cobs" : "ascii")
        << " baud=" << uart->baud();
    if (emit_health) {
      out << ", " << link::format_publisher_health(uart->stats(), uart->status(),
                                                   uart->last_errno(), 0, 0, 0);
    }
    if (uart_sampling.select_given) {
      out << ", select=" << cli::format_emit_select(uart_sampling.serials);
      if (uart_subset) {
        out << " matched=" << uart_subset->matched() << " dropped=" << uart_subset->dropped()
            << " forwarded_rows=" << uart_subset->forwarded_rows();
      }
    }
    if (uart_stride) {
      out << ", stride=" << uart_stride->stride() << " matched=" << uart_stride->matched()
          << " dropped=" << uart_stride->dropped();
    }
    out << ")\n";
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
  // Values for a preceding "--emit-uart-mode".
  if (!prior_args.empty() && prior_args.back() == "--emit-uart-mode") {
    for (std::string_view m : {"ascii", "cobs"}) {
      if (m.substr(0, partial.size()) == partial) {
        out.emplace_back(m);
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
