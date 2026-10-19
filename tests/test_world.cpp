#include "wengine/world/serialize.hpp"
#include "wengine/world/state.hpp"
#include "wengine/signal/types.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <span>
#include <sstream>
#include <string>
#include <vector>

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

void test_fill_matches_take() {
  // fill_snapshot into a reused buffer is value-identical to take_snapshot,
  // for populated and empty states alike.
  const world::State s = make_sample();
  world::Snapshot buf;
  world::fill_snapshot(s, buf);
  assert(buf == world::take_snapshot(s));

  world::State empty;
  world::fill_snapshot(empty, buf); // shrink over a warmed buffer
  assert(buf == world::take_snapshot(empty));
  assert(buf.particle_count() == 0 && buf.signal_count() == 0);
}

signal::SignalParams dc_params(double offset) {
  signal::SignalParams p;
  p.kind = signal::SignalKind::kDc;
  p.offset = offset;
  return p;
}

void test_fill_signals_and_retained_copies() {
  // Both pools in one buffer; retained owned copies survive live-State
  // mutation plus buffer reuse.
  world::State s;
  s.sim_time = 0.5;
  s.step_index = 5;
  const world::Entity a = s.spawn_entity();
  assert(world::configure_source(a, s.signals, dc_params(1.0), 0.0).ok());
  const world::Entity b = s.spawn_entity();
  assert(world::configure_source(b, s.signals, dc_params(2.0), 0.0).ok());
  const world::Entity p = s.spawn_entity();
  s.particles.attach(p.index);
  s.particles.pos_x[p.index] = 7.0;

  world::Snapshot buf;
  world::fill_snapshot(s, buf);
  assert(buf == world::take_snapshot(s));
  assert(buf.signal_count() == 2);
  assert(buf.sig_serial[0] == 0 && buf.sig_serial[1] == 1); // slot order
  assert(buf.sig_value[0] == 1.0 && buf.sig_value[1] == 2.0);

  const world::Snapshot retained = buf; // owned copy, like a retaining sink
  s.signals.value[a.index] = 99.0;
  s.particles.pos_x[p.index] = -1.0;
  s.sim_time = 0.6;
  s.step_index = 6;
  world::fill_snapshot(s, buf); // reuse must not disturb `retained`
  assert(buf == world::take_snapshot(s));
  assert(buf.sig_value[0] == 99.0 && buf.pos_x[0] == -1.0);
  assert(retained.sim_time == 0.5 && retained.step_index == 5);
  assert(retained.sig_value[0] == 1.0 && retained.sig_value[1] == 2.0);
  assert(retained.pos_x[0] == 7.0);
  assert(!(buf == retained));
}

void test_fill_spawn_despawn_and_zero() {
  // Growth, holes, despawn-to-zero, and respawn all refill correctly.
  world::State s;
  world::Snapshot buf;
  world::fill_snapshot(s, buf); // zero sources, cold buffer
  assert(buf == world::take_snapshot(s));
  assert(buf.signal_count() == 0);

  const world::Entity a = s.spawn_entity();
  assert(world::configure_source(a, s.signals, dc_params(1.0), 0.0).ok());
  const world::Entity b = s.spawn_entity();
  assert(world::configure_source(b, s.signals, dc_params(2.0), 0.0).ok());
  const world::Entity c = s.spawn_entity();
  assert(world::configure_source(c, s.signals, dc_params(3.0), 0.0).ok());
  world::fill_snapshot(s, buf);
  assert(buf == world::take_snapshot(s));
  assert(buf.signal_count() == 3);

  assert(s.despawn_entity(b)); // hole in the middle, serials stable
  world::fill_snapshot(s, buf);
  assert(buf == world::take_snapshot(s));
  assert(buf.signal_count() == 2);
  assert(buf.sig_serial[0] == 0 && buf.sig_serial[1] == 2); // slot order
  assert(buf.sig_value[0] == 1.0 && buf.sig_value[1] == 3.0);

  assert(s.despawn_entity(a) && s.despawn_entity(c)); // back to zero live
  world::fill_snapshot(s, buf);
  assert(buf == world::take_snapshot(s)); // capacities retained, sizes zero
  assert(buf.signal_count() == 0);

  const world::Entity d = s.spawn_entity(); // slot recycled, serial fresh
  assert(world::configure_source(d, s.signals, dc_params(9.0), 0.0).ok());
  world::fill_snapshot(s, buf);
  assert(buf == world::take_snapshot(s));
  assert(buf.signal_count() == 1 && buf.sig_serial[0] == 3);
  assert(buf.sig_value[0] == 9.0);
}

void test_select_into_matches_select() {
  // select_signals_into reuses capacity and matches the owning copy;
  // retained copies survive scratch reuse.
  world::Snapshot s;
  s.step_index = 4;
  s.sim_time = 0.04;
  s.sig_serial = {0, 1, 2};
  s.sig_value = {10.0, 20.0, 30.0};
  s.sig_quantity = {0, 0, 0};
  const std::uint64_t want[2] = {2, 0};

  world::Snapshot buf;
  world::select_signals_into(s, std::span<const std::uint64_t>(want, 2), buf);
  const world::Snapshot owned =
      world::select_signals(s, std::span<const std::uint64_t>(want, 2));
  assert(buf == owned); // slot order, not request order
  assert(buf.signal_count() == 2);
  assert(buf.sig_serial[0] == 0 && buf.sig_serial[1] == 2);
  assert(buf.sig_value[0] == 10.0 && buf.sig_value[1] == 30.0);

  const world::Snapshot retained = buf;
  const std::uint64_t one[1] = {1};
  world::select_signals_into(s, std::span<const std::uint64_t>(one, 1), buf);
  assert(buf.signal_count() == 1 && buf.sig_serial[0] == 1);
  assert(retained.signal_count() == 2 && retained.sig_serial[1] == 2);
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
  test_fill_matches_take();
  test_fill_signals_and_retained_copies();
  test_fill_spawn_despawn_and_zero();
  test_select_into_matches_select();
  test_round_trip();
  test_corrupt_input();
  test_determinism();
  std::cout << "test_world: OK\n";
  return 0;
}
