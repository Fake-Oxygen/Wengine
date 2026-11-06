#include "wengine/cli/cancel.hpp"
#include "wengine/cli/commands.hpp"
#include "wengine/cli/emit_options.hpp"
#include "wengine/cli/status.hpp"
#include "wengine/core/run_config.hpp"
#include "wengine/emit/csv_sink.hpp"
#include "wengine/emit/logic_sink.hpp"
#include "wengine/emit/routing.hpp"
#include "wengine/emit/signal_sink.hpp"
#include "wengine/link/frame.hpp"
#include "wengine/link/output_report.hpp"
#include "wengine/link/protocol.hpp"
#include "wengine/link/shm_publisher.hpp"
#include "wengine/link/socket_publisher.hpp"
#include "wengine/link/uart_publisher.hpp"
#include "wengine/logic/clock_domains.hpp"
#include "wengine/logic/netlist.hpp"
#include "wengine/logic/seq_config.hpp"
#include "wengine/logic/yaml_store.hpp"
#include "wengine/ros2/cli_options.hpp"
#include "wengine/ros2/publisher.hpp"
#ifdef WENGINE_HAS_RCLCPP
#include "rclcpp/rclcpp.hpp"
#include "wengine/ros2/rclcpp_transport.hpp"
#endif
#include "wengine/signal/types.hpp"
#include "wengine/solvers/configured_run.hpp"
#include "wengine/solvers/paced_run.hpp"
#include "wengine/solvers/run.hpp"
#include "wengine/solvers/solver_registry.hpp"

#include <array>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <csignal>
#include <fstream>
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

constexpr std::array<std::string_view, 48> kSimOptions{{
    "--steps", "-n", "--dt", "--solver", "--gravity", "-g", "--particles", "-p", "--x0",
    "--y0", "--vx0", "--vy0", "--emit-csv", "--emit-shm", "--emit-tcp", "--emit-udp",
    "--emit-every", "--emit-csv-stride", "--emit-shm-stride", "--emit-tcp-stride",
    "--emit-udp-stride", "--emit-udp-budget", "--emit-csv-select", "--emit-shm-select",
    "--emit-tcp-select", "--emit-udp-select", "--emit-uart", "--emit-uart-mode",
    "--emit-uart-stride", "--emit-uart-select", "--emit-ros", "--emit-ros-stride",
    "--emit-ros-select", "--ros-node", "--ros-ns", "--ros-signal-stem",
    "--ros-particle-stem", "--ros-qos", "--ros-grouped", "--ros-per-serial",
    "--emit-health", "--paced", "--rate",
    "--seq-file", "--seq-clocks", "--seq-from",
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

// Sequential section builders (005-US4, 006): --seq-file/--seq-clocks/
// --seq-from lower to logic::SequentialRunConfig (parsed values only).
// Domain names use the single engine-owned derivation (one domain per
// clock-driver net over edge flops (DFF/JK/T), named by the source pin
// (e.g. "BUF1.Y", "DFF2.NQ"); dangling CLK pins join the implicit "clk"
// domain; level latches belong to no domain) — the same vocabulary the
// session step verb and the file's `clocks:` section share. Matching is
// case-insensitive like the session step verb; the engine re-validates
// the flattened slot list.

[[nodiscard]] inline bool seq_eq_ci(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t k = 0; k < a.size(); ++k) {
    char ca = a[k];
    char cb = b[k];
    if (ca >= 'a' && ca <= 'z') {
      ca = static_cast<char>(ca - ('a' - 'A'));
    }
    if (cb >= 'a' && cb <= 'z') {
      cb = static_cast<char>(cb - ('a' - 'A'));
    }
    if (ca != cb) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] inline std::string seq_gate_name(const logic::Netlist &nl, std::uint16_t slot) {
  std::string s(logic::gate_kind_name(nl.gates[slot].kind));
  s += std::to_string(nl.gates[slot].counter);
  return s;
}

struct SeqDomain {
  std::string name;
  std::vector<std::uint16_t> members;
};

[[nodiscard]] inline std::vector<SeqDomain> seq_clock_domains(const logic::Netlist &nl) {
  // Single engine-owned derivation (006), adapted to the local vocabulary
  // type used by the --seq-clocks lowering and completion below.
  std::vector<SeqDomain> domains;
  for (const logic::ClockDomain &d : logic::derive_clock_domains(nl)) {
    SeqDomain out;
    out.name = d.name;
    out.members = d.members;
    domains.push_back(std::move(out));
  }
  return domains;
}

// Canonical input-pin text for block-interface pins (mirrors the session
// pin_text vocabulary so completion and --seq-from accept the same spellings
// the interactive verbs show: DFF D/CLK/RST/SET/EN, DLATCH D/EN, JK J/K/CLK,
// T T/CLK, SRLATCH S/R, numeric elsewhere).
[[nodiscard]] inline std::string seq_pin_text(const logic::Netlist &nl, std::uint16_t slot,
                                              std::uint8_t input) {
  const logic::Gate &g = nl.gates[slot];
  if (!logic::is_sequential_kind(g.kind)) {
    return seq_gate_name(nl, slot) + '.' + std::to_string(static_cast<unsigned>(input) + 1u);
  }
  std::string s = seq_gate_name(nl, slot);
  s += '.';
  if (g.kind == logic::GateKind::kJk) {
    s += (input == 0) ? "J" : (input == 1) ? "K" : "CLK";
  } else if (g.kind == logic::GateKind::kT) {
    s += (input == 0) ? "T" : "CLK";
  } else if (g.kind == logic::GateKind::kSrlatch) {
    s += (input == 0) ? "S" : "R";
  } else if (input == 0) {
    s += 'D';
  } else if (g.kind == logic::GateKind::kDff && input >= 2) {
    s += (input == 2) ? "RST" : (input == 3) ? "SET" : "EN";
  } else {
    s += (g.kind == logic::GateKind::kDff) ? "CLK" : "EN";
  }
  return s;
}

// Block-interface input pins in interface order (slot order, tie leaders
// only), like the session collect_block_inputs.
inline void seq_collect_block_inputs(const logic::Netlist &nl, std::vector<std::string> &out) {
  for (std::uint16_t s = 0; s < logic::kMaxGates; ++s) {
    const logic::Gate &g = nl.gates[s];
    if (!g.live) {
      continue;
    }
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      if (g.drivers[i] != logic::kNoDriver) {
        continue;
      }
      if ((g.ignored_mask & static_cast<std::uint8_t>(1u << i)) != 0) {
        continue;
      }
      const std::size_t flat = static_cast<std::size_t>(s) * logic::kMaxInputs + i;
      if (logic::tie_leader_flat(nl, flat) != flat) {
        continue;
      }
      out.push_back(seq_pin_text(nl, s, i));
    }
  }
}

// Compose the section from the three flags: load the workspace, resolve
// domain names to flattened member slots, and compose the stimulus vector
// in block-interface order from --seq-from overrides (unset pins read Low,
// except an untied DFF clock-enable which reads High — the same rule as the
// session compose_block_inputs with an empty poke map, so batch and
// interactive stimuli agree pin for pin).
[[nodiscard]] inline bool seq_build_section(std::string_view file,
                                            const std::optional<std::string_view> &clocks,
                                            const std::vector<std::string_view> &from,
                                            logic::SequentialRunConfig &out, std::string &err) {
  const auto fail = [&](std::string msg) {
    err = std::move(msg);
    return false;
  };
  std::ifstream f(std::string(file), std::ios::binary);
  if (!f) {
    return fail(std::string("cannot open --seq-file '") + std::string(file) + "'");
  }
  std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (!f.eof() && !f) {
    return fail(std::string("cannot read --seq-file '") + std::string(file) + "'");
  }
  logic::Netlist nl;
  std::string load_err;
  logic::yaml::Workspace ws;
  if (!logic::yaml::load_text(text, ws, load_err)) {
    return fail(std::string("cannot load --seq-file '") + std::string(file) + "': " + load_err);
  }
  nl = ws.netlist;
  // Domain selection: exactly one form — omitted means all domains.
  const std::vector<SeqDomain> domains = seq_clock_domains(nl);
  std::vector<std::uint16_t> members;
  const bool step_all = !clocks.has_value();
  if (clocks.has_value()) {
    const std::string_view list = *clocks;
    if (list.empty()) {
      return fail("empty --seq-clocks selection steps nothing (omit the flag for all domains)");
    }
    std::size_t begin = 0;
    while (true) {
      const std::size_t comma = list.find(',', begin);
      const std::string_view name =
          (comma == std::string_view::npos) ? list.substr(begin) : list.substr(begin, comma - begin);
      const SeqDomain *hit = nullptr;
      for (const SeqDomain &d : domains) {
        if (seq_eq_ci(d.name, name)) {
          hit = &d;
          break;
        }
      }
      if (hit == nullptr) {
        std::string want;
        for (const SeqDomain &d : domains) {
          if (!want.empty()) {
            want += '|';
          }
          want += d.name;
        }
        return fail(std::string("unknown clock domain '") + std::string(name) +
                    "' (want " + want + ")");
      }
      for (std::uint16_t h : hit->members) {
        for (std::uint16_t m : members) {
          if (m == h) {
            return fail(std::string("duplicate clock domain '") + std::string(name) +
                        "' in --seq-clocks");
          }
        }
      }
      members.insert(members.end(), hit->members.begin(), hit->members.end());
      if (comma == std::string_view::npos) {
        break;
      }
      begin = comma + 1;
    }
    std::sort(members.begin(), members.end());
  }
  // Stimulus in block-interface order with session defaults.
  const std::size_t n_in = logic::block_input_count(nl);
  std::vector<logic::BlockPin> in_pins(n_in);
  std::vector<logic::BlockPin> out_pins(logic::block_output_count(nl));
  std::size_t nin_got = n_in;
  std::size_t nout_got = out_pins.size();
  if (!logic::block_interface(nl, in_pins.data(), in_pins.size(), &nin_got, out_pins.data(),
                              out_pins.size(), &nout_got)
           .ok()) {
    return fail("cannot derive block interface (internal error)");
  }
  std::vector<logic::LogicValue> stimulus;
  stimulus.reserve(n_in);
  for (const logic::BlockPin &p : in_pins) {
    const logic::Gate &g = nl.gates[p.slot];
    if (g.kind == logic::GateKind::kDff && p.input == 4 && !logic::is_tied(nl, p.slot, p.input)) {
      stimulus.push_back(logic::LogicValue::kHigh); // FR-008: unconnected EN asserted
    } else {
      stimulus.push_back(logic::LogicValue::kLow);
    }
  }
  for (std::string_view spec : from) {
    const std::size_t eq = spec.find('=');
    if (eq == std::string_view::npos) {
      return fail(std::string("bad --seq-from '") + std::string(spec) + "' (want PIN=0|1)");
    }
    const std::string_view pin = spec.substr(0, eq);
    const std::string_view bit = spec.substr(eq + 1);
    if (bit != "0" && bit != "1") {
      return fail(std::string("bad --seq-from '") + std::string(spec) + "' (want PIN=0|1)");
    }
    const auto parsed = logic::parse_pin(nl, pin);
    if (!parsed.ok()) {
      return fail(std::string("unknown pin '") + std::string(pin) + "' in --seq-from");
    }
    const logic::PinRef &ref = parsed.value;
    bool block_input = false;
    std::size_t index = 0;
    if (!ref.is_output && ref.slot < logic::kMaxGates) {
      const logic::Gate &g = nl.gates[ref.slot];
      if (g.live && ref.input < g.input_count && g.drivers[ref.input] == logic::kNoDriver &&
          (g.ignored_mask & static_cast<std::uint8_t>(1u << ref.input)) == 0) {
        const std::size_t flat =
            static_cast<std::size_t>(ref.slot) * logic::kMaxInputs + ref.input;
        if (logic::tie_leader_flat(nl, flat) == flat) {
          for (std::size_t k = 0; k < in_pins.size(); ++k) {
            const std::size_t lflat = static_cast<std::size_t>(in_pins[k].slot) *
                                          logic::kMaxInputs +
                                      in_pins[k].input;
            if (lflat == flat) {
              index = k;
              block_input = true;
              break;
            }
          }
        }
      }
    }
    if (!block_input) {
      return fail(std::string("pin '") + std::string(pin) +
                  "' is not a driven block input (want a dangling interface pin)");
    }
    stimulus[index] = (bit == "1") ? logic::LogicValue::kHigh : logic::LogicValue::kLow;
  }
  out.netlist = nl;
  out.init_q = ws.init;
  out.stimulus = std::move(stimulus);
  out.members = std::move(members);
  out.step_all_domains = step_all;
  out.set = true;
  return true;
}

// Load the --seq-file workspace named in prior_args for completion (quiet:
// any failure simply yields no candidates).
[[nodiscard]] inline bool seq_complete_workspace(std::span<const std::string_view> prior_args,
                                                 logic::Netlist &nl) {
  std::string_view file;
  bool have = false;
  for (std::size_t k = 0; k < prior_args.size(); ++k) {
    if (prior_args[k] == "--seq-file") {
      if (k + 1 < prior_args.size()) {
        file = prior_args[k + 1];
        have = true;
      }
      break;
    }
    constexpr std::string_view kEq = "--seq-file=";
    if (prior_args[k].size() > kEq.size() && prior_args[k].substr(0, kEq.size()) == kEq) {
      file = prior_args[k].substr(kEq.size());
      have = true;
      break;
    }
  }
  if (!have || file.empty()) {
    return false;
  }
  std::ifstream f(std::string(file), std::ios::binary);
  if (!f) {
    return false;
  }
  std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (!f.eof() && !f) {
    return false;
  }
  std::string err;
  logic::yaml::Workspace ws;
  if (!logic::yaml::load_text(text, ws, err)) {
    return false;
  }
  nl = ws.netlist;
  return true;
}

inline void complete_seq_from_value(const logic::Netlist &nl, std::string_view val,
                                    std::vector<std::string> &out) {
  // Mirrors the session complete_from_value: bare pin prefix -> pin names;
  // exact pin (or pin + bit prefix) -> valued PIN=0|1 forms.
  const std::size_t eq = val.find('=');
  if (eq == std::string_view::npos) {
    std::vector<std::string> pins;
    seq_collect_block_inputs(nl, pins);
    bool exact = false;
    for (const std::string &p : pins) {
      if (p == val) {
        exact = true;
        break;
      }
    }
    if (exact) {
      out.emplace_back(std::string(val) + "=0");
      out.emplace_back(std::string(val) + "=1");
      return;
    }
    for (std::string &p : pins) {
      if (p.size() >= val.size() && p.substr(0, val.size()) == val) {
        out.push_back(p);
      }
    }
    return;
  }
  const std::string_view pin = val.substr(0, eq);
  const std::string_view bit = val.substr(eq + 1);
  std::vector<std::string> pins;
  seq_collect_block_inputs(nl, pins);
  bool known = false;
  for (const std::string &p : pins) {
    if (p == pin) {
      known = true;
      break;
    }
  }
  if (!known) {
    return;
  }
  if (std::string_view("0").substr(0, bit.size()) == bit) {
    out.emplace_back(std::string(pin) + "=0");
  }
  if (std::string_view("1").substr(0, bit.size()) == bit) {
    out.emplace_back(std::string(pin) + "=1");
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
  const bool emit_ros = args.take_flag({ros2::cli_options::kEmitRos});
  const bool ros_grouped_flag = args.take_flag({ros2::cli_options::kRosGrouped});
  const bool ros_per_serial_flag = args.take_flag({ros2::cli_options::kRosPerSerial});
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
  cli::EmitOutputSampling ros_sampling;
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
      !take_stride({"--emit-uart-stride"}, "uart stride", uart_sampling) ||
      !take_stride({ros2::cli_options::kEmitRosStride}, "ros stride", ros_sampling)) {
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
      !take_select({"--emit-uart-select"}, "uart selection", uart_sampling) ||
      !take_select({ros2::cli_options::kEmitRosSelect}, "ros selection", ros_sampling)) {
    return core::kExitUsage;
  }
  // ROS identity (topic layout + delivery class): value options taken
  // here; the --emit-ros/--ros-grouped/--ros-per-serial flags above take
  // no values. Strings stay views into argv for the synchronous run.
  const auto o_ros_node = args.take_option({ros2::cli_options::kRosNode});
  const auto o_ros_ns = args.take_option({ros2::cli_options::kRosNs});
  const auto o_ros_signal_stem = args.take_option({ros2::cli_options::kRosSignalStem});
  const auto o_ros_particle_stem = args.take_option({ros2::cli_options::kRosParticleStem});
  const auto o_ros_qos = args.take_option({ros2::cli_options::kRosQos});
  if (auto o = args.take_option({"--solver"})) {
    solver_name = *o;
  }
  const auto o_seq_file = args.take_option({"--seq-file"});
  const auto o_seq_clocks = args.take_option({"--seq-clocks"});
  std::vector<std::string_view> o_seq_from;
  while (auto o = args.take_option({"--seq-from"})) {
    o_seq_from.push_back(*o);
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
  // log the signals table and publish signal channels; logic runs log the
  // sequential table and publish seq channels.
  const bool is_signal = sdef->name == solvers::SignalSolver::kName;
  const bool is_logic = sdef->name == solvers::SequentialSolver::kName;

  // Sequential options belong to the logic solver only (one command per
  // solver domain); the engine driver re-validates the populated section.
  if (!is_logic && (o_seq_file || o_seq_clocks || !o_seq_from.empty())) {
    return usage_error(out, "--seq-* options only apply to solver 'logic'");
  }

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
  if (ros_sampling.stride_given && !emit_ros) {
    return usage_error(out, "--emit-ros-stride needs --emit-ros");
  }
  if (ros_sampling.select_given && !emit_ros) {
    return usage_error(out, "--emit-ros-select needs --emit-ros");
  }
  // ROS identity without its output is a usage error, never silently
  // ignored (same orphan rule as every other per-output option).
  if ((o_ros_node || o_ros_ns || o_ros_signal_stem || o_ros_particle_stem || o_ros_qos || ros_grouped_flag || ros_per_serial_flag) && !emit_ros) {
    return usage_error(out, "--ros-* needs --emit-ros");
  }
  if (ros_grouped_flag && ros_per_serial_flag) {
    return usage_error(out, "--ros-grouped and --ros-per-serial are mutually exclusive");
  }
  // ROS shapes: node/namespace/stems are rmw path segments (no leading
  // digits, no slashes except inside ns/stems); QoS is the D-ROS-5 class.
  if (o_ros_node && !ros2::cli_options::is_valid_ros_node(*o_ros_node)) {
    return usage_error(out, "invalid ROS node name (want [A-Za-z_][A-Za-z0-9_]*)");
  }
  if (o_ros_ns && !ros2::cli_options::is_valid_ros_path(*o_ros_ns)) {
    return usage_error(out, "invalid ROS namespace (want a/b segments, no leading slash)");
  }
  if (o_ros_signal_stem && !ros2::cli_options::is_valid_ros_path(*o_ros_signal_stem)) {
    return usage_error(out, "invalid ROS signal stem (want a/b segments, no leading slash)");
  }
  if (o_ros_particle_stem && !ros2::cli_options::is_valid_ros_path(*o_ros_particle_stem)) {
    return usage_error(out, "invalid ROS particle stem (want a/b segments, no leading slash)");
  }
  // Parsed + validated here so usage errors (exit 2) are identical in
  // both builds; consumed only by the rmw transport below (ROS2=ON).
  [[maybe_unused]] ros2::QosClass ros_qos = ros2::QosClass::kLatestState;
  if (o_ros_qos) {
    const auto q = ros2::cli_options::parse_ros_qos(*o_ros_qos);
    if (!q.ok()) {
      return usage_error(out, "unknown ROS QoS (want latest|reliable)");
    }
    ros_qos = q.value;
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
  if (!is_signal && (csv_sampling.select_given || shm_sampling.select_given || tcp_sampling.select_given || udp_sampling.select_given ||
                     uart_sampling.select_given || ros_sampling.select_given)) {
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
    if (ros_sampling.select_given) {
      for (std::uint64_t id : ros_sampling.serials) {
        if (id != 0) {
          out << "Error: --emit-ros-select serial " << id << " out of range (sim signal runs one source, serial 0)\n";
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
    // ROS is a frame transport too (grouped SignalBlock/JointState honor
    // the same 64-channel over-bound gate in the publisher): an unfiltered
    // single-source run always fits; a select list must fit like the rest.
    if (emit_ros && ros_sampling.select_given && !cli::signal_channels_fit(ros_sampling.serials.size())) {
      out << "Error: --emit-ros-select needs " << (ros_sampling.serials.size() + 1) << " channels, transport holds " << link::kMaxChannels << '\n';
      out << kUsage << '\n';
      return core::kExitUsage;
    }
    if (!requested[1] && !requested[2] && !requested[3]) {
      // No transport: nothing to validate (single default source always fits).
    } else if (!csv_sampling.select_given && !shm_sampling.select_given &&
               !tcp_sampling.select_given && !udp_sampling.select_given) {
      // Unfiltered single-source signal run: 2 channels, always fits.
    }
  } else if (shm_name || tcp_endpoint || udp_endpoint || emit_ros) {
    if (n_particles > static_cast<long long>(link::kMaxItems)) {
      out << "Error: " << n_particles << " particles need " << n_particles
          << " items, transport holds " << link::kMaxItems << '\n';
      out << kUsage << '\n';
      return core::kExitUsage;
    }
  }

  // Headless run goes through the engine seam: the CLI builds a RunConfig
  // (plain data, no engine wiring here) and the driver owns the rest.
  // Signal and logic runs default to zero particles unless explicitly
  // requested (particles are orthogonal to both domains).
  if ((is_signal || is_logic) && !particles_given) {
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

  // Sequential section: the logic solver needs an explicit workspace (no
  // defaults run); any --seq-* with another solver was rejected above.
  if (is_logic) {
    if (!o_seq_file) {
      return usage_error(out, "logic run needs --seq-file <workspace.yaml>");
    }
    logic::SequentialRunConfig seq;
    std::string seq_err;
    if (!seq_build_section(*o_seq_file, o_seq_clocks, o_seq_from, seq, seq_err)) {
      return usage_error(out, seq_err);
    }
    cfg.seq = std::move(seq);
  }

  // Known transport capacity before running: logic frames carry seq.count
  // plus one channel per live flop (1 + N <= 64, same gate as signals);
  // CSV is unbounded. Counts the validated section the --seq-* options
  // populate (empty until then, always fits).
  if (is_logic && (shm_name || tcp_endpoint || udp_endpoint)) {
    std::size_t n_flops = 0;
    for (std::uint16_t slot = 0; slot < logic::kMaxGates; ++slot) {
      const logic::Gate &g = cfg.seq.netlist.gates[slot];
      if (g.live && logic::is_sequential_kind(g.kind)) {
        ++n_flops;
      }
    }
    if (!cli::signal_channels_fit(n_flops)) {
      out << "Error: logic run needs " << (n_flops + 1) << " channels, transport holds "
          << link::kMaxChannels << '\n';
      out << kUsage << '\n';
      return core::kExitUsage;
    }
  }

  // Signal runs log the signals table (D2b-A), logic runs the sequential
  // table; particle runs keep CsvSink.
  std::optional<emit::CsvSink> csv;
  std::optional<emit::SignalsCsvSink> sig_csv;
  std::optional<emit::LogicCsvSink> logic_csv;
  if (csv_path) {
    if (is_signal) {
      sig_csv.emplace(std::string{*csv_path});
      if (!sig_csv->ok()) {
        out << "Error: cannot open CSV file '" << *csv_path << "'\n";
        return core::kExitRuntime;
      }
    } else if (is_logic) {
      logic_csv.emplace(std::string{*csv_path});
      if (!logic_csv->ok()) {
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
  // readers pick the matching adapter; signal and logic solvers publish
  // through their own Frame adapters (same class, no subclassing).
  std::optional<link::ShmPublisher> shm;
  if (shm_name) {
    const link::FrameAdapter adapter = is_signal   ? &link::frame_from_signal_snapshot
                                       : is_logic ? &link::frame_from_logic_snapshot
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
    const link::FrameAdapter adapter = is_signal   ? &link::frame_from_signal_snapshot
                                       : is_logic ? &link::frame_from_logic_snapshot
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
    const link::FrameAdapter adapter = is_signal   ? &link::frame_from_signal_snapshot
                                       : is_logic ? &link::frame_from_logic_snapshot
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
  if (emit_ros) {
    if (!ros2::cli_options::ensure_ros_initialized()) {
      out << "Error: cannot initialize ROS node\n";
      return core::kExitRuntime;
    }
    try {
      // Root namespace (like examples/ros2_bridge.cpp): the bridge forms
      // absolute topics itself (`/<ns>/<stem>`, passed relative here), so
      // `ros2 topic list` shows them verbatim.
      ros_node = std::make_shared<rclcpp::Node>(std::string(o_ros_node ? *o_ros_node : "wengine"));
    } catch (...) {
      out << "Error: cannot initialize ROS node\n";
      return core::kExitRuntime;
    }
    ros2::BridgeConfig ros_cfg;
    ros2::cli_options::make_bridge_config(
        ros_cfg, o_ros_node ? *o_ros_node : "", o_ros_ns ? *o_ros_ns : "",
        o_ros_signal_stem ? *o_ros_signal_stem : "",
        o_ros_particle_stem ? *o_ros_particle_stem : "", ros_qos,
        !ros_per_serial_flag);
    ros_transport.emplace(*ros_node, ros_cfg);
    ros_pub.emplace(ros_cfg, *ros_transport);
    if (!ros_pub->ok()) {
      out << "Error: ROS publish failed (" << ros2::signals_block_topic(ros_cfg) << " unavailable)\n";
      return core::kExitRuntime;
    }
  }
#else
  if (emit_ros) {
    out << "Error: --emit-ros needs a ROS2 build (rebuild with -DBUILD_ROS2=ON)\n";
    return core::kExitRuntime;
  }
#endif

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
  std::optional<emit::StrideSink> logic_csv_stride;
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
  emit::ISink *sig_csv_out = nullptr;
  emit::ISink *logic_csv_out = nullptr;
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
  if (logic_csv) {
    logic_csv_out = &*logic_csv;
    if (csv_sampling.stride_given && csv_sampling.stride != 1) {
      logic_csv_stride.emplace(*logic_csv_out, csv_sampling.stride);
      logic_csv_out = &*logic_csv_stride;
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
#ifdef WENGINE_HAS_RCLCPP
  emit::ISink *ros_out = nullptr;
  if (ros_pub) {
    ros_out = &*ros_pub;
    if (is_signal && ros_sampling.select_given) {
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
  std::array<emit::ISink *, 8> sink_arr{};
  std::size_t n_sinks = 0;
  if (csv_out) {
    sink_arr[n_sinks++] = csv_out;
  }
  if (sig_csv_out) {
    sink_arr[n_sinks++] = sig_csv_out;
  }
  if (logic_csv_out) {
    sink_arr[n_sinks++] = logic_csv_out;
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
  for (std::size_t k = 0; k < n_sinks; ++k) {
    run.add_sink(*sink_arr[k]);
  }
  core::Status st = core::Status::success();
  solvers::paced::Stats paced_stats;
  const std::uint64_t total_steps = static_cast<std::uint64_t>(steps);
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
    out << "Error: simulation step failed at step " << s.step_index
        << " (partial step state is not a completed tick)\n";
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
  // Values for a preceding "--seq-clocks": clock-domain names from the
  // --seq-file workspace when available (nothing otherwise).
  if (!prior_args.empty() && prior_args.back() == "--seq-clocks") {
    logic::Netlist nl;
    if (seq_complete_workspace(prior_args, nl)) {
      for (const SeqDomain &d : seq_clock_domains(nl)) {
        if (d.name.size() >= partial.size() && d.name.substr(0, partial.size()) == partial) {
          out.emplace_back(d.name);
        }
      }
    }
    return;
  }
  // Values for a preceding "--seq-from": block-input pins -> PIN=0|1 forms.
  if (!prior_args.empty() && prior_args.back() == "--seq-from") {
    logic::Netlist nl;
    if (seq_complete_workspace(prior_args, nl)) {
      complete_seq_from_value(nl, partial, out);
    }
    return;
  }
  constexpr std::string_view kSeqClocksEq = "--seq-clocks=";
  if (partial.substr(0, kSeqClocksEq.size()) == kSeqClocksEq) {
    const std::string_view sub = partial.substr(kSeqClocksEq.size());
    logic::Netlist nl;
    if (seq_complete_workspace(prior_args, nl)) {
      for (const SeqDomain &d : seq_clock_domains(nl)) {
        if (d.name.size() >= sub.size() && d.name.substr(0, sub.size()) == sub) {
          out.emplace_back(std::string{kSeqClocksEq} + d.name);
        }
      }
    }
    return;
  }
  constexpr std::string_view kSeqFromEq = "--seq-from=";
  if (partial.substr(0, kSeqFromEq.size()) == kSeqFromEq) {
    const std::string_view sub = partial.substr(kSeqFromEq.size());
    logic::Netlist nl;
    if (seq_complete_workspace(prior_args, nl)) {
      std::vector<std::string> forms;
      complete_seq_from_value(nl, sub, forms);
      for (std::string &f : forms) {
        out.emplace_back(std::string{kSeqFromEq} + f);
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
