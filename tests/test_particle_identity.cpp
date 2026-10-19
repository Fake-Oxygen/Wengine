#include "wengine/emit/csv_sink.hpp"
#include "wengine/link/compact_codec.hpp"
#include "wengine/link/frame.hpp"
#include "wengine/link/protocol.hpp"
#include "wengine/world/serialize.hpp"
#include "wengine/world/state.hpp"

#include <array>
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

using namespace wengine;

namespace {

world::State make_three() {
  world::State s;
  const world::Entity a = s.spawn_entity();
  const world::Entity b = s.spawn_entity();
  const world::Entity c = s.spawn_entity();
  s.particles.attach(a.index);
  s.particles.attach(b.index);
  s.particles.attach(c.index);
  s.particles.pos_x[a.index] = 1.0;
  s.particles.pos_y[a.index] = 2.0;
  s.particles.vel_x[a.index] = 3.0;
  s.particles.vel_y[a.index] = 4.0;
  s.particles.pos_x[b.index] = 5.0;
  s.particles.pos_y[b.index] = 6.0;
  s.particles.vel_x[b.index] = 7.0;
  s.particles.vel_y[b.index] = 8.0;
  s.particles.pos_x[c.index] = 9.0;
  s.particles.pos_y[c.index] = 10.0;
  s.particles.vel_x[c.index] = 11.0;
  s.particles.vel_y[c.index] = 12.0;
  s.sim_time = 0.03;
  s.step_index = 3;
  assert(a.index == 0 && b.index == 1 && c.index == 2);
  assert(a.generation == 0 && b.generation == 0 && c.generation == 0);
  return s;
}

void test_snapshot_carries_identity() {
  const world::State s = make_three();
  const world::Snapshot snap = world::take_snapshot(s);
  assert(snap.particle_count() == 3);
  assert(snap.has_particle_identity());
  assert(snap.particle_index[0] == 0 && snap.particle_generation[0] == 0);
  assert(snap.particle_index[1] == 1 && snap.particle_generation[1] == 0);
  assert(snap.particle_index[2] == 2 && snap.particle_generation[2] == 0);
  // Numeric values in dense slot order.
  assert(snap.pos_x[0] == 1.0 && snap.pos_x[1] == 5.0 && snap.pos_x[2] == 9.0);
}

void test_frame_and_inverse_preserve_ids() {
  const world::Snapshot snap = world::take_snapshot(make_three());
  const link::Frame f = link::frame_from_snapshot(snap);
  assert(f.items.size() == 3);
  for (std::size_t k = 0; k < 3; ++k) {
    assert(f.items[k].has_identity);
    assert(f.items[k].entity_index == snap.particle_index[k]);
    assert(f.items[k].entity_generation == snap.particle_generation[k]);
  }
  assert(f.items[0].x == 1.0 && f.items[1].x == 5.0 && f.items[2].x == 9.0);
  const world::Snapshot back = link::snapshot_from_frame(f);
  assert(back == snap); // exact, IDs + values + order
}

void test_despawn_keeps_survivor_identity() {
  world::State s = make_three();
  const world::Entity b{1, 0};
  assert(s.despawn_entity(b));
  const world::Snapshot snap = world::take_snapshot(s);
  assert(snap.particle_count() == 2);
  assert(snap.has_particle_identity());
  // Dense position 1 now holds old slot 2, but identity stays (2,0):
  // distinct objects are never confused by shifted positions.
  assert(snap.particle_index[0] == 0 && snap.particle_generation[0] == 0);
  assert(snap.particle_index[1] == 2 && snap.particle_generation[1] == 0);
  assert(snap.pos_x[0] == 1.0 && snap.pos_x[1] == 9.0);
  const link::Frame f = link::frame_from_snapshot(snap);
  assert(f.items.size() == 2);
  assert(f.items[0].entity_index == 0 && f.items[1].entity_index == 2);
  assert(link::snapshot_from_frame(f) == snap);
}

void test_slot_reuse_has_different_identity() {
  world::State s = make_three();
  assert(s.despawn_entity(world::Entity{1, 0}));
  const world::Entity d = s.spawn_entity(); // recycles slot 1, bumps generation
  assert(d.index == 1 && d.generation == 1);
  s.particles.attach(d.index);
  s.particles.pos_x[d.index] = 50.0;
  s.particles.pos_y[d.index] = 60.0;
  const world::Snapshot snap = world::take_snapshot(s);
  assert(snap.particle_count() == 3);
  assert(snap.particle_index[0] == 0 && snap.particle_generation[0] == 0);
  assert(snap.particle_index[1] == 1 && snap.particle_generation[1] == 1);
  assert(snap.particle_index[2] == 2 && snap.particle_generation[2] == 0);
  // Recycled slot (1,1) differs from the despawned (1,0): never aliases.
  assert(!(snap.particle_index[1] == 1 && snap.particle_generation[1] == 0));
  const link::Frame f = link::frame_from_snapshot(snap);
  assert(f.items[1].has_identity && f.items[1].entity_generation == 1);
  assert(link::snapshot_from_frame(f) == snap);
}

void test_checkpoint_restore_preserves_ids() {
  world::State s = make_three();
  assert(s.despawn_entity(world::Entity{1, 0}));
  const world::Entity d = s.spawn_entity();
  s.particles.attach(d.index);
  s.particles.pos_x[d.index] = 50.0;
  const world::Snapshot before = world::take_snapshot(s);
  std::ostringstream bytes;
  assert(world::save_state(s, bytes).ok());
  world::State dst;
  std::istringstream in(bytes.str());
  assert(world::load_state(dst, in).ok());
  assert(dst == s);
  const world::Snapshot after = world::take_snapshot(dst);
  assert(after == before);
  assert(after.has_particle_identity());
  // Freelist order survived: next spawn recycles deterministically.
  const world::Entity e1 = s.spawn_entity();
  const world::Entity e2 = dst.spawn_entity();
  assert(e1 == e2);
}

void test_shm_round_trip_preserves_ids() {
  const world::Snapshot snap = world::take_snapshot(make_three());
  const link::Frame f = link::frame_from_snapshot(snap);
  link::LinkFrame wire{};
  assert(link::encode_frame(f, wire, 42).ok());
  link::Frame back;
  assert(link::decode_frame(wire, back).ok());
  assert(back == f);
  assert(back.items[1].has_identity && back.items[1].entity_index == 1);
  assert(link::snapshot_from_frame(back) == snap);
}

void test_compact_round_trip_preserves_ids() {
  const world::Snapshot snap = world::take_snapshot(make_three());
  const link::Frame f = link::frame_from_snapshot(snap);
  std::array<unsigned char, link::compact::kMaxRecordBytes> buf{};
  std::size_t len = 0;
  assert(link::compact::encode_record(f, "projectile", 99, 7, buf.data(), buf.size(), len)
             .ok());
  link::Frame back;
  std::uint64_t run_id = 0;
  std::uint64_t seq = 0;
  link::compact::RecordKind kind{};
  assert(link::compact::decode_record(buf.data(), len, "projectile", back, run_id, seq, kind)
             .ok());
  assert(back == f && run_id == 99 && seq == 7);
  assert(back.items[2].has_identity && back.items[2].entity_generation == 0);
  assert(link::snapshot_from_frame(back) == snap);
}

void test_legacy_frames_have_no_identity() {
  // Manually built snapshots (no identity vectors) adapt to frames without
  // identity; the inverse leaves identity empty (unavailable, never
  // fabricated from dense positions).
  world::Snapshot legacy;
  legacy.step_index = 1;
  legacy.sim_time = 0.01;
  legacy.pos_x = {1.0, 2.0};
  legacy.pos_y = {3.0, 4.0};
  legacy.vel_x = {5.0, 6.0};
  legacy.vel_y = {7.0, 8.0};
  assert(!legacy.has_particle_identity());
  const link::Frame f = link::frame_from_snapshot(legacy);
  assert(f.items.size() == 2);
  assert(!f.items[0].has_identity && !f.items[1].has_identity);
  // SHM + compact round-trips preserve the unavailable marking.
  link::LinkFrame wire{};
  assert(link::encode_frame(f, wire, 1).ok());
  link::Frame shm_back;
  assert(link::decode_frame(wire, shm_back).ok());
  assert(!shm_back.items[0].has_identity);
  std::array<unsigned char, 4096> buf{};
  std::size_t len = 0;
  assert(link::compact::encode_record(f, "projectile", 0, 1, buf.data(), buf.size(), len)
             .ok());
  link::Frame sock_back;
  std::uint64_t run_id = 0;
  std::uint64_t seq = 0;
  link::compact::RecordKind kind{};
  assert(link::compact::decode_record(buf.data(), len, "", sock_back, run_id, seq, kind)
             .ok());
  assert(!sock_back.items[0].has_identity);
  const world::Snapshot back = link::snapshot_from_frame(shm_back);
  assert(!back.has_particle_identity());
  assert(back.pos_x == legacy.pos_x); // values/order preserved, IDs unavailable
}

void test_csv_round_trip_preserves_ids() {
  const std::string path = "/tmp/wengine_test_particle_identity.csv";
  std::remove(path.c_str());
  const world::Snapshot snap = world::take_snapshot(make_three());
  {
    emit::CsvSink sink(path);
    assert(sink.ok());
    sink.on_snapshot(snap);
    assert(sink.flush().ok());
  }
  std::ifstream in(path);
  assert(static_cast<bool>(in));
  std::vector<world::Snapshot> out;
  std::string error;
  assert(emit::parse_csv_log(in, out, error).ok());
  assert(out.size() == 1 && out[0] == snap);
  std::remove(path.c_str());
  // Legacy 7-column logs still parse as unavailable.
  std::istringstream legacy("step,time,slot,x,y,vx,vy\n1,0.01,0,1,2,3,4\n");
  std::vector<world::Snapshot> leg;
  assert(emit::parse_csv_log(legacy, leg, error).ok());
  assert(leg.size() == 1 && !leg[0].has_particle_identity());
}

} // namespace

int main() {
  test_snapshot_carries_identity();
  test_frame_and_inverse_preserve_ids();
  test_despawn_keeps_survivor_identity();
  test_slot_reuse_has_different_identity();
  test_checkpoint_restore_preserves_ids();
  test_shm_round_trip_preserves_ids();
  test_compact_round_trip_preserves_ids();
  test_legacy_frames_have_no_identity();
  test_csv_round_trip_preserves_ids();
  std::cout << "test_particle_identity: OK\n";
  return 0;
}
