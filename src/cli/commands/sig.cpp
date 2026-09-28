#include "wengine/cli/commands.hpp"
#include "wengine/cli/emit_options.hpp"
#include "wengine/cli/status.hpp"
#include "wengine/core/run_config.hpp"
#include "wengine/emit/routing.hpp"
#include "wengine/emit/signal_sink.hpp"
#include "wengine/link/control_server.hpp"
#include "wengine/link/frame.hpp"
#include "wengine/link/output_report.hpp"
#include "wengine/link/protocol.hpp"
#include "wengine/link/shm_publisher.hpp"
#include "wengine/link/socket_publisher.hpp"
#include "wengine/signal/lua.hpp"
#include "wengine/signal/table.hpp"
#include "wengine/signal/types.hpp"
#include "wengine/solvers/configured_run.hpp"
#include "wengine/solvers/paced_run.hpp"
#include "wengine/solvers/run.hpp"
#include "wengine/solvers/signal_control.hpp"
#include "wengine/solvers/solver_registry.hpp"

#include <array>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <sstream>
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

volatile std::sig_atomic_t g_sigint_requested = 0;

void request_sigint(int) noexcept { g_sigint_requested = 1; }

bool sigint_requested() noexcept { return g_sigint_requested != 0; }

constexpr std::array<std::string_view, 36> kSigOptions{{
    "--steps", "-n", "--dt", "--sig-kind", "--sig-freq", "--sig-amp", "--sig-offset",
    "--sig-phase", "--sig-duty", "--sig-seed", "--sig-quantity", "--sig-count",
    "--sig-expr", "--sig-table", "--sig-lua", "--emit-csv", "--emit-shm", "--emit-tcp",
    "--emit-udp", "--emit-every", "--emit-csv-stride", "--emit-shm-stride",
    "--emit-tcp-stride", "--emit-udp-stride", "--emit-udp-budget", "--emit-csv-select",
    "--emit-shm-select", "--emit-tcp-select", "--emit-udp-select", "--emit-health",
    "--control-tcp", "--forever", "--paced", "--rate", "--help", "-h",
}};

// Chunked live-output health (PHYSI-39): shared with sim run. Fast batches
// step in bounded chunks so mid-run sticky failures surface on stderr
// while the sim continues; --forever prints the same 1-chunk periodic
// line until Ctrl-C. Chunked run(N) is bit-identical to one run(total).
inline constexpr std::uint64_t kHealthChunkSteps = 4096;

void print_live_health(std::uint64_t done_steps,
                       const std::optional<link::ShmPublisher> &shm,
                       const std::optional<link::TcpPublisher> &tcp,
                       const std::optional<link::UdpPublisher> &udp, double elapsed_s,
                       double sample_rate_hz) {
  std::cerr << "live: steps=" << done_steps << " sample_rate=" << sample_rate_hz
            << "Hz elapsed=" << elapsed_s << "s";
  if (elapsed_s > 0.0) {
    if (shm) {
      std::cerr << " shm_bytes_s="
                << static_cast<double>(shm->stats().bytes_accepted) / elapsed_s;
    }
    if (tcp) {
      std::cerr << " tcp_bytes_s="
                << static_cast<double>(tcp->stats().bytes_accepted) / elapsed_s;
    }
    if (udp) {
      std::cerr << " udp_bytes_s="
                << static_cast<double>(udp->stats().bytes_accepted) / elapsed_s;
    }
  }
  std::cerr << '\n';
  if (shm) {
    std::cerr << "live shm: "
              << link::format_publisher_health(shm->stats(), shm->status(),
                                              shm->last_errno(), 0, 0, 0)
              << '\n';
  }
  if (tcp) {
    const std::uint64_t pending = tcp->has_pending_frame() ? 1 : 0;
    std::cerr << "live tcp: "
              << link::format_publisher_health(tcp->stats(), tcp->status(),
                                              tcp->last_errno(), pending,
                                              tcp->pending_bytes(), pending)
              << '\n';
  }
  if (udp) {
    std::cerr << "live udp: "
              << link::format_publisher_health(udp->stats(), udp->status(),
                                              udp->last_errno(), 0, 0, 0)
              << '\n';
  }
}

} // namespace

int SigSpec::run(ArgCursor &args, std::ostream &out) {
  if (args.take_flag({"--help", "-h"})) {
    out << kName << " - " << kDescription << "\nUsage:\n" << kUsage << '\n';
    return core::kExitOk;
  }

  long long steps = 100;
  double dt = 0.01;
  long long emit_every = 1;
  const bool forever = args.take_flag({"--forever"});
  const bool paced = args.take_flag({"--paced"});
  const bool emit_health = args.take_flag({"--emit-health"});
  const auto o_rate = args.take_option({"--rate"});
  std::optional<std::string_view> csv_path;
  std::optional<std::string_view> shm_name;
  std::optional<std::string_view> tcp_endpoint;
  std::optional<std::string_view> udp_endpoint;
  std::optional<std::string_view> control_endpoint;
  long long udp_budget = static_cast<long long>(link::kDefaultUdpPayloadBudget);
  bool udp_budget_given = false;

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
  tcp_endpoint = args.take_option({"--emit-tcp"});
  udp_endpoint = args.take_option({"--emit-udp"});
  control_endpoint = args.take_option({"--control-tcp"});
  cli::EmitOutputSampling csv_sampling;
  cli::EmitOutputSampling shm_sampling;
  cli::EmitOutputSampling tcp_sampling;
  cli::EmitOutputSampling udp_sampling;
  auto take_stride = [&](std::initializer_list<std::string_view> names, const char *what,
                         cli::EmitOutputSampling &slot) -> bool {
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
  auto take_select = [&](std::initializer_list<std::string_view> names, const char *what,
                         cli::EmitOutputSampling &slot) -> bool {
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
      !take_stride({"--emit-udp-stride"}, "udp stride", udp_sampling)) {
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
      !take_select({"--emit-udp-select"}, "udp selection", udp_sampling)) {
    return core::kExitUsage;
  }

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
  const auto o_sig_lua = args.take_option({"--sig-lua"});

  signal::SignalParams params;
  if (o_sig_kind) {
    const auto kind = signal::parse_kind(*o_sig_kind);
    if (!kind.ok()) {
      return usage_error(out, "unknown signal kind (want sine|square|triangle|saw|dc|"
                              "pulse|noise|sum|expression|table|lua)");
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
  std::string lua_script;
  const bool is_expr = params.kind == signal::SignalKind::kExpression;
  const bool is_table = params.kind == signal::SignalKind::kTable;
  const bool is_lua = params.kind == signal::SignalKind::kLua;
  if (is_expr) {
    if (!o_sig_expr) {
      return usage_error(out, "expression kind needs --sig-expr '<math>'");
    }
    if (o_sig_table) {
      return usage_error(out, "--sig-table is table-only (no expression source to replay)");
    }
    if (o_sig_lua) {
      return usage_error(out, "--sig-lua is lua-only (no script in an expression source)");
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
    if (o_sig_lua) {
      return usage_error(out, "--sig-lua is lua-only (no script in a table source)");
    }
  } else if (is_lua) {
    if (!o_sig_lua) {
      return usage_error(out, "lua kind needs --sig-lua <file.lua>");
    }
    if (o_sig_expr) {
      return usage_error(out, "--sig-expr is expression-only (no program in a lua source)");
    }
    if (o_sig_table) {
      return usage_error(out, "--sig-table is table-only (no CSV in a lua source)");
    }
  } else {
    if (o_sig_expr) {
      return usage_error(out, "--sig-expr needs --sig-kind expression");
    }
    if (o_sig_table) {
      return usage_error(out, "--sig-table needs --sig-kind table");
    }
    if (o_sig_lua) {
      return usage_error(out, "--sig-lua needs --sig-kind lua");
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

  if (!forever && steps < 1) {
    return usage_error(out, "steps must be >= 1");
  }
  if (!(dt > 0.0)) {
    return usage_error(out, "dt must be > 0");
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
  if (is_lua) {
    // Script file I/O stays at the edge: the engine only ever sees text.
    std::ifstream lua_in(std::string{*o_sig_lua});
    if (!lua_in) {
      out << "Error: cannot open Lua file '" << *o_sig_lua << "'\n";
      return core::kExitRuntime;
    }
    std::ostringstream lua_buf;
    lua_buf << lua_in.rdbuf();
    if (!lua_in && !lua_in.eof()) {
      out << "Error: cannot read Lua file '" << *o_sig_lua << "'\n";
      return core::kExitRuntime;
    }
    lua_script = lua_buf.str();
    if (!lua::check_script(std::string_view(lua_script.data(), lua_script.size())).ok()) {
      out << "Error: invalid Lua script '" << *o_sig_lua << "' (syntax check failed)\n";
      out << kUsage << '\n';
      return core::kExitUsage;
    }
  }
  if (sig_seed < 0 || sig_count < 0 || !signal::validate_params(params).ok()) {
    return usage_error(out, "invalid signal params (freq > 0, duty in 0..1, seed/count >= 0)");
  }
  // Per-output sampling validation (PHYSI-34): stride/select without its
  // output is a usage error; selections address stable batch serials
  // 0..count-1 (fresh State spawns count identical sources).
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
  {
    const cli::EmitOutputSampling *outs[4] = {&csv_sampling, &shm_sampling, &tcp_sampling,
                                              &udp_sampling};
    const char *names[4] = {"csv", "shm", "tcp", "udp"};
    const bool requested[4] = {csv_path.has_value(), shm_name.has_value(),
                               tcp_endpoint.has_value(), udp_endpoint.has_value()};
    const auto n_live = static_cast<std::size_t>(sig_count < 0 ? 0 : sig_count);
    for (int k = 0; k < 4; ++k) {
      if (!outs[k]->select_given) {
        continue;
      }
      for (std::uint64_t id : outs[k]->serials) {
        if (id >= static_cast<std::uint64_t>(n_live)) {
          out << "Error: --emit-" << names[k] << "-select serial " << id
              << " out of range (count=" << n_live << ", serials 0.." << (n_live == 0 ? 0 : n_live - 1)
              << ")\n";
          out << kUsage << '\n';
          return core::kExitUsage;
        }
      }
      if (requested[k] && k > 0 && !cli::signal_channels_fit(outs[k]->serials.size())) {
        out << "Error: --emit-" << names[k] << "-select needs "
            << (outs[k]->serials.size() + 1) << " channels, transport holds "
            << link::kMaxChannels << '\n';
        out << kUsage << '\n';
        return core::kExitUsage;
      }
    }
    // Unfiltered transports must fit the whole batch (1 + count <= 64).
    for (int k = 1; k < 4; ++k) {
      if (!requested[k] || outs[k]->select_given) {
        continue;
      }
      if (!cli::signal_channels_fit(n_live)) {
        out << "Error: --emit-" << names[k] << " needs " << (n_live + 1)
            << " channels, transport holds " << link::kMaxChannels
            << " (subscribe with --emit-" << names[k] << "-select)\n";
        out << kUsage << '\n';
        return core::kExitUsage;
      }
    }
  }

  core::RunConfig cfg;
  cfg.loop.dt = dt;
  cfg.loop.emit_every = static_cast<std::uint64_t>(emit_every);
  cfg.steps = forever ? UINT64_MAX : static_cast<std::uint64_t>(steps);
  cfg.solver = solvers::SignalSolver::kName;
  cfg.particles = 0;
  cfg.signal.params = params;
  cfg.signal.count = static_cast<std::uint64_t>(sig_count);
  cfg.signal.set = true;
  cfg.signal.table = std::move(table_points);
  cfg.signal.lua = std::move(lua_script);

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

  // Socket publishers ride the same emit path (best-effort ISinks owned
  // here). Endpoint shape errors are usage errors; connect/DNS failures are
  // runtime errors — same split as SHM names.
  std::optional<link::TcpPublisher> tcp;
  if (tcp_endpoint) {
    if (!link::parse_endpoint(*tcp_endpoint).ok()) {
      return usage_error(out, "invalid TCP endpoint (want host:port)");
    }
    tcp.emplace(*tcp_endpoint, cfg.solver, &link::frame_from_signal_snapshot);
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
    udp.emplace(*udp_endpoint, cfg.solver, &link::frame_from_signal_snapshot,
                static_cast<std::size_t>(udp_budget));
    if (!udp->ok()) {
      out << "Error: cannot open UDP '" << *udp_endpoint << "'\n";
      return core::kExitRuntime;
    }
  }

  // Per-output sampling wrappers (PHYSI-34, edge-side only — Runner keeps
  // its single global emit_every gate; each output further selects via
  // StrideSink step%stride==0 and/or SignalSubsetSink serial allowlists).
  // Chain per output: publisher <- Subset (if select) <- Stride (if != 1).
  std::optional<emit::SignalSubsetSink> csv_subset;
  std::optional<emit::StrideSink> csv_stride;
  std::optional<emit::SignalSubsetSink> shm_subset;
  std::optional<emit::StrideSink> shm_stride;
  std::optional<emit::SignalSubsetSink> tcp_subset;
  std::optional<emit::StrideSink> tcp_stride;
  std::optional<emit::SignalSubsetSink> udp_subset;
  std::optional<emit::StrideSink> udp_stride;
  emit::ISink *csv_out = nullptr;
  emit::ISink *shm_out = nullptr;
  emit::ISink *tcp_out = nullptr;
  emit::ISink *udp_out = nullptr;
  if (sig_csv) {
    csv_out = &*sig_csv;
    if (csv_sampling.select_given) {
      csv_subset.emplace(*csv_out);
      csv_subset->set_subscription(csv_sampling.serials);
      csv_out = &*csv_subset;
    }
    if (csv_sampling.stride_given && csv_sampling.stride != 1) {
      csv_stride.emplace(*csv_out, csv_sampling.stride);
      csv_out = &*csv_stride;
    }
  }
  if (shm) {
    shm_out = &*shm;
    if (shm_sampling.select_given) {
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
    if (tcp_sampling.select_given) {
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
    if (udp_sampling.select_given) {
      udp_subset.emplace(*udp_out);
      udp_subset->set_subscription(udp_sampling.serials);
      udp_out = &*udp_subset;
    }
    if (udp_sampling.stride_given && udp_sampling.stride != 1) {
      udp_stride.emplace(*udp_out, udp_sampling.stride);
      udp_out = &*udp_stride;
    }
  }

  // Lua sources evaluate in an extra solver that steps before SignalSolver
  // (custom-source contract — see solvers/run.hpp).
  std::unique_ptr<lua::LuaSignalSolver> lua_solver;
  std::array<core::ISolver *, 1> extra_arr{};
  std::size_t n_extra = 0;
  if (is_lua) {
    lua_solver = lua::create_lua_solver();
    extra_arr[n_extra++] = lua_solver.get();
  }
  // Persistent configured run (PHYSI-35): one build, then either a fast
  // batch or paced chunks against absolute deadlines (PHYSI-36). Sinks stay
  // caller-owned and are attached once; the engine never sleeps.
  solvers::ConfiguredRun run;
  {
    const std::span<core::ISolver *> extra(extra_arr.data(), n_extra);
    const bool has_lua = solvers::contains_lua_solver(extra);
    const core::Status built =
        solvers::ConfiguredRun::build(cfg, extra, has_lua, run, nullptr);
    if (!built.ok()) {
      out << "Error: simulation step failed\n";
      if (lua_solver && !lua_solver->last_error().empty()) {
        out << "lua: " << lua_solver->last_error() << '\n';
      }
      return core::kExitRuntime;
    }
  }
  world::State s;
  std::array<emit::ISink *, 4> sink_arr{};
  std::size_t n_sinks = 0;
  if (csv_out) {
    sink_arr[n_sinks++] = csv_out;
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
  for (std::size_t k = 0; k < n_sinks; ++k) {
    run.add_sink(*sink_arr[k]);
  }
  // SIG-10 control plane (step-synchronous): when --control-tcp is set,
  // every step is driven singly with a non-blocking poll + bus drain
  // before it, so commands apply at exact steps and log for replay.
  // Without the flag the original chunked paths run unchanged.
  std::optional<link::ControlServer> ctl;
  if (control_endpoint) {
    if (!link::parse_endpoint(*control_endpoint).ok()) {
      return usage_error(out, "invalid control endpoint (want host:port)");
    }
    ctl.emplace(*control_endpoint);
    if (!ctl->ok()) {
      out << "Error: cannot listen control '" << *control_endpoint << "'\n";
      return core::kExitRuntime;
    }
  }
  solvers::ControlBus ctl_bus(ctl ? ctl->run_id() : 0);
  ctl_bus.add_channel(); // channel 0: local CLI (reserved, first in drain order)
  if (ctl) {
    for (std::uint32_t i = 0; i < ctl->max_clients(); ++i) {
      ctl_bus.add_channel(); // channel 1+i: TCP slot i (fixed order)
    }
  }
  std::vector<link::ControlSlotLines> ctl_polled;
  auto ctl_service = [&] {
    if (!ctl) {
      return;
    }
    ctl->poll(ctl_polled);
    for (auto &sl : ctl_polled) {
      for (auto &ln : sl.lines) {
        ctl_bus.enqueue(1 + sl.slot, std::move(ln));
      }
    }
    ctl_bus.drain(run.state(), run.state().step_index);
    for (std::uint32_t c = 1; c < ctl_bus.channel_count(); ++c) {
      for (auto &r : ctl_bus.take_responses(c)) {
        ctl->push_response(c - 1, r);
      }
    }
    ctl->flush();
  };
  // Paced waits only buffer arrivals (poll) and flush answers; the drain
  // stays in before_chunk so commands still apply at exact step starts.
  auto ctl_on_idle = [&] {
    if (!ctl) {
      return;
    }
    ctl->poll(ctl_polled);
    for (auto &sl : ctl_polled) {
      for (auto &ln : sl.lines) {
        ctl_bus.enqueue(1 + sl.slot, std::move(ln));
      }
    }
    ctl->flush();
  };
  using SignalHandler = void (*)(int);
  SignalHandler previous_sigint = nullptr;
  const bool handles_sigint = forever || paced;
  if (handles_sigint) {
    g_sigint_requested = 0;
    previous_sigint = std::signal(SIGINT, request_sigint);
    if (previous_sigint == SIG_ERR) {
      out << "Error: cannot install Ctrl-C handler\n";
      return core::kExitRuntime;
    }
  }
  core::Status st = core::Status::success();
  solvers::paced::Stats paced_stats;
  const std::uint64_t total_steps = cfg.steps;
  const bool has_live = shm || tcp || udp;
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
  if (paced) {
    const auto epoch = solvers::paced::SteadyClock::now();
    auto last_report = std::chrono::steady_clock::now();
    auto idle = [&] {
      ctl_on_idle();
      if (tcp) {
        (void)tcp->service_pending();
      }
      if (emit_health && has_live) {
        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now - last_report).count() >= 1.0) {
          last_report = now;
          print_live_health(run.state().step_index, shm, tcp, udp,
                            wall_elapsed_s(), sample_rate_hz);
        }
      }
    };
    auto before_chunk = [&](std::uint64_t) { ctl_service(); };
    st = solvers::paced::run_paced<solvers::paced::SteadyClock>(
        run, total_steps, rate_hz, 1, epoch, sigint_requested, idle, paced_stats,
        before_chunk);
  } else if (ctl) {
    // Step-synchronous control: poll + drain before every single step
    // (finite and --forever alike); health stays chunked as usual.
    std::uint64_t done = 0;
    while (done < total_steps) {
      if (forever && sigint_requested()) {
        break;
      }
      ctl_service();
      st = run.run(1);
      if (!st.ok()) {
        break;
      }
      ++done;
      if (emit_health && has_live && done % kHealthChunkSteps == 0) {
        print_live_health(done, shm, tcp, udp, wall_elapsed_s(),
                          sample_rate_hz);
      }
    }
    if (st.ok()) {
      st = core::Status::success();
    }
  } else if (forever) {
    // --forever: bounded chunks until Ctrl-C; each chunk boundary is a
    // periodic health point with --emit-health (step-boundary stop
    // granularity stays one chunk, like paced). Telemetry failures never
    // stop the sim here — they surface per chunk and in the final summary.
    std::uint64_t done = 0;
    while (!sigint_requested()) {
      st = run.run(kHealthChunkSteps, &sigint_requested);
      if (!st.ok()) {
        break;
      }
      done += kHealthChunkSteps;
      if (emit_health && has_live) {
        print_live_health(done, shm, tcp, udp, wall_elapsed_s(),
                          sample_rate_hz);
      }
    }
    if (st.ok()) {
      st = core::Status::success();
    }
  } else {
    std::uint64_t done = 0;
    while (done < total_steps) {
      std::uint64_t n = kHealthChunkSteps;
      if (n > total_steps - done) {
        n = total_steps - done;
      }
      st = run.run(n);
      if (!st.ok()) {
        break;
      }
      done += n;
      if (emit_health && has_live && done < total_steps) {
        print_live_health(done, shm, tcp, udp, wall_elapsed_s(),
                          sample_rate_hz);
      }
    }
  }
  if (handles_sigint) {
    (void)std::signal(SIGINT, previous_sigint);
  }
  s = run.state();
  // Explicit bounded stream finish (PHYSI-32): normal completion and the
  // Ctrl-C step-boundary stop above both drain here. The destructor never
  // waits, so an undrained tail would otherwise be abandoned silently.
  core::Status tcp_finish = core::Status::success();
  if (tcp) {
    tcp_finish = tcp->finish(link::kTcpFinishTimeoutMs);
  }
  if (!st.ok()) {
    out << "Error: simulation step failed\n";
    if (lua_solver && !lua_solver->last_error().empty()) {
      out << "lua: " << lua_solver->last_error() << '\n';
    }
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
  if (is_lua) {
    out << "lua: " << *o_sig_lua << " (" << cfg.signal.lua.size() << " bytes)\n";
  }
  if (forever || (paced && paced_stats.steps_done < total_steps)) {
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
    out << "csv: " << sig_csv->path() << " (" << sig_csv->rows_written() << " rows";
    if (csv_sampling.select_given) {
      out << ", select=" << cli::format_emit_select(csv_sampling.serials);
      if (csv_subset) {
        out << " matched=" << csv_subset->matched() << " dropped=" << csv_subset->dropped()
            << " forwarded_rows=" << csv_subset->forwarded_rows();
      }
    }
    if (csv_stride) {
      out << ", stride=" << csv_stride->stride() << " matched=" << csv_stride->matched()
          << " dropped=" << csv_stride->dropped();
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
  if (ctl) {
    ctl->flush();
    out << "control: " << ctl->endpoint() << " (applied=" << ctl_bus.applied()
        << " readonly=" << ctl_bus.readonly() << " rejected=" << ctl_bus.rejected()
        << " clients=" << ctl->stats().accepted << ")\n";
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
