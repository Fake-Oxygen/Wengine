#include "wengine/cli/cancel.hpp"
#include "wengine/cli/commands.hpp"
#include "wengine/cli/emit_options.hpp"
#include "wengine/cli/plot.hpp"
#include "wengine/cli/status.hpp"
#include "wengine/core/run_config.hpp"
#include "wengine/emit/routing.hpp"
#include "wengine/emit/signal_sink.hpp"
#include "wengine/link/control_client.hpp"
#include "wengine/link/control_server.hpp"
#include "wengine/link/frame.hpp"
#include "wengine/link/output_report.hpp"
#include "wengine/link/protocol.hpp"
#include "wengine/link/shm_publisher.hpp"
#include "wengine/link/socket_publisher.hpp"
#include "wengine/link/uart_publisher.hpp"
#include "wengine/ros2/cli_options.hpp"
#include "wengine/ros2/control.hpp"
#include "wengine/ros2/publisher.hpp"
#ifdef WENGINE_HAS_RCLCPP
#include "rclcpp/rclcpp.hpp"
#include "wengine/ros2/rclcpp_transport.hpp"
#include "wengine/ros2/ros_control.hpp"
#endif
#include "wengine/signal/lua.hpp"
#include "wengine/signal/table.hpp"
#include "wengine/signal/types.hpp"
#include "wengine/solvers/configured_run.hpp"
#include "wengine/solvers/paced_run.hpp"
#include "wengine/solvers/run.hpp"
#include "wengine/solvers/signal_control.hpp"
#include "wengine/solvers/solver_registry.hpp"

#include <array>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
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
  return s == "run" || s == "list" || s == "add" || s == "set" || s == "start" ||
         s == "stop" || s == "drop";
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

constexpr std::array<std::string_view, 7> kSigModes{
    {"run", "list", "add", "set", "start", "stop", "drop"}};

volatile std::sig_atomic_t g_sigint_requested = 0;

void request_sigint(int) noexcept { g_sigint_requested = 1; }

bool sigint_requested() noexcept { return g_sigint_requested != 0; }

// TUI-5 async stop: the worker sets the global cancel atom while the UI
// stays live; headless runs never set it. Checked at step boundaries.
bool combined_stop() noexcept {
  return sigint_requested() || cli::cancel_requested();
}

constexpr std::array<std::string_view, 54> kSigOptions{{
    "--steps", "-n", "--dt", "--sig-kind", "--sig-freq", "--sig-amp", "--sig-offset",
    "--sig-phase", "--sig-duty", "--sig-seed", "--sig-quantity", "--sig-count",
    "--sig-expr", "--sig-table", "--sig-lua", "--sig-operands", "--emit-csv", "--emit-shm", "--emit-tcp",
    "--emit-udp", "--emit-every", "--emit-csv-stride", "--emit-shm-stride",
    "--emit-tcp-stride", "--emit-udp-stride", "--emit-udp-budget", "--emit-csv-select",
    "--emit-shm-select", "--emit-tcp-select", "--emit-udp-select", "--emit-health",
    "--emit-uart", "--emit-uart-mode", "--emit-uart-stride", "--emit-uart-select",
    "--emit-ros", "--emit-ros-stride", "--emit-ros-select",
    "--ros-node", "--ros-ns", "--ros-signal-stem", "--ros-particle-stem",
    "--ros-qos", "--ros-grouped", "--ros-per-serial",
    "--control-tcp", "--control-uart", "--control-ros",
    "--forever", "--paced", "--rate", "--plot", "--help", "-h",
}};

// Chunked live-output health (PHYSI-39): shared with sim run. Fast batches
// step in bounded chunks so mid-run sticky failures surface on stderr
// while the sim continues; --forever prints the same 1-chunk periodic
// line until Ctrl-C. Chunked run(N) is bit-identical to one run(total).
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
    if (uart != nullptr) {
      std::cerr << " uart_bytes_s="
                << static_cast<double>(uart->stats().bytes_accepted) / elapsed_s;
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

// Options taken once before mode dispatch (see SigSpec::run): every
// value-taking --opt is consumed here so bare values (dc, 5V, 1kHz, 100,
// file paths) are never mistaken for positional modes/serials. Flags take
// no values and stay with their mode. Views borrow the ArgCursor tokens.
// Each mode rejects the fields it does not own (stray options are usage
// errors, never silently ignored).
struct SigCliOptions {
  // Shared signal vocabulary (batch run + per-source family).
  std::optional<std::string_view> sig_kind;
  std::optional<std::string_view> sig_freq;
  std::optional<std::string_view> sig_amp;
  std::optional<std::string_view> sig_offset;
  std::optional<std::string_view> sig_phase;
  std::optional<std::string_view> sig_duty;
  std::optional<std::string_view> sig_seed;
  std::optional<std::string_view> sig_quantity;
  std::optional<std::string_view> sig_count;
  std::optional<std::string_view> sig_expr;
  std::optional<std::string_view> sig_table;
  std::optional<std::string_view> sig_lua;
  std::optional<std::string_view> sig_operands;
  // Control endpoint (run: server bind address; list/add/...: connect target).
  std::optional<std::string_view> control_tcp;
  // Batch/transport values (interpreted by the run mode only).
  std::optional<std::string_view> steps;
  std::optional<std::string_view> dt;
  std::optional<std::string_view> emit_every;
  std::optional<std::string_view> rate;
  std::optional<std::string_view> emit_csv;
  std::optional<std::string_view> emit_shm;
  std::optional<std::string_view> emit_tcp;
  std::optional<std::string_view> emit_udp;
  std::optional<std::string_view> emit_uart;
  std::optional<std::string_view> emit_uart_mode;
  std::optional<std::string_view> emit_udp_budget;
  std::optional<std::string_view> emit_csv_stride;
  std::optional<std::string_view> emit_shm_stride;
  std::optional<std::string_view> emit_tcp_stride;
  std::optional<std::string_view> emit_udp_stride;
  std::optional<std::string_view> emit_uart_stride;
  std::optional<std::string_view> emit_csv_select;
  std::optional<std::string_view> emit_shm_select;
  std::optional<std::string_view> emit_tcp_select;
  std::optional<std::string_view> emit_udp_select;
  std::optional<std::string_view> emit_uart_select;
  std::optional<std::string_view> emit_ros_stride;
  std::optional<std::string_view> emit_ros_select;
  std::optional<std::string_view> ros_node;
  std::optional<std::string_view> ros_ns;
  std::optional<std::string_view> ros_signal_stem;
  std::optional<std::string_view> ros_particle_stem;
  std::optional<std::string_view> ros_qos;
};

void take_sig_options(ArgCursor &args, SigCliOptions &o) {
  o.sig_kind = args.take_option({"--sig-kind"});
  o.sig_freq = args.take_option({"--sig-freq"});
  o.sig_amp = args.take_option({"--sig-amp"});
  o.sig_offset = args.take_option({"--sig-offset"});
  o.sig_phase = args.take_option({"--sig-phase"});
  o.sig_duty = args.take_option({"--sig-duty"});
  o.sig_seed = args.take_option({"--sig-seed"});
  o.sig_quantity = args.take_option({"--sig-quantity"});
  o.sig_count = args.take_option({"--sig-count"});
  o.sig_expr = args.take_option({"--sig-expr"});
  o.sig_table = args.take_option({"--sig-table"});
  o.sig_lua = args.take_option({"--sig-lua"});
  o.sig_operands = args.take_option({"--sig-operands"});
  o.control_tcp = args.take_option({"--control-tcp"});
  o.steps = args.take_option({"--steps", "-n"});
  o.dt = args.take_option({"--dt"});
  o.emit_every = args.take_option({"--emit-every"});
  o.rate = args.take_option({"--rate"});
  o.emit_csv = args.take_option({"--emit-csv"});
  o.emit_shm = args.take_option({"--emit-shm"});
  o.emit_tcp = args.take_option({"--emit-tcp"});
  o.emit_udp = args.take_option({"--emit-udp"});
  o.emit_uart = args.take_option({"--emit-uart"});
  o.emit_uart_mode = args.take_option({"--emit-uart-mode"});
  o.emit_udp_budget = args.take_option({"--emit-udp-budget"});
  o.emit_csv_stride = args.take_option({"--emit-csv-stride"});
  o.emit_shm_stride = args.take_option({"--emit-shm-stride"});
  o.emit_tcp_stride = args.take_option({"--emit-tcp-stride"});
  o.emit_udp_stride = args.take_option({"--emit-udp-stride"});
  o.emit_uart_stride = args.take_option({"--emit-uart-stride"});
  o.emit_csv_select = args.take_option({"--emit-csv-select"});
  o.emit_shm_select = args.take_option({"--emit-shm-select"});
  o.emit_tcp_select = args.take_option({"--emit-tcp-select"});
  o.emit_udp_select = args.take_option({"--emit-udp-select"});
  o.emit_uart_select = args.take_option({"--emit-uart-select"});
  o.emit_ros_stride = args.take_option({ros2::cli_options::kEmitRosStride});
  o.emit_ros_select = args.take_option({ros2::cli_options::kEmitRosSelect});
  o.ros_node = args.take_option({ros2::cli_options::kRosNode});
  o.ros_ns = args.take_option({ros2::cli_options::kRosNs});
  o.ros_signal_stem = args.take_option({ros2::cli_options::kRosSignalStem});
  o.ros_particle_stem = args.take_option({ros2::cli_options::kRosParticleStem});
  o.ros_qos = args.take_option({ros2::cli_options::kRosQos});
}

// True when any batch/transport value option was given: the per-source
// family (list/add/set/start/stop/drop) rejects them all.
[[nodiscard]] bool has_batch_options(const SigCliOptions &o) noexcept {
  return o.steps || o.dt || o.emit_every || o.rate || o.emit_csv || o.emit_shm ||
         o.emit_tcp || o.emit_udp || o.emit_uart || o.emit_uart_mode ||
         o.emit_udp_budget || o.emit_csv_stride || o.emit_shm_stride ||
         o.emit_tcp_stride || o.emit_udp_stride || o.emit_uart_stride ||
         o.emit_csv_select || o.emit_shm_select || o.emit_tcp_select ||
         o.emit_udp_select || o.emit_uart_select || o.emit_ros_stride ||
         o.emit_ros_select || o.ros_node || o.ros_ns || o.ros_signal_stem ||
         o.ros_particle_stem || o.ros_qos;
}

int run_batch(ArgCursor &args, std::ostream &out, std::span<const std::string_view> extra_pos, const SigCliOptions &o) {
  // (--help is served by the SigSpec::run dispatcher before dispatch.)
  // Sums configure through the per-source family (distinct per-source
  // operands cannot come from identical batch sources).
  if (o.sig_operands) {
    return usage_error(out, "--sig-operands needs sig add --sig-kind sum");
  }
  long long steps = 100;
  double dt = 0.01;
  long long emit_every = 1;
  const bool forever = args.take_flag({"--forever"});
  const bool paced = args.take_flag({"--paced"});
  const bool emit_health = args.take_flag({"--emit-health"});
  const bool emit_ros = args.take_flag({ros2::cli_options::kEmitRos});
  const bool ros_grouped_flag = args.take_flag({ros2::cli_options::kRosGrouped});
  const bool ros_per_serial_flag = args.take_flag({ros2::cli_options::kRosPerSerial});
  // Live plot window: in-process SDL2 2D view, independent of the GUI
  // backend and of the external Python receivers (see --emit-tcp). The
  // window observes emitted ticks only, like every other sink.
  const bool plot = args.take_flag({"--plot"});
  const auto o_rate = o.rate;
  std::optional<std::string_view> csv_path = o.emit_csv;
  std::optional<std::string_view> shm_name = o.emit_shm;
  std::optional<std::string_view> tcp_endpoint = o.emit_tcp;
  std::optional<std::string_view> udp_endpoint = o.emit_udp;
  std::optional<std::string_view> control_endpoint = o.control_tcp;
  std::optional<std::string_view> uart_device = o.emit_uart;
  std::optional<std::string_view> uart_mode_text = o.emit_uart_mode;
  const bool control_uart = args.take_flag({"--control-uart"});
  const bool control_ros = args.take_flag({"--control-ros"});
  long long udp_budget = static_cast<long long>(link::kDefaultUdpPayloadBudget);
  bool udp_budget_given = false;

  auto take_i64 = [&](std::optional<std::string_view> v, const char *what, long long &slot) -> bool {
    if (v) {
      const auto p = parse_i64(*v);
      if (!p.ok()) {
        out << "Error: invalid " << what << " '" << *v << "'\n";
        out << SigSpec::kUsage << '\n';
        return false;
      }
      slot = p.value;
    }
    return true;
  };

  if (!take_i64(o.steps, "step count", steps)) {
    return core::kExitUsage;
  }
  if (o.dt) {
    const auto p = parse_time(*o.dt);
    if (!p.ok()) {
      out << "Error: invalid time step '" << *o.dt << "'\n";
      out << SigSpec::kUsage << '\n';
      return core::kExitUsage;
    }
    dt = p.value;
  }
  if (!take_i64(o.emit_every, "emit stride", emit_every)) {
    return core::kExitUsage;
  }
  cli::EmitOutputSampling csv_sampling;
  cli::EmitOutputSampling shm_sampling;
  cli::EmitOutputSampling tcp_sampling;
  cli::EmitOutputSampling udp_sampling;
  cli::EmitOutputSampling uart_sampling;
  cli::EmitOutputSampling ros_sampling;
  auto take_stride = [&](std::optional<std::string_view> v, const char *what,
                         cli::EmitOutputSampling &slot) -> bool {
    if (v) {
      const auto p = cli::parse_emit_stride(*v);
      if (!p.ok()) {
        out << "Error: invalid " << what << " '" << *v << "' (want >= 0, 0 = muted)\n";
        out << SigSpec::kUsage << '\n';
        return false;
      }
      slot.stride = p.value;
      slot.stride_given = true;
    }
    return true;
  };
  auto take_select = [&](std::optional<std::string_view> v, const char *what,
                         cli::EmitOutputSampling &slot) -> bool {
    if (v) {
      const auto p = cli::parse_emit_select(*v);
      if (!p.ok()) {
        out << "Error: invalid " << what << " '" << *v << "' (want e.g. 0,2,5)\n";
        out << SigSpec::kUsage << '\n';
        return false;
      }
      slot.serials = p.value;
      slot.select_given = true;
    }
    return true;
  };
  if (!take_stride(o.emit_csv_stride, "csv stride", csv_sampling) ||
      !take_stride(o.emit_shm_stride, "shm stride", shm_sampling) ||
      !take_stride(o.emit_tcp_stride, "tcp stride", tcp_sampling) ||
      !take_stride(o.emit_udp_stride, "udp stride", udp_sampling) ||
      !take_stride(o.emit_uart_stride, "uart stride", uart_sampling) ||
      !take_stride(o.emit_ros_stride, "ros stride", ros_sampling)) {
    return core::kExitUsage;
  }
  if (o.emit_udp_budget) {
    const auto p = parse_i64(*o.emit_udp_budget);
    if (!p.ok() || p.value < 1 ||
        static_cast<std::uint64_t>(p.value) > link::kMaxUdpPayload) {
      out << "Error: invalid udp budget '" << *o.emit_udp_budget << "' (want 1.."
          << link::kMaxUdpPayload << ")\n";
      out << SigSpec::kUsage << '\n';
      return core::kExitUsage;
    }
    udp_budget = p.value;
    udp_budget_given = true;
  }
  if (!take_select(o.emit_csv_select, "csv selection", csv_sampling) ||
      !take_select(o.emit_shm_select, "shm selection", shm_sampling) ||
      !take_select(o.emit_tcp_select, "tcp selection", tcp_sampling) ||
      !take_select(o.emit_udp_select, "udp selection", udp_sampling) ||
      !take_select(o.emit_uart_select, "uart selection", uart_sampling) ||
      !take_select(o.emit_ros_select, "ros selection", ros_sampling)) {
    return core::kExitUsage;
  }

  // Signal vocabulary lives here (never in sim run): raw views first,
  // amp/offset parse against --sig-quantity. Values arrive pre-taken (see
  // take_sig_options) so bare words never read as positional modes.
  const auto o_sig_kind = o.sig_kind;
  const auto o_sig_freq = o.sig_freq;
  const auto o_sig_amp = o.sig_amp;
  const auto o_sig_offset = o.sig_offset;
  const auto o_sig_phase = o.sig_phase;
  const auto o_sig_duty = o.sig_duty;
  const auto o_sig_seed = o.sig_seed;
  const auto o_sig_quantity = o.sig_quantity;
  const auto o_sig_count = o.sig_count;
  const auto o_sig_expr = o.sig_expr;
  const auto o_sig_table = o.sig_table;
  const auto o_sig_lua = o.sig_lua;

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
  auto take_qty = [&](std::optional<std::string_view> v, const char *what,
                      double &slot) -> bool {
    if (v) {
      // Quantity-aware parse through the units-owned tables (no ad-hoc
      // voltage/current switch here — see signal::kQuantities).
      const auto p = parse_quantity_value(params.quantity, *v);
      if (!p.ok()) {
        out << "Error: invalid " << what << " '" << *v << "'\n";
        out << SigSpec::kUsage << '\n';
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
      out << SigSpec::kUsage << '\n';
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
      out << SigSpec::kUsage << '\n';
      return core::kExitUsage;
    }
    params.phase_rad = p.value;
  }
  if (o_sig_duty) {
    const auto p = parse_double(*o_sig_duty);
    if (!p.ok()) {
      out << "Error: invalid signal duty '" << *o_sig_duty << "'\n";
      out << SigSpec::kUsage << '\n';
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
      out << SigSpec::kUsage << '\n';
      return core::kExitUsage;
    }
    sig_seed = p.value;
  }
  if (o_sig_count) {
    const auto p = parse_i64(*o_sig_count);
    if (!p.ok()) {
      out << "Error: invalid signal count '" << *o_sig_count << "'\n";
      out << SigSpec::kUsage << '\n';
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
      out << SigSpec::kUsage << '\n';
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

  // The `run` mode takes no extra positionals; all params are named.
  if (!extra_pos.empty()) {
    return usage_error(out, "too many positional arguments");
  }
  if (args.has_remaining()) {
    return cli::unexpected_error(out, *args.first_remaining(), SigSpec::kUsage);
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
        out << SigSpec::kUsage << '\n';
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
      out << SigSpec::kUsage << '\n';
      return core::kExitUsage;
    }
  }
  if (sig_seed < 0 || sig_count < 0 || !signal::validate_params(params).ok()) {
    return usage_error(out, "invalid signal params (finite values, freq > 0, duty in 0..1, seed/count >= 0)");
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
  // ROS identity orphans (same never-silent rule as every other option):
  // publish layout/decimation need --emit-ros; the node/namespace serve
  // either plane (topics and/or services); topic stems and delivery class
  // are publish-only.
  if ((ros_sampling.stride_given || ros_sampling.select_given ||
       ros_grouped_flag || ros_per_serial_flag) && !emit_ros) {
    return usage_error(out, "--emit-ros-stride/select and --ros-grouped/per-serial need --emit-ros");
  }
  if ((o.ros_node || o.ros_ns) && !emit_ros && !control_ros) {
    return usage_error(out, "--ros-node/ns needs --emit-ros or --control-ros");
  }
  if ((o.ros_signal_stem || o.ros_particle_stem || o.ros_qos) && !emit_ros) {
    return usage_error(out, "--ros-signal-stem/particle-stem/qos needs --emit-ros");
  }
  if (ros_grouped_flag && ros_per_serial_flag) {
    return usage_error(out, "--ros-grouped and --ros-per-serial are mutually exclusive");
  }
  if (o.ros_node && !ros2::cli_options::is_valid_ros_node(*o.ros_node)) {
    return usage_error(out, "invalid ROS node name (want [A-Za-z_][A-Za-z0-9_]*)");
  }
  if (o.ros_ns && !ros2::cli_options::is_valid_ros_path(*o.ros_ns)) {
    return usage_error(out, "invalid ROS namespace (want a/b segments, no leading slash)");
  }
  if (o.ros_signal_stem && !ros2::cli_options::is_valid_ros_path(*o.ros_signal_stem)) {
    return usage_error(out, "invalid ROS signal stem (want a/b segments, no leading slash)");
  }
  if (o.ros_particle_stem && !ros2::cli_options::is_valid_ros_path(*o.ros_particle_stem)) {
    return usage_error(out, "invalid ROS particle stem (want a/b segments, no leading slash)");
  }
  // Parsed + validated here so usage errors (exit 2) are identical in
  // both builds; consumed only by the rmw transport below (ROS2=ON).
  [[maybe_unused]] ros2::QosClass ros_qos = ros2::QosClass::kLatestState;
  if (o.ros_qos) {
    const auto q = ros2::cli_options::parse_ros_qos(*o.ros_qos);
    if (!q.ok()) {
      return usage_error(out, "unknown ROS QoS (want latest|reliable)");
    }
    ros_qos = q.value;
  }
  // UART wire mode is explicit (never silent): ascii lines by default,
  // COBS frames on request. A mode without its device is a usage error.
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
  if (control_uart && !uart_device) {
    return usage_error(out, "--control-uart needs --emit-uart");
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
          out << SigSpec::kUsage << '\n';
          return core::kExitUsage;
        }
      }
      if (requested[k] && k > 0 && !cli::signal_channels_fit(outs[k]->serials.size())) {
        out << "Error: --emit-" << names[k] << "-select needs "
            << (outs[k]->serials.size() + 1) << " channels, transport holds "
            << link::kMaxChannels << '\n';
        out << SigSpec::kUsage << '\n';
        return core::kExitUsage;
      }
    }
    // Unfiltered transports must fit the whole batch (1 + count <= 64).
    // UART is exempt: lines are unbounded (no 64-channel frame cap), so
    // only the serial range below applies to it. ROS grouped records
    // honor the same 64-channel gate in the publisher.
    for (int k = 1; k < 4; ++k) {
      if (!requested[k] || outs[k]->select_given) {
        continue;
      }
      if (!cli::signal_channels_fit(n_live)) {
        out << "Error: --emit-" << names[k] << " needs " << (n_live + 1)
            << " channels, transport holds " << link::kMaxChannels
            << " (subscribe with --emit-" << names[k] << "-select)\n";
        out << SigSpec::kUsage << '\n';
        return core::kExitUsage;
      }
    }
    if (emit_ros && !ros_sampling.select_given && !cli::signal_channels_fit(n_live)) {
      out << "Error: --emit-ros needs " << (n_live + 1)
          << " channels, transport holds " << link::kMaxChannels
          << " (subscribe with --emit-ros-select)\n";
      out << SigSpec::kUsage << '\n';
      return core::kExitUsage;
    }
    if (ros_sampling.select_given) {
      for (std::uint64_t id : ros_sampling.serials) {
        if (id >= static_cast<std::uint64_t>(n_live)) {
          out << "Error: --emit-ros-select serial " << id
              << " out of range (count=" << n_live << ", serials 0.." << (n_live == 0 ? 0 : n_live - 1)
              << ")\n";
          out << SigSpec::kUsage << '\n';
          return core::kExitUsage;
        }
      }
      if (emit_ros && !cli::signal_channels_fit(ros_sampling.serials.size())) {
        out << "Error: --emit-ros-select needs " << (ros_sampling.serials.size() + 1)
            << " channels, transport holds " << link::kMaxChannels << '\n';
        out << SigSpec::kUsage << '\n';
        return core::kExitUsage;
      }
    }
  }
  if (uart_sampling.select_given) {
    const auto n_live = static_cast<std::uint64_t>(sig_count < 0 ? 0 : sig_count);
    for (std::uint64_t id : uart_sampling.serials) {
      if (id >= n_live) {
        out << "Error: --emit-uart-select serial " << id << " out of range (count=" << n_live
            << ", serials 0.." << (n_live == 0 ? 0 : n_live - 1) << ")\n";
        out << SigSpec::kUsage << '\n';
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

  // UART serial publisher (SIG-8 HIL track): device[,baud] shape errors
  // are usage errors; open/configure failures are runtime errors — the
  // same split as the socket endpoints above.
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

  // ROS state-out publisher (ROS2-3, edge-only ISink like every other
  // transport — never in RunConfig). Without a ROS2 build the flag is
  // parsed + validated above and fails loudly here instead of silently
  // dropping (same rebuild-hint split as --plot without SDL2).
  // BridgeConfig.stride stays 1: the StrideSink below owns decimation
  // (a muted stride-0 must still succeed with zero publishes, which a
  // zero BridgeConfig.stride would refuse at construction).
#ifdef WENGINE_HAS_RCLCPP
  std::shared_ptr<rclcpp::Node> ros_node;
  std::optional<ros2::RclcppTransport> ros_transport;
  std::optional<ros2::RosPublisher> ros_pub;
  // One node serves both planes (topics and/or services); the publisher
  // and the service set attach independently below.
  if (emit_ros || control_ros) {
    if (!ros2::cli_options::ensure_ros_initialized()) {
      out << "Error: cannot initialize ROS node\n";
      return core::kExitRuntime;
    }
    try {
      // Root namespace (like examples/ros2_bridge.cpp): the bridge forms
      // absolute topics/services itself (`/<ns>/...`, passed relative
      // here), so `ros2 topic/service list` shows them verbatim.
      ros_node = std::make_shared<rclcpp::Node>(
          std::string(o.ros_node ? *o.ros_node : "wengine"));
    } catch (...) {
      out << "Error: cannot initialize ROS node\n";
      return core::kExitRuntime;
    }
  }
  if (emit_ros) {
    ros2::BridgeConfig ros_cfg;
    ros2::cli_options::make_bridge_config(
        ros_cfg, o.ros_node ? *o.ros_node : "", o.ros_ns ? *o.ros_ns : "",
        o.ros_signal_stem ? *o.ros_signal_stem : "",
        o.ros_particle_stem ? *o.ros_particle_stem : "", ros_qos,
        !ros_per_serial_flag);
    ros_transport.emplace(*ros_node, ros_cfg);
    ros_pub.emplace(ros_cfg, *ros_transport);
    if (!ros_pub->ok()) {
      out << "Error: ROS publish failed (" << ros2::signals_block_topic(ros_cfg)
          << " unavailable)\n";
      return core::kExitRuntime;
    }
  }
#else
  if (emit_ros) {
    out << "Error: --emit-ros needs a ROS2 build (rebuild with -DBUILD_ROS2=ON)\n";
    return core::kExitRuntime;
  }
  if (control_ros) {
    out << "Error: --control-ros needs a ROS2 build (rebuild with -DBUILD_ROS2=ON)\n";
    return core::kExitRuntime;
  }
#endif

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
  std::optional<emit::SignalSubsetSink> uart_subset;
  std::optional<emit::StrideSink> uart_stride;
#ifdef WENGINE_HAS_RCLCPP
  std::optional<emit::SignalSubsetSink> ros_subset;
  std::optional<emit::StrideSink> ros_stride;
#endif
  emit::ISink *csv_out = nullptr;
  emit::ISink *shm_out = nullptr;
  emit::ISink *tcp_out = nullptr;
  emit::ISink *udp_out = nullptr;
  emit::ISink *uart_out = nullptr;
#ifdef WENGINE_HAS_RCLCPP
  emit::ISink *ros_out = nullptr;
#endif
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
  if (uart) {
    uart_out = &*uart;
    if (uart_sampling.select_given) {
      uart_subset.emplace(*uart_out);
      uart_subset->set_subscription(uart_sampling.serials);
      uart_out = &*uart_subset;
    }
    if (uart_sampling.stride_given && uart_sampling.stride != 1) {
      uart_stride.emplace(*uart_out, uart_sampling.stride);
      uart_out = &*uart_stride;
    }
  }
#ifdef WENGINE_HAS_RCLCPP
  if (ros_pub) {
    ros_out = &*ros_pub;
    if (ros_sampling.select_given) {
      ros_subset.emplace(*ros_out);
      ros_subset->set_subscription(ros_sampling.serials);
      ros_out = &*ros_subset;
    }
    if (ros_sampling.stride_given && ros_sampling.stride != 1) {
      ros_stride.emplace(*ros_out, ros_sampling.stride);
      ros_out = &*ros_stride;
    }
  }
#endif

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
  // Live plot feed (in-process SDL2 window): a best-effort ISink that
  // copies every emitted tick into the window's rolling history. Opened
  // after validation/build so usage errors never pop a window.
  PlotWindow plot_win;
  struct PlotFeed final : public emit::ISink {
    PlotWindow *win = nullptr;
    void on_snapshot(const world::Snapshot &s) override {
      if (win != nullptr && s.sig_value.size() == s.sig_serial.size()) {
        win->push(s.sim_time, s.sig_serial.data(), s.sig_value.data(), s.sig_value.size());
      }
    }
  };
  PlotFeed plot_feed;
  if (plot) {
    if (!PlotWindow::supported()) {
      out << "Error: --plot needs an SDL2 build (rebuild with SDL2 available)\n";
      return core::kExitRuntime;
    }
    if (!plot_win.open("sig run --plot")) {
      out << "Error: cannot open plot window (no display?)\n";
      return core::kExitRuntime;
    }
    plot_feed.win = &plot_win;
  }
  bool plot_quit = false;
  auto plot_pump = [&] {
    if (!plot || plot_quit) {
      return;
    }
    if (!plot_win.poll()) {
      plot_quit = true;
      return;
    }
    plot_win.render();
  };

  world::State s;
  std::array<emit::ISink *, 7> sink_arr{};
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
  if (uart_out) {
    sink_arr[n_sinks++] = uart_out;
  }
#ifdef WENGINE_HAS_RCLCPP
  if (ros_out) {
    sink_arr[n_sinks++] = ros_out;
  }
#endif
  if (plot) {
    sink_arr[n_sinks++] = &plot_feed;
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
  // UART control (SIG-8, D4b-A) shares the stream port: commands arrive
  // as control frames on the same wire and drain after the TCP slots.
  std::optional<link::uart::UartControl> uctl;
  if (control_uart) {
    uctl.emplace(*uart_port, uart_mode);
    if (!uctl->ok()) {
      out << "Error: UART control failed on '" << uart_port->device() << "'\n";
      return core::kExitRuntime;
    }
  }
  std::uint64_t control_run_id = 0;
  if (ctl) {
    control_run_id = ctl->run_id();
  } else if (uctl) {
    control_run_id = uctl->run_id();
  }
#ifdef WENGINE_HAS_RCLCPP
  else if (control_ros) {
    control_run_id =
        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
  }
#endif
  const bool has_control = ctl.has_value() || uctl.has_value()
#ifdef WENGINE_HAS_RCLCPP
                           || control_ros
#endif
      ;
  solvers::ControlBus ctl_bus(control_run_id);
  ctl_bus.add_channel(); // channel 0: local CLI (reserved, first in drain order)
  if (ctl) {
    for (std::uint32_t i = 0; i < ctl->max_clients(); ++i) {
      ctl_bus.add_channel(); // channel 1+i: TCP slot i (fixed order)
    }
  }
  std::uint32_t uart_channel = 0;
  if (uctl) {
    uart_channel = ctl_bus.add_channel(); // UART channel: after TCP slots
  }
#ifdef WENGINE_HAS_RCLCPP
  // ROS service control (ROS2-6): the six SigCommand services callbacks
  // enqueue only (same vocabulary as TCP/UART); the driver drains at the
  // step boundary in slot order. sig_list serves the driver-refreshed
  // census cache below (stale by <1 step, like the bridge).
  std::optional<ros2::RosControlService> ros_ctl;
  ros2::control::CensusCache ros_census;
  bool ros_spin_dead = false;
  if (control_ros) {
    // ros_node is guaranteed here (created when emit_ros || control_ros).
    const std::string_view ros_ns = o.ros_ns ? *o.ros_ns : "wengine";
    ros_ctl.emplace(*ros_node, ros_ns, ctl_bus, &ros_census);
    if (!ros_ctl->ok()) {
      out << "Error: ROS control services failed on '/" << ros_ns << "'\n";
      return core::kExitRuntime;
    }
    ros2::control::refresh_census(run.state(), run.state().step_index, ros_census);
  }
#endif
  std::vector<link::ControlSlotLines> ctl_polled;
  std::vector<std::string> uctl_lines;
  auto ctl_service = [&] {
    if (!has_control) {
      return;
    }
    if (ctl) {
      ctl->poll(ctl_polled);
      for (auto &sl : ctl_polled) {
        for (auto &ln : sl.lines) {
          ctl_bus.enqueue(1 + sl.slot, std::move(ln));
        }
      }
    }
    if (uctl) {
      uctl->poll_lines(uctl_lines);
      for (auto &ln : uctl_lines) {
        ctl_bus.enqueue(uart_channel, std::move(ln));
      }
    }
#ifdef WENGINE_HAS_RCLCPP
    // ROS callbacks run here, on the driver thread: they only enqueue
    // (same contract as the TCP poll above), so nothing reenters the
    // Runner and no state is touched. A dead context ends the run loudly
    // below instead of stepping blind.
    if (ros_ctl && ros_node && !ros_spin_dead) {
      try {
        rclcpp::spin_some(ros_node);
      } catch (...) {
        ros_spin_dead = true;
      }
    }
#endif
    ctl_bus.drain(run.state(), run.state().step_index);
    if (ctl) {
      for (std::uint32_t c = 1; c < ctl_bus.channel_count(); ++c) {
        if (uctl && c == uart_channel) {
          continue; // UART channel answers over the serial wire below
        }
#ifdef WENGINE_HAS_RCLCPP
        if (ros_ctl && c == ros_ctl->channel()) {
          continue; // ROS services answer the enqueue verdict at call time
        }
#endif
        for (auto &r : ctl_bus.take_responses(c)) {
          ctl->push_response(c - 1, r);
        }
      }
      ctl->flush();
    }
    if (uctl) {
      for (auto &r : ctl_bus.take_responses(uart_channel)) {
        uctl->push_response(r);
      }
      uctl->flush();
    }
#ifdef WENGINE_HAS_RCLCPP
    if (ros_ctl) {
      // Service responses were already sent at enqueue time (the bridge
      // contract); the execution outbox is driver-side accounting only.
      (void)ctl_bus.take_responses(ros_ctl->channel());
      ros2::control::refresh_census(run.state(), run.state().step_index, ros_census);
    }
#endif
  };
  // Paced waits only buffer arrivals (poll) and flush answers; the drain
  // stays in before_chunk so commands still apply at exact step starts.
  auto ctl_on_idle = [&] {
    if (!has_control) {
      return;
    }
    if (ctl) {
      ctl->poll(ctl_polled);
      for (auto &sl : ctl_polled) {
        for (auto &ln : sl.lines) {
          ctl_bus.enqueue(1 + sl.slot, std::move(ln));
        }
      }
      ctl->flush();
    }
    if (uctl) {
      uctl->poll_lines(uctl_lines);
      for (auto &ln : uctl_lines) {
        ctl_bus.enqueue(uart_channel, std::move(ln));
      }
      uctl->flush();
    }
#ifdef WENGINE_HAS_RCLCPP
    if (ros_ctl && ros_node && !ros_spin_dead) {
      try {
        rclcpp::spin_some(ros_node);
      } catch (...) {
        ros_spin_dead = true;
      }
    }
#endif
  };
  using SignalHandler = void (*)(int);
  SignalHandler previous_sigint = nullptr;
  const bool handles_sigint = forever || paced || plot;
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
  const link::uart::UartPublisher *uart_live = uart ? &*uart : nullptr;
#ifdef WENGINE_HAS_RCLCPP
  const bool has_live = shm || tcp || udp || uart || ros_pub;
#else
  const bool has_live = shm || tcp || udp || uart;
#endif
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
  std::uint64_t done = 0;
  cli::progress_begin(total_steps);
  if (paced) {
    const auto epoch = solvers::paced::SteadyClock::now();
    auto last_report = std::chrono::steady_clock::now();
    auto idle = [&] {
      ctl_on_idle();
      plot_pump();
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
    auto before_chunk = [&](std::uint64_t d) {
      ctl_service();
      plot_pump();
      cli::progress_update(d);
    };
    // Window close stops paced runs like Ctrl-C (chunk granularity: 1 step).
    auto paced_stop = [&] {
      return combined_stop() || plot_quit
#ifdef WENGINE_HAS_RCLCPP
             || ros_spin_dead
#endif
          ;
    };
    st = solvers::paced::run_paced<solvers::paced::SteadyClock>(run, total_steps, rate_hz, 1, epoch, paced_stop, idle, paced_stats, before_chunk);
    done = paced_stats.steps_done;
    cli::progress_update(done);
  } else if (has_control) {
    // Step-synchronous control: poll + drain before every single step
    // (finite and --forever alike); health stays chunked as usual. The TUI
    // async worker stops here at a step boundary via the global cancel atom.
    done = 0;
    while (done < total_steps) {
      if (cli::cancel_requested() || (forever && sigint_requested()) || plot_quit
#ifdef WENGINE_HAS_RCLCPP
          || ros_spin_dead
#endif
      ) {
        break;
      }
      ctl_service();
      st = run.run(1);
      if (!st.ok()) {
        break;
      }
      ++done;
      plot_pump();
      cli::progress_update(done);
      if (emit_health && has_live && done % kHealthChunkSteps == 0) {
        print_live_health(done, shm, tcp, udp, uart_live, wall_elapsed_s(), sample_rate_hz);
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
    // The TUI async worker stops here the same way (global cancel atom).
    done = 0;
    while (!combined_stop() && !plot_quit) {
      st = run.run(kHealthChunkSteps, &combined_stop);
      if (!st.ok()) {
        break;
      }
      done = run.state().step_index;
      plot_pump();
      cli::progress_update(done);
      if (emit_health && has_live) {
        print_live_health(done, shm, tcp, udp, uart_live, wall_elapsed_s(), sample_rate_hz);
      }
    }
    if (st.ok()) {
      st = core::Status::success();
    }
  } else {
    done = 0;
    while (done < total_steps) {
      if (cli::cancel_requested() || plot_quit) {
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
      plot_pump();
      cli::progress_update(done);
      if (emit_health && has_live && done < total_steps) {
        print_live_health(done, shm, tcp, udp, uart_live, wall_elapsed_s(), sample_rate_hz);
      }
    }
  }
  cli::progress_end();
  if (plot && !plot_quit && !sigint_requested() && !cli::cancel_requested()) {
    // Finite runs park the final frame: the sim already finished, so this
    // loop is display-only until the window closes (or Ctrl-C).
    plot_win.render();
    while (plot_win.poll() && !sigint_requested() && !cli::cancel_requested()) {
      plot_win.render();
      PlotWindow::wait_frame();
    }
    plot_quit = plot_win.should_close();
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
#ifdef WENGINE_HAS_RCLCPP
  if (ros_spin_dead) {
    out << "Error: ROS spin failed (control services unavailable)\n";
    return core::kExitRuntime;
  }
#endif
  if (!st.ok()) {
    out << "Error: simulation step failed (partial step state is not a completed tick)\n";
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
  if (forever || done < total_steps) {
    out << "steps: interrupted by " << (plot_quit ? "window close" : "Ctrl-C") << " at "
        << s.step_index << " x " << dt << "s -> t=" << s.sim_time << "s\n";
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
#ifdef WENGINE_HAS_RCLCPP
  if (ros_pub) {
    const ros2::BridgeConfig &bcfg = ros_pub->config();
    if (!ros_pub->ok()) {
      out << "Error: ROS publish failed on '" << ros2::signals_block_topic(bcfg) << "' ("
          << link::format_publisher_health(ros_pub->stats(), ros_pub->status(), 0, 0, 0, 0)
          << ")\n";
      return core::kExitRuntime;
    }
    out << "ros: " << ros2::grouped_signals_topic(bcfg);
    if (bcfg.grouped_signals) {
      out << " + " << ros2::signals_block_topic(bcfg);
    }
    out << " (grouped=" << (bcfg.grouped_signals ? "yes" : "no")
        << ", qos=" << ros2::cli_options::qos_name(bcfg.qos) << ", "
        << ros_pub->stats().published << " frames";
    if (emit_health) {
      out << ", " << link::format_publisher_health(ros_pub->stats(), ros_pub->status(), 0, 0, 0, 0);
    }
    if (ros_sampling.select_given) {
      out << ", select=" << cli::format_emit_select(ros_sampling.serials);
      if (ros_subset) {
        out << " matched=" << ros_subset->matched() << " dropped=" << ros_subset->dropped()
            << " forwarded_rows=" << ros_subset->forwarded_rows();
      }
    }
    if (ros_stride) {
      out << ", stride=" << ros_stride->stride() << " matched=" << ros_stride->matched()
          << " dropped=" << ros_stride->dropped();
    }
    out << ")\n";
  }
#endif
  if (ctl) {
    ctl->flush();
    out << "control: " << ctl->endpoint() << " (applied=" << ctl_bus.applied()
        << " readonly=" << ctl_bus.readonly() << " rejected=" << ctl_bus.rejected()
        << " clients=" << ctl->stats().accepted << ")\n";
  }
  if (uctl) {
    uctl->flush();
    out << "control-uart: " << uart_port->device() << " (malformed="
        << uctl->malformed() << " commands=" << uctl->commands_rx()
        << " responses=" << uctl->responses_tx()
        << " dropped=" << uctl->responses_dropped() << ")\n";
  }
#ifdef WENGINE_HAS_RCLCPP
  if (ros_ctl) {
    out << "control-ros: /" << (o.ros_ns ? *o.ros_ns : "wengine")
        << " (services=" << ros_ctl->service_count()
        << " enqueued=" << ros_ctl->enqueued() << " refused=" << ros_ctl->refused()
        << " applied=" << ctl_bus.applied() << " readonly=" << ctl_bus.readonly()
        << " rejected=" << ctl_bus.rejected() << ")\n";
  }
#endif
  if (plot) {
    out << "plot: live window (" << plot_win.samples() << " samples" << (plot_quit ? ", closed by user" : "") << ")\n";
  }
  return core::kExitOk;
}

// SIG-6 per-source family: thin TCP clients of the SIG-10 control plane
// (same verbs the server drains: LIST/ADD/SET/START/STOP/DROP). Each mode
// connects to a live `sig run --control-tcp <host:port>`, sends its lines,
// and prints the server verdicts. Unit-suffixed values (3.3V, 1kHz) parse
// at this frontend edge — the wire carries SI doubles only.

[[nodiscard]] std::string fmt_wire_f64(double v) {
  char buf[32];
  // %.17g round-trips IEEE doubles exactly (same guarantee as the CSV log
  // and the control LIST lines); the engine parses them back with from_chars.
  const int n = std::snprintf(buf, sizeof(buf), "%.17g", v);
  if (n <= 0 || static_cast<std::size_t>(n) >= sizeof(buf)) {
    return "0";
  }
  return std::string(buf, static_cast<std::size_t>(n));
}

[[nodiscard]] bool is_ok_verdict(std::string_view line) noexcept {
  return line.size() >= 2 && line.substr(0, 2) == "OK" &&
         (line.size() == 2 || line[2] == ' ');
}

// One command line against the server: send, read the single-line verdict,
// print it. False on transport failure (sticky client status) — the caller
// reports the connection error.
bool control_transact(link::ControlClient &ctl, const std::string &line, std::ostream &out,
                      bool &ok_out) {
  if (!ctl.send_line(line)) {
    return false;
  }
  std::string verdict;
  if (!ctl.recv_line(verdict)) {
    return false;
  }
  out << verdict << '\n';
  ok_out = is_ok_verdict(verdict);
  return true;
}

int run_control(ArgCursor &args, std::ostream &out, std::string_view mode,
                std::span<const std::string_view> pos, const SigCliOptions &o) {
  // Batch/transport values belong to sig run (taken before dispatch so bare
  // words never read as positionals — rejected here, never ignored).
  if (has_batch_options(o)) {
    return usage_error(out, "batch/transport options need sig run");
  }
  const auto o_ctl = o.control_tcp;
  if (!o_ctl) {
    return usage_error(out, "control mode needs --control-tcp <host:port>");
  }
  if (!link::parse_endpoint(*o_ctl).ok()) {
    return usage_error(out, "invalid control endpoint (want host:port)");
  }

  // Serial-addressed modes take exactly one positional (the stable source
  // serial); list/add take none.
  const bool wants_serial = mode == "set" || mode == "start" || mode == "stop" || mode == "drop";
  long long serial = -1;
  if (wants_serial) {
    if (pos.size() != 1) {
      return usage_error(out, "want one source serial");
    }
    const auto p = parse_i64(pos[0]);
    if (!p.ok() || p.value < 0) {
      return usage_error(out, "invalid source serial");
    }
    serial = p.value;
  } else if (!pos.empty()) {
    return usage_error(out, "too many positional arguments");
  }

  // Signal vocabulary (same --sig-* options as the batch run, but each
  // source is configured individually — including sums, which batch runs
  // reject for lack of distinct per-source operands). Values arrive
  // pre-taken (see take_sig_options).
  const auto o_sig_kind = o.sig_kind;
  const auto o_sig_freq = o.sig_freq;
  const auto o_sig_amp = o.sig_amp;
  const auto o_sig_offset = o.sig_offset;
  const auto o_sig_phase = o.sig_phase;
  const auto o_sig_duty = o.sig_duty;
  const auto o_sig_seed = o.sig_seed;
  const auto o_sig_quantity = o.sig_quantity;
  const auto o_sig_operands = o.sig_operands;
  const auto o_sig_expr = o.sig_expr;
  const auto o_sig_table = o.sig_table;
  const auto o_sig_lua = o.sig_lua;
  const auto o_sig_count = o.sig_count;

  // Batch-only payloads never belong to one-line control verbs.
  if (o_sig_table) {
    return usage_error(out, "--sig-table needs a batch run (sig run --sig-kind table)");
  }
  if (o_sig_lua) {
    return usage_error(out, "--sig-lua needs a batch run (sig run --sig-kind lua)");
  }
  if (o_sig_count) {
    return usage_error(out, "--sig-count is batch-only (sig add creates one source)");
  }

  signal::SignalKind kind = signal::SignalKind::kSine;
  if (o_sig_kind) {
    const auto k = signal::parse_kind(*o_sig_kind);
    if (!k.ok()) {
      return usage_error(out, "unknown signal kind (want sine|square|triangle|saw|dc|"
                              "pulse|noise|sum|expression|table|lua)");
    }
    kind = k.value;
  }
  signal::QuantityId quantity = signal::QuantityId::kVoltage;
  if (o_sig_quantity) {
    const auto q = signal::parse_quantity(*o_sig_quantity);
    if (!q.ok()) {
      return usage_error(out, "unknown signal quantity (want U|I)");
    }
    quantity = q.value;
  }
  if (kind == signal::SignalKind::kTable || kind == signal::SignalKind::kLua) {
    return usage_error(out, "table/lua need bulk payloads (use a batch run)");
  }

  // Quantity-aware value parsing through the units-owned tables (the same
  // dimensional firewall as the batch run: "2mA" as volts fails here).
  auto take_qty = [&](std::optional<std::string_view> v, const char *what,
                      double &slot) -> bool {
    if (v) {
      const auto p = parse_quantity_value(quantity, *v);
      if (!p.ok()) {
        out << "Error: invalid " << what << " '" << *v << "'\n";
        out << SigSpec::kUsage << '\n';
        return false;
      }
      slot = p.value;
    }
    return true;
  };
  auto take_num = [&](std::optional<std::string_view> v, const char *what,
                      core::Result<double> (*parse)(std::string_view),
                      double &slot) -> bool {
    if (v) {
      const auto p = parse(*v);
      if (!p.ok()) {
        out << "Error: invalid " << what << " '" << *v << "'\n";
        out << SigSpec::kUsage << '\n';
        return false;
      }
      slot = p.value;
    }
    return true;
  };

  std::vector<std::string> lines; // control lines to send, in order
  if (mode == "add") {
    signal::SignalParams params;
    params.kind = kind;
    params.quantity = quantity;
    double freq = params.freq_hz;
    double amp = params.amp;
    double offset = params.offset;
    double phase = params.phase_rad;
    double duty = params.duty;
    if (!take_num(o_sig_freq, "signal frequency", parse_frequency, freq) ||
        !take_qty(o_sig_amp, "signal amplitude", amp) ||
        !take_qty(o_sig_offset, "signal offset", offset) ||
        !take_num(o_sig_phase, "signal phase", parse_double, phase) ||
        !take_num(o_sig_duty, "signal duty", parse_double, duty)) {
      return core::kExitUsage;
    }
    std::uint64_t seed = params.seed;
    if (o_sig_seed) {
      const auto p = parse_i64(*o_sig_seed);
      if (!p.ok() || p.value < 0) {
        return usage_error(out, "invalid signal seed");
      }
      seed = static_cast<std::uint64_t>(p.value);
    }
    params.freq_hz = freq;
    params.amp = amp;
    params.offset = offset;
    params.phase_rad = phase;
    params.duty = duty;
    params.seed = seed;
    // Cross-kind leftovers are usage errors, never silently ignored (same
    // rule as the batch run).
    const bool is_expr = kind == signal::SignalKind::kExpression;
    const bool is_sum = kind == signal::SignalKind::kSum;
    if (is_expr && !o_sig_expr) {
      return usage_error(out, "expression kind needs --sig-expr '<math>'");
    }
    if (!is_expr && o_sig_expr) {
      return usage_error(out, "--sig-expr needs --sig-kind expression");
    }
    std::vector<std::uint64_t> operands;
    if (o_sig_operands) {
      if (!is_sum) {
        return usage_error(out, "--sig-operands needs --sig-kind sum");
      }
      std::string_view tail = *o_sig_operands;
      while (!tail.empty()) {
        const std::size_t comma = tail.find(',');
        const std::string_view item =
            (comma == std::string_view::npos) ? tail : tail.substr(0, comma);
        const auto p = parse_i64(item);
        if (!p.ok() || p.value < 0) {
          return usage_error(out, "invalid operands (want e.g. 0,2,5)");
        }
        operands.push_back(static_cast<std::uint64_t>(p.value));
        tail = (comma == std::string_view::npos) ? std::string_view{} : tail.substr(comma + 1);
      }
      if (operands.empty() || operands.size() > signal::SignalParams::kMaxOps) {
        return usage_error(out, "invalid operands (want 1..8 serials)");
      }
    } else if (is_sum) {
      return usage_error(out, "sum needs --sig-operands <s0,s1>");
    }
    // Operand serials ride in params for validation (existence/kind is the
    // engine's check at apply time — serials may legitimately reference
    // sources the CLI cannot see).
    params.op_count = static_cast<std::uint8_t>(operands.size());
    for (std::size_t k = 0; k < operands.size(); ++k) {
      params.ops[k] = operands[k];
    }
    if (is_expr) {
      const auto prog = signal::parse_expression(*o_sig_expr);
      if (!prog.ok()) {
        out << "Error: invalid expression '" << *o_sig_expr << "' (" << prog.error << ")\n";
        out << SigSpec::kUsage << '\n';
        return core::kExitUsage;
      }
      params.expr = prog.value;
      if (!signal::eval_expression(params.expr, 0.0).ok()) {
        return usage_error(out, "expression fails at t=0");
      }
    }
    if (!signal::validate_params(params).ok()) {
      return usage_error(out, "invalid signal params (finite values, freq > 0, duty in 0..1)");
    }
    // One ADD line (EXPR last: the rest of the line is program text).
    std::string line =
        "ADD " + std::string(signal::kKindNames[static_cast<std::size_t>(kind)]) + " " +
        std::string(signal::quantity_suffix(quantity));
    if (o_sig_freq) {
      line += " FREQ " + fmt_wire_f64(freq);
    }
    if (o_sig_amp) {
      line += " AMP " + fmt_wire_f64(amp);
    }
    if (o_sig_offset) {
      line += " OFFSET " + fmt_wire_f64(offset);
    }
    if (o_sig_phase) {
      line += " PHASE " + fmt_wire_f64(phase);
    }
    if (o_sig_duty) {
      line += " DUTY " + fmt_wire_f64(duty);
    }
    if (o_sig_seed) {
      line += " SEED " + std::to_string(seed);
    }
    if (is_sum) {
      line += " OPS ";
      for (std::size_t k = 0; k < operands.size(); ++k) {
        line += (k == 0 ? "" : ",") + std::to_string(operands[k]);
      }
    }
    if (is_expr) {
      line += " EXPR ";
      line += *o_sig_expr;
    }
    lines.push_back(std::move(line));
  } else if (mode == "set") {
    if (!o_sig_freq && !o_sig_amp && !o_sig_offset && !o_sig_phase && !o_sig_duty &&
        !o_sig_seed && !o_sig_quantity) {
      return usage_error(out, "set needs at least one --sig-* parameter");
    }
    if (o_sig_kind || o_sig_operands || o_sig_expr) {
      return usage_error(out, "set takes values only (--sig-freq/amp/offset/phase/"
                              "duty/seed/quantity)");
    }
    double freq = 0.0;
    double amp = 0.0;
    double offset = 0.0;
    double phase = 0.0;
    double duty = 0.0;
    if (!take_num(o_sig_freq, "signal frequency", parse_frequency, freq) ||
        !take_qty(o_sig_amp, "signal amplitude", amp) ||
        !take_qty(o_sig_offset, "signal offset", offset) ||
        !take_num(o_sig_phase, "signal phase", parse_double, phase) ||
        !take_num(o_sig_duty, "signal duty", parse_double, duty)) {
      return core::kExitUsage;
    }
    const std::string id = std::to_string(serial);
    if (o_sig_freq) {
      lines.push_back("SET " + id + " FREQ " + fmt_wire_f64(freq));
    }
    if (o_sig_amp) {
      lines.push_back("SET " + id + " AMP " + fmt_wire_f64(amp));
    }
    if (o_sig_offset) {
      lines.push_back("SET " + id + " OFFSET " + fmt_wire_f64(offset));
    }
    if (o_sig_phase) {
      lines.push_back("SET " + id + " PHASE " + fmt_wire_f64(phase));
    }
    if (o_sig_duty) {
      lines.push_back("SET " + id + " DUTY " + fmt_wire_f64(duty));
    }
    if (o_sig_seed) {
      const auto p = parse_i64(*o_sig_seed);
      if (!p.ok() || p.value < 0) {
        return usage_error(out, "invalid signal seed");
      }
      lines.push_back("SET " + id + " SEED " + std::to_string(p.value));
    }
    if (o_sig_quantity) {
      lines.push_back("SET " + id + " QUANTITY " +
                      std::string(signal::quantity_suffix(quantity)));
    }
  } else if (mode == "list") {
    if (o_sig_kind || o_sig_freq || o_sig_amp || o_sig_offset || o_sig_phase || o_sig_duty ||
        o_sig_seed || o_sig_quantity || o_sig_operands || o_sig_expr) {
      return usage_error(out, "list takes no signal options");
    }
    lines.emplace_back("LIST");
  } else {
    // start/stop/drop: single verb line over the stable serial.
    if (o_sig_kind || o_sig_freq || o_sig_amp || o_sig_offset || o_sig_phase || o_sig_duty ||
        o_sig_seed || o_sig_quantity || o_sig_operands || o_sig_expr) {
      return usage_error(out, "start/stop/drop take no signal options");
    }
    const std::string verb = mode == "start" ? "START" : (mode == "stop" ? "STOP" : "DROP");
    lines.push_back(verb + " " + std::to_string(serial));
  }

  if (args.has_remaining()) {
    return cli::unexpected_error(out, *args.first_remaining(), SigSpec::kUsage);
  }

  link::ControlClient ctl(*o_ctl);
  if (!ctl.ok()) {
    if (ctl.status().code == core::StatusCode::kInvalidArgument) {
      return usage_error(out, "invalid control endpoint (want host:port)");
    }
    out << "Error: cannot connect control '" << *o_ctl << "'\n";
    return core::kExitRuntime;
  }
  if (mode == "list") {
    // LIST answers with 1 + n lines: the verdict carries the SRC count, so
    // the read length is known up front (never read-until-quiet: that would
    // block on the recv timeout waiting for bytes that never come).
    if (!ctl.send_line(lines.front())) {
      out << "Error: control connection failed on '" << ctl.endpoint() << "'\n";
      return core::kExitRuntime;
    }
    std::string verdict;
    if (!ctl.recv_line(verdict)) {
      out << "Error: control connection failed on '" << ctl.endpoint() << "'\n";
      return core::kExitRuntime;
    }
    out << verdict << '\n';
    constexpr std::string_view kPrefix = "OK list ";
    if (verdict.substr(0, kPrefix.size()) != kPrefix) {
      return core::kExitRuntime; // ERR verdict (e.g. rejected) already printed
    }
    const auto count =
        parse_i64(std::string_view(verdict.data() + kPrefix.size(),
                                   verdict.size() - kPrefix.size()));
    if (!count.ok() || count.value < 0) {
      out << "Error: bad LIST response on '" << ctl.endpoint() << "'\n";
      return core::kExitRuntime;
    }
    for (long long k = 0; k < count.value; ++k) {
      std::string src;
      if (!ctl.recv_line(src)) {
        out << "Error: control connection failed on '" << ctl.endpoint() << "'\n";
        return core::kExitRuntime;
      }
      out << src << '\n';
    }
    return core::kExitOk;
  }
  bool all_ok = true;
  for (const std::string &line : lines) {
    bool ok_line = false;
    if (!control_transact(ctl, line, out, ok_line)) {
      out << "Error: control connection failed on '" << ctl.endpoint() << "'\n";
      return core::kExitRuntime;
    }
    all_ok = all_ok && ok_line;
  }
  return all_ok ? core::kExitOk : core::kExitRuntime;
}

int SigSpec::run(ArgCursor &args, std::ostream &out) {
  if (args.take_flag({"--help", "-h"})) {
    out << kName << " - " << kDescription << "\nUsage:\n" << kUsage << '\n';
    return core::kExitOk;
  }
  // Every value-taking --opt is consumed before positional scanning, so
  // bare values (dc, 5V, 1kHz, 100, file paths) never read as the mode or
  // a serial. Flags take no values and stay with their mode.
  SigCliOptions opt;
  take_sig_options(args, opt);
  // Mode is the first positional (options may precede it: `sig --sig-kind
  // sine add ...` works like the batch run's free option order).
  std::array<std::string_view, 3> pos{};
  std::size_t n_pos = 0;
  while (auto p = args.next_positional()) {
    if (n_pos >= pos.size()) {
      return usage_error(out, "too many positional arguments");
    }
    pos[n_pos++] = *p;
  }
  if (n_pos < 1) {
    return usage_error(out, "missing mode (run|list|add|set|start|stop|drop)");
  }
  const std::string_view mode = pos[0];
  if (!is_sig_mode(mode)) {
    return usage_error(out, "unknown mode (run|list|add|set|start|stop|drop)");
  }
  const std::span<const std::string_view> rest{pos.data() + 1, n_pos - 1};
  if (mode == "run") {
    return run_batch(args, out, rest, opt);
  }
  return run_control(args, out, mode, rest, opt);
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
  // Values for a preceding "--emit-uart-mode".
  if (!prior_args.empty() && prior_args.back() == "--emit-uart-mode") {
    for (std::string_view m : {"ascii", "cobs"}) {
      if (m.substr(0, partial.size()) == partial) {
        out.emplace_back(m);
      }
    }
    return;
  }
  // Values for a preceding "--ros-qos".
  if (!prior_args.empty() && prior_args.back() == ros2::cli_options::kRosQos) {
    for (std::string_view q : {ros2::cli_options::kQosLatest, ros2::cli_options::kQosReliable}) {
      if (q.substr(0, partial.size()) == partial) {
        out.emplace_back(q);
      }
    }
    return;
  }
  constexpr std::string_view kRosQosEq = "--ros-qos=";
  if (partial.substr(0, kRosQosEq.size()) == kRosQosEq) {
    const std::string_view sub = partial.substr(kRosQosEq.size());
    for (std::string_view q : {ros2::cli_options::kQosLatest, ros2::cli_options::kQosReliable}) {
      if (q.substr(0, sub.size()) == sub) {
        out.emplace_back(std::string{kRosQosEq} + std::string{q});
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
