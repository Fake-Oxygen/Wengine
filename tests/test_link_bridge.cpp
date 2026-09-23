// Replay bridge: CSV parse fidelity + rejection, Frame<->Snapshot inverse,
// and the task acceptance — CSV -> bus -> reader output matches the
// original log, and a bus recording is byte-identical to the source.

#include "wengine/emit/csv_sink.hpp"
#include "wengine/link/frame.hpp"
#include "wengine/link/protocol.hpp"
#include "wengine/link/shm_publisher.hpp"
#include "wengine/link/shm_reader.hpp"

#include <array>
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace wengine;

namespace {

// Steps 1 (two particles, inexact binary), 2 (empty: no CSV rows by format),
// 5 (one particle, non-consecutive step to prove grouping by value).
std::vector<world::Snapshot> make_snaps() {
  std::vector<world::Snapshot> snaps;
  world::Snapshot a;
  a.step_index = 1;
  a.sim_time = 0.01;
  a.pos_x = {0.1 + 0.2, -4.0};
  a.pos_y = {1.5, 0.25};
  a.vel_x = {0.0, 2.5};
  a.vel_y = {-0.5, 0.0};
  snaps.push_back(a);
  world::Snapshot b;
  b.step_index = 2;
  b.sim_time = 0.02;
  snaps.push_back(b);
  world::Snapshot c;
  c.step_index = 5;
  c.sim_time = 0.05;
  c.pos_x = {3.375};
  c.pos_y = {-0.125};
  c.vel_x = {10.0};
  c.vel_y = {10.0};
  snaps.push_back(c);
  return snaps;
}

void write_log(const std::string &path,
               const std::vector<world::Snapshot> &snaps) {
  std::remove(path.c_str());
  emit::CsvSink csv(path);
  assert(csv.ok());
  for (const auto &s : snaps) {
    csv.on_snapshot(s);
  }
  assert(csv.flush().ok());
}

std::string read_file(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  assert(static_cast<bool>(in));
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void test_parse_roundtrip() {
  const std::string path = "/tmp/wengine_test_bridge_parse.csv";
  const auto snaps = make_snaps();
  write_log(path, snaps);

  std::ifstream in(path);
  assert(static_cast<bool>(in));
  std::vector<world::Snapshot> parsed;
  std::string error;
  assert(emit::parse_csv_log(in, parsed, error).ok());
  assert(error.empty());
  // The empty step-2 snapshot has no rows: parse yields the steps present.
  assert(parsed.size() == 2);
  assert(parsed[0] == snaps[0]); // bitwise, incl. 0.1 + 0.2
  assert(parsed[1] == snaps[2]);

  // Header-only log is valid and empty.
  std::istringstream empty("step,time,slot,x,y,vx,vy\n");
  std::vector<world::Snapshot> none;
  assert(emit::parse_csv_log(empty, none, error).ok() && none.empty());
  std::remove(path.c_str());
}

void test_parse_rejects() {
  const char *bad[] = {
      "", // missing header
      "step,time,slot,x,y,vx\n", // short header
      "step,time,slot,x,y,vx,vy\n1,0.01,0,1.0,2.0\n", // short row
      "step,time,slot,x,y,vx,vy\n1,0.01,0,1,2,3,4,5\n", // long row
      "step,time,slot,x,y,vx,vy\n1,abc,0,1,2,3,4\n", // non-numeric
      "step,time,slot,x,y,vx,vy\n-1,0.01,0,1,2,3,4\n", // negative step
      "step,time,slot,x,y,vx,vy\n+1,0.01,0,1,2,3,4\n", // plus-prefixed step
      "step,time,slot,x,y,vx,vy\n1,0.01,0,1.5x,2,3,4\n", // trailing garbage
      "step,time,slot,x,y,vx,vy\n1,0.01,0,1,2,3,4\n\n", // empty line
      "step,time,slot,x,y,vx,vy\n5,0.05,0,1,2,3,4\n3,0.03,0,1,2,3,4\n", // decreasing
      "step,time,slot,x,y,vx,vy\n1,0.01,0,1,2,3,4\n1,0.02,1,5,6,7,8\n", // time drift
      "step,time,slot,x,y,vx,vy\n1,0.01,0,1,2,3,4\n1,0.01,2,5,6,7,8\n", // slot gap
      "step,time,slot,x,y,vx,vy\n1,0.01,1,1,2,3,4\n1,0.01,0,5,6,7,8\n", // slot reorder
  };
  for (const char *text : bad) {
    std::istringstream in(text);
    std::vector<world::Snapshot> out;
    std::string error;
    assert(!emit::parse_csv_log(in, out, error).ok());
    assert(!error.empty() && out.empty()); // cleared on failure
  }
}

void test_snapshot_from_frame() {
  link::Frame f;
  f.step = 9;
  f.time = 0.09;
  link::FrameItem it;
  it.x = 1.0;
  it.y = 2.0;
  it.z = 3.0; // dropped: no CSV column
  it.vx = 4.0;
  it.vy = 5.0;
  it.vz = 6.0; // dropped: no CSV column
  f.items.push_back(it);
  f.channels.push_back({"node_A", 7.0}); // ignored: CSV is particle-shaped
  const world::Snapshot s = link::snapshot_from_frame(f);
  assert(s.step_index == 9 && s.sim_time == 0.09);
  assert(s.pos_x == std::vector<double>{1.0});
  assert(s.pos_y == std::vector<double>{2.0});
  assert(s.vel_x == std::vector<double>{4.0});
  assert(s.vel_y == std::vector<double>{5.0});

  // Inverse law within CSV bounds: back-and-forth is exact.
  const world::Snapshot src = make_snaps()[0];
  assert(link::snapshot_from_frame(link::frame_from_snapshot(src)) == src);
}

void test_csv_bus_csv_roundtrip() {
  // Task acceptance: CSV -> bus -> reader matches the source log, and a
  // bus recording of the playback is byte-identical to the source file.
  const std::string src_path = "/tmp/wengine_test_bridge_src.csv";
  const std::string rec_path = "/tmp/wengine_test_bridge_rec.csv";
  const auto snaps = make_snaps();
  write_log(src_path, snaps);

  std::ifstream in(src_path);
  assert(static_cast<bool>(in));
  std::vector<world::Snapshot> parsed;
  std::string error;
  assert(emit::parse_csv_log(in, parsed, error).ok());
  assert(parsed.size() == 2);

  const std::string seg =
      "/wengine-test-bridge-" + std::to_string(::getpid());
  link::ShmPublisher pub(seg, "projectile");
  assert(pub.ok());
  link::ShmReader reader(seg);
  assert(reader.ok());

  std::remove(rec_path.c_str());
  emit::CsvSink rec(rec_path);
  assert(rec.ok());
  for (std::size_t k = 0; k < parsed.size(); ++k) {
    pub.on_snapshot(parsed[k]);
    assert(pub.ok());
    // Sole writer, synchronous: latest is exactly what was just published.
    link::LinkFrame wire{};
    assert(reader.read_latest(wire));
    assert(wire.frame_seq == k + 1);
    link::Frame got;
    assert(link::decode_frame(wire, got).ok());
    assert(got == link::frame_from_snapshot(parsed[k]));
    rec.on_snapshot(link::snapshot_from_frame(got));
  }
  assert(rec.flush().ok());
  assert(read_file(rec_path) == read_file(src_path)); // byte-identical

  std::remove(src_path.c_str());
  std::remove(rec_path.c_str());
}

} // namespace

int main() {
  test_parse_roundtrip();
  test_parse_rejects();
  test_snapshot_from_frame();
  test_csv_bus_csv_roundtrip();
  std::cout << "test_link_bridge: OK\n";
  return 0;
}
