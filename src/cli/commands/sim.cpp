#include "wengine/cli/commands.hpp"
#include "wengine/core/runner.hpp"
#include "wengine/emit/csv_sink.hpp"
#include "wengine/solvers/projectile.hpp"

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
  out << "Error: " << msg << '\n';
  out << SimSpec::kUsage << '\n';
  return 2;
}

int unexpected_error(std::ostream &out, std::string_view tok) {
  out << "Error: unexpected argument '" << tok << "'\n";
  out << SimSpec::kUsage << '\n';
  return 2;
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

constexpr std::array<std::string_view, 15> kSimOptions{{
    "--steps", "-n", "--dt", "--gravity", "-g", "--particles", "-p", "--x0", "--y0",
    "--vx0", "--vy0", "--emit-csv", "--emit-every", "--help", "-h",
}};

} // namespace

int SimSpec::run(ArgCursor &args, std::ostream &out) {
  if (args.take_flag({"--help", "-h"})) {
    out << kName << " - " << kDescription << "\nUsage:\n" << kUsage << '\n';
    return 0;
  }

  long long steps = 100;
  double dt = 0.01;
  double gravity = 9.81;
  long long n_particles = 1;
  double x0 = 0.0, y0 = 0.0, vx0 = 10.0, vy0 = 10.0;
  long long emit_every = 1;
  std::optional<std::string_view> csv_path;

  auto take_i64 = [&](std::initializer_list<std::string_view> names, const char *what,
                      long long &slot) -> bool {
    if (auto o = args.take_option(names)) {
      auto p = parse_i64(*o);
      if (!p.ok) {
        out << "Error: invalid " << what << " '" << *o << "'\n";
        out << kUsage << '\n';
        return false;
      }
      slot = p.value;
    }
    return true;
  };
  auto take_num = [&](std::initializer_list<std::string_view> names, const char *what,
                      double &slot, ParseResult<double> (*parse)(std::string_view)) -> bool {
    if (auto o = args.take_option(names)) {
      auto p = parse(*o);
      if (!p.ok) {
        out << "Error: invalid " << what << " '" << *o << "'\n";
        out << kUsage << '\n';
        return false;
      }
      slot = p.value;
    }
    return true;
  };

  if (!take_i64({"--steps", "-n"}, "step count", steps)) {
    return 2;
  }
  if (!take_num({"--dt"}, "time step", dt, parse_time)) {
    return 2;
  }
  if (!take_num({"--gravity", "-g"}, "gravity", gravity, parse_double)) {
    return 2;
  }
  if (!take_i64({"--particles", "-p"}, "particle count", n_particles)) {
    return 2;
  }
  if (!take_num({"--x0"}, "x0", x0, parse_double)) {
    return 2;
  }
  if (!take_num({"--y0"}, "y0", y0, parse_double)) {
    return 2;
  }
  if (!take_num({"--vx0"}, "vx0", vx0, parse_double)) {
    return 2;
  }
  if (!take_num({"--vy0"}, "vy0", vy0, parse_double)) {
    return 2;
  }
  if (!take_i64({"--emit-every"}, "emit stride", emit_every)) {
    return 2;
  }
  csv_path = args.take_option({"--emit-csv"});

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
    out << "Error: unknown option '" << *args.first_remaining() << "'\n";
    out << kUsage << '\n';
    return 2;
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

  core::Runner runner{{.dt = dt,
                       .emit_every = static_cast<std::uint64_t>(emit_every)}};
  solvers::ProjectileSolver solver(gravity);
  runner.add_solver(solver);
  for (long long k = 0; k < n_particles; ++k) {
    world::Entity e = runner.state().spawn_entity();
    runner.state().particles.attach(e.index);
    runner.state().particles.pos_x[e.index] = x0;
    runner.state().particles.pos_y[e.index] = y0;
    runner.state().particles.vel_x[e.index] = vx0;
    runner.state().particles.vel_y[e.index] = vy0;
  }

  std::optional<emit::CsvSink> csv;
  if (csv_path) {
    csv.emplace(std::string{*csv_path});
    if (!csv->ok()) {
      out << "Error: cannot open CSV file '" << *csv_path << "'\n";
      return 1;
    }
    runner.add_sink(*csv);
  }

  const core::Status st =
      runner.run(static_cast<std::uint64_t>(steps));
  if (!st.ok()) {
    out << "Error: simulation step failed\n";
    return 1;
  }

  const world::State &s = runner.state();
  out << "sim run: solver=" << solver.name() << " g=" << solver.gravity() << '\n';
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
    csv->flush();
    out << "csv: " << csv->path() << " (" << csv->rows_written() << " rows)\n";
  }
  return 0;
}

void SimSpec::complete(std::span<const std::string_view> prior_args, std::string_view partial,
                       std::vector<std::string> &out) {
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
