#include "wengine/cli/commands.hpp"
#include "wengine/cli/status.hpp"
#include "wengine/core/run_config.hpp"
#include "wengine/emit/csv_sink.hpp"
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

constexpr std::array<std::string_view, 16> kSimOptions{{
    "--steps", "-n", "--dt", "--solver", "--gravity", "-g", "--particles", "-p", "--x0",
    "--y0", "--vx0", "--vy0", "--emit-csv", "--emit-every", "--help", "-h",
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
  if (!take_i64({"--particles", "-p"}, "particle count", n_particles)) {
    return core::kExitUsage;
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

  // Headless run goes through the engine seam: the CLI builds a RunConfig
  // (plain data, no engine wiring here) and the driver owns the rest.
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

  std::optional<emit::CsvSink> csv;
  if (csv_path) {
    csv.emplace(std::string{*csv_path});
    if (!csv->ok()) {
      out << "Error: cannot open CSV file '" << *csv_path << "'\n";
      return core::kExitRuntime;
    }
  }

  world::State s;
  core::Status st;
  if (csv) {
    std::array<emit::ISink *, 1> sinks{&*csv};
    st = solvers::run_headless(cfg, sinks, s);
  } else {
    st = solvers::run_headless(cfg, std::span<emit::ISink *>{}, s);
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
  if (csv) {
    if (!csv->flush().ok()) {
      out << "Error: failed writing CSV file '" << csv->path() << "'\n";
      return core::kExitRuntime;
    }
    out << "csv: " << csv->path() << " (" << csv->rows_written() << " rows)\n";
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
