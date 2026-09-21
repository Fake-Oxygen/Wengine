#include "wengine/world/serialize.hpp"
#include "wengine/world/state.hpp"

#include <cassert>
#include <iostream>
#include <sstream>
#include <string>

using namespace wengine;

namespace {

world::State make_sample() {
  world::State s;
  s.sim_time = 1.5;
  s.step_index = 42;
  const world::Entity a = s.spawn_entity();
  const world::Entity b = s.spawn_entity();
  const world::Entity c = s.spawn_entity();
  s.particles.attach(a.index);
  s.particles.pos_x[a.index] = 1.0;
  s.particles.vel_y[a.index] = -2.0;
  s.particles.attach(b.index);
  s.particles.pos_x[b.index] = 3.0;
  assert(s.despawn_entity(b)); // hole in the middle
  s.particles.attach(c.index);
  s.particles.pos_y[c.index] = 5.0;
  return s; // NRVO/move: a(0,g0) + c(2,g0) live, b(1,g0) dead, freelist [1]
}

void test_registry() {
  world::EntityRegistry reg;
  const world::Entity a = reg.spawn();
  const world::Entity b = reg.spawn();
  const world::Entity c = reg.spawn();
  assert(a.index == 0 && b.index == 1 && c.index == 2);
  assert(a.generation == 0 && reg.alive_count() == 3);
  assert(reg.alive(a) && reg.alive(b) && reg.alive(c));
  assert(!reg.alive(world::kInvalidEntity));
  assert(!reg.alive(world::Entity{99, 0})); // foreign index

  assert(reg.despawn(b));
  assert(!reg.alive(b)); // stale handle never aliases
  assert(!reg.despawn(b)); // double despawn
  assert(reg.alive_count() == 2);

  const world::Entity d = reg.spawn(); // reuses slot 1, bumped generation
  assert(d.index == 1 && d.generation == 1 && reg.alive(d));
  assert(!reg.alive(b));
  assert(reg.alive_count() == 3);

  reg.clear();
  assert(reg.alive_count() == 0 && reg.capacity() == 0);
}

void test_state_spawn_despawn() {
  world::State s;
  const world::Entity e = s.spawn_entity();
  assert(s.entities.alive(e));
  assert(!s.particles.has(e.index)); // components are opt-in
  s.particles.attach(e.index);
  assert(s.particles.has(e.index) && s.particles.live_count == 1);
  s.particles.attach(e.index); // idempotent
  assert(s.particles.live_count == 1);

  const world::Entity bare = s.spawn_entity(); // no components
  assert(s.despawn_entity(bare));
  assert(!s.despawn_entity(bare));
  assert(!s.despawn_entity(world::Entity{7, 0}));

  assert(s.despawn_entity(e)); // detaches pools, releases slot
  assert(!s.particles.has(e.index) && s.particles.live_count == 0);
  assert(s.particles.capacity() > 0); // capacity retained
  s.clear();
  assert(s.sim_time == 0.0 && s.step_index == 0);
  assert(s.entities.alive_count() == 0 && s.particles.live_count == 0);
}

void test_snapshot() {
  world::State s = make_sample();
  const world::Snapshot snap = world::take_snapshot(s);
  assert(snap.sim_time == 1.5 && snap.step_index == 42);
  assert(snap.particle_count() == 2); // live slots only, slot order
  assert(snap.pos_x[0] == 1.0 && snap.vel_y[0] == -2.0);
  assert(snap.pos_y[1] == 5.0);

  world::State empty;
  const world::Snapshot e = world::take_snapshot(empty);
  assert(e.particle_count() == 0);
}

void test_round_trip() {
  const world::State src = make_sample();
  std::ostringstream bytes;
  assert(world::save_state(src, bytes).ok());

  world::State dst;
  std::istringstream in(bytes.str());
  assert(world::load_state(dst, in).ok());
  assert(dst == src);
  assert(world::take_snapshot(dst) == world::take_snapshot(src));

  // Freelist order survived: both recycle slot 1 with generation 1 next.
  world::State a = src;
  world::State b = dst;
  const world::Entity ea = a.spawn_entity();
  const world::Entity eb = b.spawn_entity();
  assert(ea == eb && ea.index == 1 && ea.generation == 1);
}

void test_corrupt_input() {
  std::ostringstream good;
  assert(world::save_state(make_sample(), good).ok());
  const std::string blob = good.str();
  assert(!blob.empty());

  auto load_of = [](const std::string &data) {
    world::State s;
    s.sim_time = 9.0; // poison: must be cleared on failure
    std::istringstream in(data);
    const core::Status st = world::load_state(s, in);
    return std::pair(st, s);
  };
  {
    std::string bad = blob;
    bad[0] = 'X';
    const auto [st, s] = load_of(bad);
    assert(!st.ok() && st.code == core::StatusCode::kInvalidArgument);
    assert(s.sim_time == 0.0 && s.step_index == 0);
  }
  {
    std::string bad = blob; // version field is bytes 4..7
    bad[4] = static_cast<char>(0x7F);
    const auto [st, s] = load_of(bad);
    assert(!st.ok());
    assert(s == world::State{});
  }
  {
    const auto [st, s] = load_of(blob.substr(0, blob.size() - 10)); // truncated
    assert(!st.ok());
    assert(s == world::State{});
  }
  {
    const auto [st, s] = load_of("");
    assert(!st.ok());
  }
}

void test_determinism() {
  assert(make_sample() == make_sample());
  assert(world::take_snapshot(make_sample()) == world::take_snapshot(make_sample()));
}

} // namespace

int main() {
  test_registry();
  test_state_spawn_despawn();
  test_snapshot();
  test_round_trip();
  test_corrupt_input();
  test_determinism();
  std::cout << "test_world: OK\n";
  return 0;
}
