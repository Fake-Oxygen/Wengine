#include "wengine/core/runner.hpp"
#include "wengine/emit/console_sink.hpp"
#include "wengine/emit/csv_sink.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace wengine;

namespace {

constexpr bool near(double a, double b, double eps = 1e-12) {
  return std::fabs(a - b) <= eps * (1.0 + std::fabs(b));
}

// Spawns one particle per step, stamped with its slot index.
struct Spawner : core::ISolver {
  [[nodiscard]] std::string_view name() const noexcept override { return "spawn"; }

  core::Status step(world::State &s, double) noexcept override {
    const world::Entity e = s.spawn_entity();
    s.particles.attach(e.index);
    s.particles.pos_x[e.index] = static_cast<double>(e.index);
    s.particles.pos_y[e.index] = -static_cast<double>(e.index);
    return core::Status::success();
  }
};

std::vector<std::string> read_lines(const std::string &path) {
  std::vector<std::string> lines;
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    lines.push_back(line);
  }
  return lines;
}

std::vector<std::string> split_csv(const std::string &line) {
  std::vector<std::string> fields;
  std::string cur;
  std::istringstream ss(line);
  while (std::getline(ss, cur, ',')) {
    fields.push_back(cur);
  }
  return fields;
}

void test_console_format() {
  const world::Snapshot snap{
      .sim_time = 0.03, .step_index = 3, .pos_x = {1.0, 2.0}, .pos_y = {0.0, 0.0},
      .vel_x = {0.0, 0.0}, .vel_y = {0.0, 0.0}};
  std::ostringstream out;
  emit::ConsoleSink sink(out);
  sink.on_snapshot(snap);
  assert(out.str() == "step 3 t 0.03s particles 2\n");
}

void test_csv_content() {
  const std::string path = "/tmp/wengine_test_emit.csv";
  std::remove(path.c_str());

  world::State s;
  const world::Entity a = s.spawn_entity();
  const world::Entity b = s.spawn_entity();
  s.particles.attach(a.index);
  s.particles.pos_x[a.index] = 0.1 + 0.2; // inexact binary: tests replay precision
  s.particles.vel_y[a.index] = 1.5;
  s.particles.attach(b.index);
  s.particles.pos_x[b.index] = -4.0;
  s.sim_time = 0.07;
  s.step_index = 7;

  emit::CsvSink sink(path);
  assert(sink.ok());
  sink.on_snapshot(world::take_snapshot(s));
  assert(sink.rows_written() == 2);
  sink.flush();

  const auto lines = read_lines(path);
  assert(lines.size() == 3);
  assert(lines[0] == "step,time,slot,entity_index,entity_generation,x,y,vx,vy");
  const auto r0 = split_csv(lines[1]);
  const auto r1 = split_csv(lines[2]);
  assert(r0.size() == 9 && r1.size() == 9);
  assert(std::stoull(r0[0]) == 7 && std::stoull(r0[2]) == 0);
  assert(std::stoull(r0[3]) == 0 && std::stoull(r0[4]) == 0); // stable identity
  assert(std::stod(r0[1]) == 0.07); // exact round-trip via max_digits10
  assert(std::stod(r0[5]) == 0.1 + 0.2);
  assert(std::stod(r0[8]) == 1.5);
  assert(std::stoull(r1[2]) == 1 && std::stod(r1[5]) == -4.0);
  assert(std::stoull(r1[3]) == 1 && std::stoull(r1[4]) == 0);
  std::remove(path.c_str());
}

void test_csv_empty_and_failure() {
  const std::string path = "/tmp/wengine_test_emit_empty.csv";
  std::remove(path.c_str());
  {
    emit::CsvSink sink(path);
    assert(sink.ok());
    sink.on_snapshot(world::Snapshot{}); // no particles: header only
    assert(sink.rows_written() == 0);
  }
  const auto lines = read_lines(path);
  assert(lines.size() == 1 && lines[0] == "step,time,slot,entity_index,entity_generation,x,y,vx,vy");
  std::remove(path.c_str());

  // Unwritable path: sticky failure, never throws.
  emit::CsvSink bad("/tmp/wengine_no_such_dir_xyz/out.csv");
  assert(!bad.ok());
  bad.on_snapshot(world::Snapshot{});
  assert(!bad.ok() && bad.rows_written() == 0);
}

void test_end_to_end_through_runner() {
  // Headless-style drive: runner -> console + file sinks, then verify the
  // file against the snapshots (State -> Snapshot -> ISink proven).
  const std::string path = "/tmp/wengine_test_emit_e2e.csv";
  std::remove(path.c_str());

  core::Runner runner{{.dt = 0.01, .emit_every = 1}};
  Spawner spawner;
  std::ostringstream console;
  emit::ConsoleSink cout_sink(console);
  emit::CsvSink file_sink(path);
  runner.add_solver(spawner);
  runner.add_sink(cout_sink);
  runner.add_sink(file_sink);
  assert(runner.run(4).ok());
  assert(file_sink.ok() && file_sink.rows_written() == 1 + 2 + 3 + 4);
  file_sink.flush();

  // Console got one line per snapshot.
  std::size_t console_lines = 0;
  {
    std::istringstream ss(console.str());
    std::string line;
    while (std::getline(ss, line)) {
      ++console_lines;
      assert(line.rfind("step ", 0) == 0);
    }
  }
  assert(console_lines == 4);

  // File: header + 10 rows; step k carries k particles in slot order.
  const auto lines = read_lines(path);
  assert(lines.size() == 11 && lines[0] == "step,time,slot,entity_index,entity_generation,x,y,vx,vy");
  std::size_t row = 1;
  for (std::uint64_t step = 1; step <= 4; ++step) {
    for (std::uint64_t slot = 0; slot < step; ++slot, ++row) {
      const auto f = split_csv(lines[row]);
      assert(f.size() == 9);
      assert(std::stoull(f[0]) == step);
      assert(near(std::stod(f[1]), 0.01 * step));
      assert(std::stoull(f[2]) == slot);
      assert(near(std::stod(f[5]), static_cast<double>(slot)));
      assert(near(std::stod(f[6]), -static_cast<double>(slot)));
      assert(std::stoull(f[3]) == slot && std::stoull(f[4]) == 0); // identity = slot, gen 0
    }
  }
  std::remove(path.c_str());
}

} // namespace

int main() {
  test_console_format();
  test_csv_content();
  test_csv_empty_and_failure();
  test_end_to_end_through_runner();
  std::cout << "test_emit: OK\n";
  return 0;
}
