#include "wengine/core/runner.hpp"
#include "wengine/emit/routing.hpp"
#include "wengine/emit/signal_sink.hpp"
#include "wengine/signal/types.hpp"
#include "wengine/solvers/run.hpp"
#include "wengine/solvers/signal.hpp"
#include "wengine/world/serialize.hpp"

#include <array>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <span>
#include <sstream>
#include <string>
#include <vector>

using namespace wengine;

namespace {

constexpr bool near(double a, double b, double eps = 1e-9) {
  return std::fabs(a - b) <= eps * (1.0 + std::fabs(b));
}

struct Capture : emit::ISink {
  std::vector<world::Snapshot> snaps;
  void on_snapshot(const world::Snapshot &s) override { snaps.push_back(s); }
};

signal::SignalParams dc(double offset) {
  signal::SignalParams p;
  p.kind = signal::SignalKind::kDc;
  p.offset = offset;
  return p;
}

world::Entity spawn_checked(core::Runner &runner, const signal::SignalParams &p) {
  world::Entity e = runner.state().spawn_entity();
  assert(world::configure_source(e, runner.state().signals, p, runner.state().sim_time).ok());
  return e;
}

void test_serial_stability() {
  // Slots recycle, serials don't: subscriptions survive respawn.
  core::Runner runner{{.dt = 0.01}};
  solvers::SignalSolver solver;
  runner.add_solver(solver);
  signal::SignalParams s;
  s.freq_hz = 1.0;
  s.amp = 2.0;
  const world::Entity a = spawn_checked(runner, s);
  const world::Entity b = spawn_checked(runner, dc(3.0));
  assert(runner.state().signals.serial[a.index] == 0);
  assert(runner.state().signals.serial[b.index] == 1);
  assert(runner.state().despawn_entity(a));
  const world::Entity c = spawn_checked(runner, dc(9.0));
  assert(c.index == a.index); // slot recycled
  assert(runner.state().signals.serial[c.index] == 2); // serial never reused
  assert(!world::find_serial(runner.state().signals, 0).has_value());
  assert(*world::find_serial(runner.state().signals, 1) == b.index);
  assert(*world::find_serial(runner.state().signals, 2) == c.index);

  assert(runner.run(3).ok());
  const world::Snapshot snap = world::take_snapshot(runner.state());
  assert(snap.signal_count() == 2);
  assert(snap.sig_serial[0] == 2 && snap.sig_serial[1] == 1); // slot order
  assert(snap.sig_value[0] == 9.0 && snap.sig_value[1] == 3.0); // B untouched
}

void test_subset_routing() {
  // 1:N fan-out with per-sink subscriptions + N:1 mix-down, then
  // reconfiguration mid-run without touching the Runner.
  core::Runner runner{{.dt = 0.01}};
  solvers::SignalSolver solver;
  runner.add_solver(solver);
  spawn_checked(runner, dc(1.0)); // serial 0
  spawn_checked(runner, dc(2.0)); // serial 1

  Capture full;
  Capture only0;
  Capture only1;
  emit::SignalSubsetSink sub0(only0);
  emit::SignalSubsetSink sub1(only1);
  sub0.set_subscription({0});
  sub1.set_subscription({1});
  runner.add_sink(full);
  runner.add_sink(sub0);
  runner.add_sink(sub1);
  assert(runner.run(3).ok());

  assert(full.snaps.size() == 3);
  assert(full.snaps[0].signal_count() == 2);
  assert(only0.snaps.size() == 3 && only1.snaps.size() == 3);
  for (const world::Snapshot &sn : only0.snaps) {
    assert(sn.signal_count() == 1 && sn.sig_serial[0] == 0 && sn.sig_value[0] == 1.0);
  }
  for (const world::Snapshot &sn : only1.snaps) {
    assert(sn.signal_count() == 1 && sn.sig_serial[0] == 1 && sn.sig_value[0] == 2.0);
  }
  assert(sub0.matched() == 3 && sub0.dropped() == 0 && sub0.forwarded_rows() == 3);

  // Reconfigure mid-run: sub0 now takes both, sub1 mutes.
  sub0.set_subscription({0, 1});
  sub1.set_subscription({});
  assert(runner.run(2).ok());
  assert(only0.snaps.size() == 5);
  assert(only0.snaps[3].signal_count() == 2 && only0.snaps[4].signal_count() == 2);
  assert(only1.snaps.size() == 3); // muted: nothing more forwarded
  assert(sub1.dropped() == 2); // two muted snapshots counted loudly
  assert(full.snaps.size() == 5); // unfiltered sink unaffected
}

void test_subset_ticks_and_drops() {
  // Unknown serials: ticks still forwarded (empty rows), drops counted.
  core::Runner runner{{.dt = 0.01}};
  solvers::SignalSolver solver;
  runner.add_solver(solver);
  spawn_checked(runner, dc(1.0));
  Capture inner;
  emit::SignalSubsetSink sub(inner);
  sub.set_subscription({9}); // never exists
  runner.add_sink(sub);
  assert(runner.run(2).ok());
  assert(inner.snaps.size() == 2); // ticks preserved for downstream readers
  assert(inner.snaps[0].signal_count() == 0);
  assert(sub.matched() == 2 && sub.forwarded_rows() == 0 && sub.dropped() == 2);
}

void test_select_order() {
  // select_signals preserves snapshot (slot) order, not request order.
  world::Snapshot s;
  s.step_index = 4;
  s.sim_time = 0.04;
  s.sig_serial = {0, 1, 2};
  s.sig_value = {10.0, 20.0, 30.0};
  s.sig_quantity = {0, 0, 0};
  const world::Snapshot sub =
      world::select_signals(s, std::span<const std::uint64_t>({2, 0}));
  assert(sub.step_index == 4 && sub.sim_time == 0.04);
  assert(sub.signal_count() == 2);
  assert(sub.sig_serial[0] == 0 && sub.sig_serial[1] == 2);
  assert(sub.sig_value[0] == 10.0 && sub.sig_value[1] == 30.0);
}

void test_stride() {
  core::Runner runner{{.dt = 0.01}};
  solvers::SignalSolver solver;
  runner.add_solver(solver);
  spawn_checked(runner, dc(1.0));
  Capture inner;
  emit::StrideSink stride(inner, 3);
  runner.add_sink(stride);
  assert(runner.run(9).ok());
  // Step-aligned decimation: steps 3, 6, 9 — not "every 3rd received".
  assert(inner.snaps.size() == 3);
  assert(inner.snaps[0].step_index == 3);
  assert(inner.snaps[1].step_index == 6);
  assert(inner.snaps[2].step_index == 9);
  assert(stride.matched() == 3 && stride.dropped() == 6);
  // Reconfigurable mid-run without a rebuild.
  stride.set_stride(1);
  assert(runner.run(2).ok());
  assert(inner.snaps.size() == 5);
  assert(stride.matched() == 5);
  stride.set_stride(0); // muted
  assert(runner.run(1).ok());
  assert(inner.snaps.size() == 5 && stride.dropped() == 7);
}

void test_sum_golden() {
  // sum = sine(1 Hz, amp 2) + dc(3): closed form per axis, derived by hand.
  core::Runner runner{{.dt = 0.01}};
  solvers::SignalSolver solver;
  runner.add_solver(solver);
  signal::SignalParams sine;
  sine.freq_hz = 1.0;
  sine.amp = 2.0;
  spawn_checked(runner, sine); // serial 0
  spawn_checked(runner, dc(3.0)); // serial 1
  signal::SignalParams sum;
  sum.kind = signal::SignalKind::kSum;
  sum.ops[0] = 0;
  sum.ops[1] = 1;
  sum.op_count = 2;
  const world::Entity e = runner.state().spawn_entity();
  assert(world::configure_source(e, runner.state().signals, sum, 0.0).ok());
  // Spawn-time sample = operands' current sum (sine(0) + 3).
  assert(runner.state().signals.value[e.index] == 3.0);
  assert(runner.run(100).ok()); // t = 1 s: sine(2pi) ~= 0
  const double v = runner.state().signals.value[e.index];
  assert(near(v, 3.0, 1e-9));
  // Slot order independence: operand slots bracket the sum either way.
  const world::Snapshot snap = world::take_snapshot(runner.state());
  assert(snap.signal_count() == 3 && snap.sig_serial[2] == 2);
}

void test_sum_rejections() {
  core::Runner runner{{.dt = 0.01}};
  solvers::SignalSolver solver;
  runner.add_solver(solver);
  spawn_checked(runner, dc(1.0)); // serial 0
  const std::size_t before = runner.state().signals.live_count;

  auto try_sum = [&](std::uint64_t a, std::uint64_t b, std::uint8_t n) {
    world::Entity e = runner.state().spawn_entity();
    signal::SignalParams sum;
    sum.kind = signal::SignalKind::kSum;
    sum.ops[0] = a;
    sum.ops[1] = b;
    sum.op_count = n;
    const core::Status st =
        world::configure_source(e, runner.state().signals, sum, 0.0);
    runner.state().despawn_entity(e); // keep the pool tidy either way
    return st;
  };
  assert(!try_sum(9, 0, 2).ok()); // unknown serial
  assert(!try_sum(0, 0, 0).ok()); // empty operand list
  assert(runner.state().signals.live_count == before); // untouched on failure
  // Sum-of-sum is rejected (sums stay flattened).
  world::Entity e1 = runner.state().spawn_entity();
  signal::SignalParams sum1;
  sum1.kind = signal::SignalKind::kSum;
  sum1.ops[0] = 0;
  sum1.op_count = 1;
  assert(world::configure_source(e1, runner.state().signals, sum1, 0.0).ok());
  const std::uint64_t sum_serial = runner.state().signals.serial[e1.index];
  world::Entity e2 = runner.state().spawn_entity();
  signal::SignalParams sum2 = sum1;
  sum2.ops[0] = sum_serial;
  assert(!world::configure_source(e2, runner.state().signals, sum2, 0.0).ok());
  runner.state().despawn_entity(e1);
  runner.state().despawn_entity(e2);

  // Batch path rejects sums (identical batch sources can't hold distinct
  // operands); the per-source family (SIG-6) configures sums directly.
  core::RunConfig cfg;
  cfg.loop.dt = 0.01;
  cfg.steps = 2;
  cfg.solver = "signal";
  cfg.particles = 0;
  cfg.signal.set = true;
  cfg.signal.params.kind = signal::SignalKind::kSum;
  cfg.signal.params.op_count = 1;
  world::State out;
  assert(!solvers::run_headless(cfg, std::span<emit::ISink *>{}, out).ok());
}

void test_dangling_operand_fails_loudly() {
  core::Runner runner{{.dt = 0.01}};
  solvers::SignalSolver solver;
  runner.add_solver(solver);
  const world::Entity a = spawn_checked(runner, dc(1.0)); // serial 0
  world::Entity e = runner.state().spawn_entity();
  signal::SignalParams sum;
  sum.kind = signal::SignalKind::kSum;
  sum.ops[0] = 0;
  sum.op_count = 1;
  assert(world::configure_source(e, runner.state().signals, sum, 0.0).ok());
  assert(runner.run(2).ok());
  assert(runner.state().despawn_entity(a)); // operand gone mid-run
  const std::uint64_t step = runner.state().step_index;
  const double t = runner.state().sim_time;
  assert(!runner.run(1).ok()); // loud, deterministic failure...
  assert(runner.state().step_index == step); // ...advancing neither time
  assert(runner.state().sim_time == t); // ...nor index, emitting nothing
}

void test_sink_failure_isolation() {
  // A failing sink never alters sim state or timing for its siblings.
  core::Runner runner{{.dt = 0.01}};
  solvers::SignalSolver solver;
  runner.add_solver(solver);
  spawn_checked(runner, dc(1.0));
  Capture good;
  emit::SignalsCsvSink broken("/nonexistent-dir-xyz/sig.csv");
  assert(!broken.ok());
  runner.add_sink(good);
  runner.add_sink(broken);
  assert(runner.run(5).ok());
  assert(runner.state().step_index == 5);
  assert(good.snaps.size() == 5);
  assert(good.snaps[4].sig_value[0] == 1.0);
}

void test_serialize_v3_and_v2() {
  core::Runner runner{{.dt = 0.01}};
  solvers::SignalSolver solver;
  runner.add_solver(solver);
  signal::SignalParams sine;
  sine.freq_hz = 2.0;
  sine.amp = 1.5;
  spawn_checked(runner, sine); // serial 0
  world::Entity e = runner.state().spawn_entity();
  signal::SignalParams sum;
  sum.kind = signal::SignalKind::kSum;
  sum.ops[0] = 0;
  sum.op_count = 1;
  sum.offset = 0.25;
  assert(world::configure_source(e, runner.state().signals, sum, 0.0).ok());
  assert(runner.run(4).ok());

  std::ostringstream bytes;
  assert(world::save_state(runner.state(), bytes).ok());
  world::State dst;
  std::istringstream in(bytes.str());
  assert(world::load_state(dst, in).ok());
  assert(dst == runner.state());
  assert(world::take_snapshot(dst) == world::take_snapshot(runner.state()));
  assert(dst.signals.serial[1] == 1);

  // Hand-crafted v2 blob (no serials/operands): loads with serials
  // synthesized as slot indices and empty operand lists.
  std::string v2;
  v2 += 'W';
  v2 += 'E';
  v2 += 'N';
  v2 += 'G';
  auto put_u32 = [&](std::uint32_t v) {
    for (int k = 0; k < 4; ++k) {
      v2 += static_cast<char>(v & 0xFFu);
      v >>= 8;
    }
  };
  auto put_u64 = [&](std::uint64_t v) {
    for (int k = 0; k < 8; ++k) {
      v2 += static_cast<char>(v & 0xFFu);
      v >>= 8;
    }
  };
  auto put_f64 = [&](double v) { put_u64(std::bit_cast<std::uint64_t>(v)); };
  put_u32(2); // version
  put_f64(0.5); // sim_time
  put_u64(5); // step_index
  put_u32(1); // one entity...
  put_u32(0); // ...generation 0...
  put_u32(0); // ...nothing freed
  put_u32(0); // particles
  put_u32(1); // one signal slot
  v2 += static_cast<char>(1); // present
  v2 += static_cast<char>(4); // kind = dc
  v2 += static_cast<char>(0); // quantity = U
  v2 += static_cast<char>(1); // running
  put_f64(0.0); // freq
  put_f64(0.0); // amp
  put_f64(2.5); // offset
  put_f64(0.0); // phase
  put_f64(0.5); // duty
  put_f64(0.0); // t0
  put_f64(2.5); // value
  put_u64(7); // seed
  put_u64(7); // rng
  world::State old;
  std::istringstream vin(v2);
  assert(world::load_state(old, vin).ok());
  assert(old.signals.live_count == 1);
  assert(old.signals.serial[0] == 0); // synthesized
  assert(old.signals.value[0] == 2.5);
  assert(old.signals.op_count[0] == 0);
  assert(old.sim_time == 0.5 && old.step_index == 5);
  // Serial counter continues past synthesized ids (no aliasing).
  world::Entity fresh = old.spawn_entity();
  signal::SignalParams p;
  assert(world::configure_source(fresh, old.signals, p, 0.0).ok());
  assert(old.signals.serial[fresh.index] == 1);
}

void test_sum_quantity_validation() {
  // Dimensional rule: every operand must carry the sum's own quantity.
  // All quantity choices below derive from kQuantities (no per-quantity
  // switch), so a future appended row works untouched.
  assert(signal::kQuantityCount >= 2);
  const signal::QuantityId q0 = signal::kQuantities[0].id;
  const signal::QuantityId q1 = signal::kQuantities[1].id;
  assert(q0 != q1);
  // The helper agrees with the table: same-dimensional compatible,
  // cross-dimensional rejected — generically, for every row.
  for (const signal::QuantityDesc &d : signal::kQuantities) {
    const signal::QuantityId same = d.id;
    assert(signal::sum_quantities_compatible(same, std::span<const signal::QuantityId>(&same, 1)));
    for (const signal::QuantityDesc &e : signal::kQuantities) {
      if (e.id == d.id) {
        continue;
      }
      const std::array<signal::QuantityId, 2> mixed{same, e.id};
      assert(!signal::sum_quantities_compatible(
          same, std::span<const signal::QuantityId>(mixed.data(), mixed.size())));
    }
  }

  auto dc_q = [](double offset, signal::QuantityId q) {
    signal::SignalParams p;
    p.kind = signal::SignalKind::kDc;
    p.quantity = q;
    p.offset = offset;
    return p;
  };

  core::Runner runner{{.dt = 0.01}};
  solvers::SignalSolver solver;
  runner.add_solver(solver);
  const world::Entity a = spawn_checked(runner, dc_q(1.0, q0)); // serial 0
  const world::Entity b = spawn_checked(runner, dc_q(2.0, q1)); // serial 1
  const world::Entity c = spawn_checked(runner, dc_q(4.0, q0)); // serial 2
  const std::uint64_t sa = runner.state().signals.serial[a.index];
  const std::uint64_t sb = runner.state().signals.serial[b.index];
  const std::uint64_t sc = runner.state().signals.serial[c.index];
  const std::size_t live_before = runner.state().signals.live_count;

  auto try_sum = [&](signal::QuantityId out_q, std::uint64_t o0, std::uint64_t o1) {
    world::Entity e = runner.state().spawn_entity();
    signal::SignalParams sum;
    sum.kind = signal::SignalKind::kSum;
    sum.quantity = out_q;
    sum.ops[0] = o0;
    sum.ops[1] = o1;
    sum.op_count = 2;
    const core::Status st =
        world::configure_source(e, runner.state().signals, sum, 0.0);
    runner.state().despawn_entity(e);
    return st;
  };
  // Mixed operands fail for either output tag; a same-operand pair with a
  // mismatched output tag fails too — all before pool mutation.
  assert(!try_sum(q0, sa, sb).ok()); // q0 + q1 -> q0
  assert(!try_sum(q1, sa, sb).ok()); // q0 + q1 -> q1
  assert(!try_sum(q1, sa, sc).ok()); // q0 + q0 -> q1 (output mismatch)
  assert(runner.state().signals.live_count == live_before);

  // Same-dimension sums keep operand order and exact values (dc is exact,
  // so == is the right assertion — no tolerance widening).
  world::Entity e0 = runner.state().spawn_entity();
  signal::SignalParams sum0;
  sum0.kind = signal::SignalKind::kSum;
  sum0.quantity = q0;
  sum0.ops[0] = sa;
  sum0.ops[1] = sc;
  sum0.op_count = 2;
  sum0.offset = 0.5;
  assert(world::configure_source(e0, runner.state().signals, sum0, 0.0).ok());
  assert(runner.state().signals.value[e0.index] == 1.0 + 4.0 + 0.5);
  assert(runner.run(3).ok());
  assert(runner.state().signals.value[e0.index] == 1.0 + 4.0 + 0.5);

  // Generic loop: every table row sums with itself; any distinct pair fails.
  for (const signal::QuantityDesc &d : signal::kQuantities) {
    core::Runner g{{.dt = 0.01}};
    solvers::SignalSolver gs;
    g.add_solver(gs);
    const world::Entity g0 = spawn_checked(g, dc_q(1.0, d.id));
    const world::Entity g1 = spawn_checked(g, dc_q(2.0, d.id));
    const std::uint64_t s0 = g.state().signals.serial[g0.index];
    const std::uint64_t s1 = g.state().signals.serial[g1.index];
    world::Entity ge = g.state().spawn_entity();
    signal::SignalParams gp;
    gp.kind = signal::SignalKind::kSum;
    gp.quantity = d.id;
    gp.ops[0] = s0;
    gp.ops[1] = s1;
    gp.op_count = 2;
    assert(world::configure_source(ge, g.state().signals, gp, 0.0).ok());
    assert(g.state().signals.value[ge.index] == 3.0);
    for (const signal::QuantityDesc &f : signal::kQuantities) {
      if (f.id == d.id) {
        continue;
      }
      const world::Entity hx = g.state().spawn_entity();
      signal::SignalParams hp = gp;
      hp.quantity = f.id;
      assert(!world::configure_source(hx, g.state().signals, hp, 0.0).ok());
      g.state().despawn_entity(hx);
    }
  }

  // Stopped operands contribute their frozen sample (exact ==, solver skips
  // stopped slots so the bits never change).
  core::Runner fr{{.dt = 0.01}};
  solvers::SignalSolver fs;
  fr.add_solver(fs);
  signal::SignalParams sine;
  sine.kind = signal::SignalKind::kSine;
  sine.quantity = q0;
  sine.freq_hz = 1.0;
  sine.amp = 2.0;
  const world::Entity fa = spawn_checked(fr, sine);
  const world::Entity fb = spawn_checked(fr, dc_q(3.0, q0));
  const std::uint64_t fsa = fr.state().signals.serial[fa.index];
  const std::uint64_t fsb = fr.state().signals.serial[fb.index];
  world::Entity fe = fr.state().spawn_entity();
  signal::SignalParams fsum;
  fsum.kind = signal::SignalKind::kSum;
  fsum.quantity = q0;
  fsum.ops[0] = fsa;
  fsum.ops[1] = fsb;
  fsum.op_count = 2;
  assert(world::configure_source(fe, fr.state().signals, fsum, 0.0).ok());
  assert(fr.run(5).ok());
  fr.state().signals.running[fa.index] = 0; // freeze the sine in place
  const double frozen = fr.state().signals.value[fa.index];
  assert(fr.run(4).ok());
  assert(fr.state().signals.value[fa.index] == frozen);
  assert(fr.state().signals.value[fe.index] == frozen + 3.0);

  // Checkpoint boundary: a valid same-dimension sum round-trips; flipping
  // the stored output quantity (bypassing configure) fails the load, as
  // does a dangling operand left by a despawn before save.
  {
    std::ostringstream bytes;
    assert(world::save_state(runner.state(), bytes).ok());
    world::State dst;
    std::istringstream in(bytes.str());
    assert(world::load_state(dst, in).ok());
    assert(dst == runner.state());
  }
  {
    // Corrupt the sum's output tag behind configure's back: save the
    // tampered pool, then the load must reject it.
    runner.state().signals.quantity[e0.index] =
        static_cast<std::uint8_t>(q1);
    std::ostringstream bad;
    assert(world::save_state(runner.state(), bad).ok());
    runner.state().signals.quantity[e0.index] =
        static_cast<std::uint8_t>(q0); // restore the live pool
    world::State dst;
    std::istringstream in(bad.str());
    assert(!world::load_state(dst, in).ok());
  }
  {
    core::Runner dr{{.dt = 0.01}};
    solvers::SignalSolver ds;
    dr.add_solver(ds);
    const world::Entity d0 = spawn_checked(dr, dc_q(1.0, q0));
    const world::Entity d1 = spawn_checked(dr, dc_q(2.0, q0));
    const std::uint64_t sd0 = dr.state().signals.serial[d0.index];
    world::Entity de = dr.state().spawn_entity();
    signal::SignalParams dp;
    dp.kind = signal::SignalKind::kSum;
    dp.quantity = q0;
    dp.ops[0] = sd0;
    dp.ops[1] = dr.state().signals.serial[d1.index];
    dp.op_count = 2;
    assert(world::configure_source(de, dr.state().signals, dp, 0.0).ok());
    assert(dr.state().despawn_entity(d0)); // operand gone: dangling sum
    std::ostringstream bad;
    assert(world::save_state(dr.state(), bad).ok());
    world::State dst;
    std::istringstream in(bad.str());
    assert(!world::load_state(dst, in).ok());
  }
  runner.state().despawn_entity(e0);
}

} // namespace

int main() {
  test_serial_stability();
  test_subset_routing();
  test_subset_ticks_and_drops();
  test_select_order();
  test_stride();
  test_sum_golden();
  test_sum_rejections();
  test_dangling_operand_fails_loudly();
  test_sink_failure_isolation();
  test_serialize_v3_and_v2();
  test_sum_quantity_validation();
  std::cout << "test_routing: OK\n";
  return 0;
}
