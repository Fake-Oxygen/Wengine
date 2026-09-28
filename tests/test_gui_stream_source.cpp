// GUI external sources: SHM subscribe parity + lag isolation, CSV replay
// transport/pacing, and the three-level parity check (CLI text vs replayed
// frame vs serialized CSV artifact). Engine-only header; no ImGui linked.

#include "controllers/external_source.hpp"
#include "wengine/core/run_config.hpp"
#include "wengine/emit/csv_sink.hpp"
#include "wengine/emit/signal_sink.hpp"
#include "wengine/link/frame.hpp"
#include "wengine/link/shm_publisher.hpp"
#include "wengine/solvers/run.hpp"

#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <new>
#include <sstream>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace wengine;

namespace {

constexpr bool near(double a, double b, double eps = 1e-9) {
  return std::fabs(a - b) <= eps * (1.0 + std::fabs(b));
}

std::string seg_name(const char *tag) {
  return "/wengine-test-guisrc-" + std::string(tag) + "-" + std::to_string(::getpid());
}

core::RunConfig batch_cfg(std::uint64_t steps) {
  core::RunConfig cfg;
  cfg.loop.dt = 0.01;
  cfg.loop.emit_every = 1;
  cfg.steps = steps;
  cfg.solver = "projectile";
  cfg.gravity = 9.81;
  cfg.particles = 1;
  return cfg;
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

void test_shm_subscribe_parity() {
  // Displayed frames match engine truth exactly (==, not near).
  const std::string name = seg_name("parity");
  link::ShmPublisher pub(name, "projectile");
  assert(pub.ok());
  world::State final_state;
  std::array<emit::ISink *, 1> sinks{&pub};
  assert(solvers::run_headless(batch_cfg(3), sinks, final_state).ok());

  gui::ShmSource src;
  src.set_segment(name);
  assert(src.ensure_attached().ok() && src.attached() && src.ok());
  assert(src.poll());
  assert(src.has_frame());
  assert(src.frame() == link::frame_from_snapshot(world::take_snapshot(final_state)));
  assert(!src.poll()); // nothing newer: no duplicate delivery
  // Pre-attach frames count as missed (same display as link_sample).
  assert(src.lag().received == 1 && src.lag().missed == 2);

  world::Snapshot extra;
  extra.step_index = 99;
  extra.sim_time = 0.99;
  extra.pos_x = {7.0};
  extra.pos_y = {8.0};
  extra.vel_x = {9.0};
  extra.vel_y = {10.0};
  pub.on_snapshot(extra);
  assert(src.poll());
  assert(src.frame() == link::frame_from_snapshot(extra));
  assert(src.lag().received == 2 && src.lag().missed == 2);
}

void test_shm_wait_and_refuse() {
  gui::ShmSource src;
  // Missing publisher: pending, not failure — UI shows "waiting".
  src.set_segment(seg_name("absent"));
  assert(src.ensure_attached().ok());
  assert(!src.attached() && src.ok() && src.error().empty());
  assert(!src.poll() && !src.has_frame());

  // Bad name: loud usage-class failure.
  src.set_segment("no-leading-slash");
  assert(!src.ensure_attached().ok());
  assert(!src.attached() && !src.ok());
  assert(src.error().find("invalid segment name") != std::string::npos);

  // Refused segment fails fast with the versioned message (crafted future
  // version: this reader supports kVersion).
  const std::string name = seg_name("refused");
  ::shm_unlink(name.c_str());
  const int fd = ::shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
  assert(fd >= 0);
  const std::size_t size =
      sizeof(link::SegmentHeader) + link::kSlotCount * sizeof(link::Slot);
  assert(::ftruncate(fd, static_cast<off_t>(size)) == 0);
  void *base = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  assert(base != MAP_FAILED);
  auto *header = new (base) link::SegmentHeader();
  const char *domain = "projectile";
  for (std::size_t i = 0; domain[i] != '\0'; ++i) {
    header->domain[i] = domain[i];
  }
  header->max_items = static_cast<std::uint64_t>(link::kMaxItems);
  header->max_channels = static_cast<std::uint64_t>(link::kMaxChannels);
  header->slot_count = static_cast<std::uint64_t>(link::kSlotCount);
  header->version.store(link::kVersion + 1, std::memory_order_release);
  src.set_segment(name);
  assert(!src.ensure_attached().ok());
  assert(!src.attached());
  assert(src.error().find("has version " + std::to_string(link::kVersion + 1)) !=
         std::string::npos);
  assert(src.error().find("supports version " + std::to_string(link::kVersion)) !=
         std::string::npos);
  ::munmap(base, size);
  ::close(fd);
  ::shm_unlink(name.c_str());
}

void test_shm_lag_isolation() {
  // Lag lives entirely on the display side: 12 frames into an 8-slot ring
  // with no polling, then one poll takes the latest exactly while the lag
  // counters account for the skipped span. No Runner, no sim state here —
  // by construction there is nothing to perturb.
  const std::string name = seg_name("lag");
  link::ShmPublisher pub(name, "projectile");
  assert(pub.ok());
  world::State final_state;
  std::array<emit::ISink *, 1> sinks{&pub};
  assert(solvers::run_headless(batch_cfg(12), sinks, final_state).ok());
  assert(pub.stats().published == 12);

  gui::ShmSource src;
  src.set_segment(name);
  assert(src.poll());
  assert(src.frame() == link::frame_from_snapshot(world::take_snapshot(final_state)));
  assert(src.lag().received == 1 && src.lag().missed == 11);
  assert(src.lag().max_gap == 11);
  assert(!src.poll());
}

std::vector<world::Snapshot> make_log_snaps() {
  std::vector<world::Snapshot> snaps;
  world::Snapshot a;
  a.step_index = 2;
  a.sim_time = 0.02;
  a.pos_x = {0.1 + 0.2, -4.0};
  a.pos_y = {1.5, 0.25};
  a.vel_x = {0.0, 2.5};
  a.vel_y = {-0.5, 0.0};
  snaps.push_back(a);
  world::Snapshot b;
  b.step_index = 4;
  b.sim_time = 0.04;
  b.pos_x = {3.375};
  b.pos_y = {-0.125};
  b.vel_x = {10.0};
  b.vel_y = {10.0};
  snaps.push_back(b);
  return snaps;
}

void test_shm_switch_segment() {
  // Subscribing to a different segment mid-session works with no restart:
  // target switch drops the old attach, cursor, and lag.
  const std::string name_a = seg_name("switch-a");
  const std::string name_b = seg_name("switch-b");
  link::ShmPublisher pub_a(name_a, "projectile");
  link::ShmPublisher pub_b(name_b, "projectile");
  assert(pub_a.ok() && pub_b.ok());
  world::State unused;
  std::array<emit::ISink *, 1> sinks_a{&pub_a};
  std::array<emit::ISink *, 1> sinks_b{&pub_b};
  assert(solvers::run_headless(batch_cfg(3), sinks_a, unused).ok());
  assert(solvers::run_headless(batch_cfg(7), sinks_b, unused).ok());

  gui::ShmSource src;
  src.set_segment(name_a);
  assert(src.poll());
  assert(src.frame().step == 3);
  assert(src.lag().received == 1);

  src.set_segment(name_b);
  assert(!src.has_frame()); // switched context, nothing shown yet
  assert(src.poll());
  assert(src.frame().step == 7); // the other run's latest, exactly
  assert(src.lag().received == 1 && src.lag().missed == 6);
}

void test_shm_same_name_republish_needs_reconnect() {
  // POSIX fd semantics: after the publisher exits and recreates the same
  // name, the old mapping shows frozen frames and the cursor never
  // advances past them. reconnect() is the supported recovery.
  const std::string name = seg_name("repub");
  world::State unused;
  gui::ShmSource src;
  {
    // First headless run; the publisher dies at scope end like a real
    // exiting process (segment unlinked with it).
    link::ShmPublisher pub(name, "projectile");
    assert(pub.ok());
    std::array<emit::ISink *, 1> sinks{&pub};
    assert(solvers::run_headless(batch_cfg(2), sinks, unused).ok());
    src.set_segment(name);
    assert(src.poll());
    assert(src.frame().step == 2);
  }
  {
    // Second run under the same name.
    link::ShmPublisher pub(name, "projectile");
    assert(pub.ok());
    std::array<emit::ISink *, 1> sinks{&pub};
    assert(solvers::run_headless(batch_cfg(5), sinks, unused).ok());

    // Stale mapping: nothing newer than cursor 2 on the old object.
    assert(!src.poll());
    assert(src.frame().step == 2);

    src.reconnect();
    assert(!src.has_frame());
    assert(src.poll());
    assert(src.frame().step == 5);
    assert(src.lag().received == 1);
  }
}

void test_csv_load_and_step() {
  const std::string path = "/tmp/wengine_test_guisrc.csv";
  const auto snaps = make_log_snaps();
  write_log(path, snaps);

  gui::CsvReplay replay;
  assert(!replay.has_frame() && replay.count() == 0);
  std::ifstream in(path);
  assert(static_cast<bool>(in));
  assert(replay.load(in).ok() && replay.ok());
  assert(replay.count() == 2 && replay.cursor() == 0);
  assert(replay.has_frame());
  assert(replay.frame() == link::frame_from_snapshot(snaps[0]));

  replay.step_once();
  assert(replay.cursor() == 1);
  assert(replay.frame() == link::frame_from_snapshot(snaps[1]));
  replay.step_once(); // parked at the end: holds final snapshot
  assert(replay.cursor() == 1);
  assert(replay.frame() == link::frame_from_snapshot(snaps[1]));
  replay.reset();
  assert(replay.cursor() == 0);
  assert(replay.frame() == link::frame_from_snapshot(snaps[0]));
  std::remove(path.c_str());
}

void test_csv_play_pacing() {
  // Log times 0.0, 0.1, 0.3: deterministic wall-folding at rate 1 and 2.
  std::vector<world::Snapshot> snaps;
  const double times[3] = {0.0, 0.1, 0.3};
  const std::uint64_t steps[3] = {0, 1, 3};
  for (int k = 0; k < 3; ++k) {
    world::Snapshot s;
    s.step_index = steps[k];
    s.sim_time = times[k];
    s.pos_x = {times[k]};
    s.pos_y = {0.0};
    s.vel_x = {0.0};
    s.vel_y = {0.0};
    snaps.push_back(s);
  }
  const std::string path = "/tmp/wengine_test_guisrc_pace.csv";
  write_log(path, snaps);

  gui::CsvReplay replay;
  std::ifstream in(path);
  assert(replay.load(in).ok());
  assert(replay.rate() == 1.0);
  replay.set_rate(0.0); // ignored: pacing stays positive
  replay.set_rate(-2.0);
  assert(replay.rate() == 1.0);

  replay.set_playing(true);
  replay.advance_frame(0.05);
  assert(replay.cursor() == 0 && replay.playing());
  replay.advance_frame(0.05); // banked 0.1 >= delta 0.1
  assert(replay.cursor() == 1 && replay.playing());
  assert(near(replay.frame().time, 0.1));
  replay.advance_frame(0.1); // need 0.2 more
  assert(replay.cursor() == 1 && replay.playing());
  replay.advance_frame(0.1);
  assert(replay.cursor() == 2 && !replay.playing()); // parked at end
  assert(near(replay.frame().time, 0.3));

  // Double rate halves the wall needed; reset replays from the top.
  replay.set_rate(2.0);
  assert(replay.rate() == 2.0);
  replay.reset();
  replay.set_playing(true);
  replay.advance_frame(0.05); // delta 0.1 / 2 = 0.05: exact step
  assert(replay.cursor() == 1);
  replay.advance_frame(0.1); // banks past delta (0.3 - 0.1) / 2
  assert(replay.cursor() == 2 && !replay.playing());
  std::remove(path.c_str());
}

void test_csv_parity_exact() {
  // Three-level parity: CSV text field == replayed frame value == engine
  // snapshot value, exactly (max_digits10 round-trip where applicable).
  const std::string path = "/tmp/wengine_test_guisrc_parity.csv";
  const auto snaps = make_log_snaps();
  write_log(path, snaps);

  gui::CsvReplay replay;
  std::ifstream in(path);
  assert(replay.load(in).ok());
  assert(replay.frame().items[0].x == 0.1 + 0.2);
  assert(replay.frame().items[0].x == snaps[0].pos_x[0]);

  std::ifstream text(path);
  std::string header;
  assert(static_cast<bool>(std::getline(text, header)));
  std::string row;
  assert(static_cast<bool>(std::getline(text, row)));
  const std::size_t c3 = row.find(',', row.find(',', row.find(',') + 1) + 1);
  const double file_x = std::stod(row.substr(c3 + 1, row.find(',', c3 + 1) - c3 - 1));
  assert(file_x == replay.frame().items[0].x);
  std::remove(path.c_str());
}

void test_csv_load_file() {
  gui::CsvReplay replay;
  assert(!replay.load_file("/wengine-no-such-dir-xyz/missing.csv").ok());
  assert(!replay.error().empty() && replay.count() == 0);

  const std::string path = "/tmp/wengine_test_guisrc_file.csv";
  write_log(path, make_log_snaps());
  assert(replay.load_file(path).ok());
  assert(replay.count() == 2 && replay.has_frame());
  assert(replay.frame() == link::frame_from_snapshot(make_log_snaps()[0]));
  std::remove(path.c_str());
}

void test_csv_load_rejects() {
  gui::CsvReplay replay;
  std::istringstream bad("step,time,slot,x,y,vx,vy\n1,0.01,0,1,2,3\n");
  assert(!replay.load(bad).ok());
  assert(!replay.ok() && !replay.error().empty());
  assert(replay.count() == 0 && !replay.has_frame());
}

void test_csv_signal_replay() {
  // Signal CSV logs replay through the signal adapter: header sniffing
  // selects parser + adapter, and replayed frames equal the live adapter
  // output exactly (==, not near).
  core::RunConfig cfg;
  cfg.loop.dt = 0.01;
  cfg.loop.emit_every = 1;
  cfg.steps = 4;
  cfg.solver = "signal";
  cfg.particles = 0;
  cfg.signal.params.kind = signal::SignalKind::kSine;
  cfg.signal.params.freq_hz = 5.0;
  cfg.signal.set = true;
  const std::string path = "/tmp/wengine_test_guisrc_sig.csv";
  std::remove(path.c_str());
  world::State final_state;
  {
    emit::SignalsCsvSink sig_csv(path);
    assert(sig_csv.ok());
    std::array<emit::ISink *, 1> sinks{&sig_csv};
    assert(solvers::run_headless(cfg, sinks, final_state).ok());
    assert(sig_csv.flush().ok());
    assert(sig_csv.rows_written() == 4); // one source x four emissions
  }
  std::vector<world::Snapshot> file_snaps;
  {
    std::ifstream in(path);
    std::string error;
    assert(emit::parse_signals_log(in, file_snaps, error).ok());
    assert(file_snaps.size() == 4);
  }
  gui::CsvReplay replay;
  assert(replay.load_file(path).ok());
  assert(replay.count() == 4 && replay.has_frame());
  for (std::size_t k = 0; k < file_snaps.size(); ++k) {
    const link::Frame want = link::frame_from_signal_snapshot(file_snaps[k]);
    if (k > 0) {
      replay.step_once();
    }
    assert(replay.frame() == want);
  }
  assert(replay.frame().channels.size() == 2); // sig.count + sig.0.U
  assert(replay.frame().channels[1].name == "sig.0.U");
  assert(replay.frame().channels[1].value == file_snaps.back().sig_value[0]);
  // Switching back to a particle log re-sniffs the adapter.
  write_log(path, make_log_snaps());
  assert(replay.load_file(path).ok());
  assert(replay.frame() == link::frame_from_snapshot(make_log_snaps()[0]));
  std::remove(path.c_str());
}

} // namespace

int main() {
  test_shm_subscribe_parity();
  test_shm_wait_and_refuse();
  test_shm_lag_isolation();
  test_shm_switch_segment();
  test_shm_same_name_republish_needs_reconnect();
  test_csv_load_and_step();
  test_csv_play_pacing();
  test_csv_parity_exact();
  test_csv_load_file();
  test_csv_load_rejects();
  test_csv_signal_replay();
  std::cout << "test_gui_stream_source: OK\n";
  return 0;
}
