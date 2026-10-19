// Lua scripting source (SIG-4b): pin, sandbox, budget, goldens,
// determinism, serialization, and headless wiring. Links
// wengine_signal_lua (isolated runtime); the engine stays Lua-free.

#include "wengine/signal/lua.hpp"
#include "wengine/signal/types.hpp"
#include "wengine/solvers/run.hpp"
#include "wengine/solvers/signal.hpp"
#include "wengine/world/serialize.hpp"
#include "wengine/world/state.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace wengine;

namespace {

constexpr bool near(double a, double b, double eps = 1e-9) {
  return std::fabs(a - b) <= eps * (1.0 + std::fabs(b));
}

void test_pin() {
  // Pinned interpreter: a submodule bump fails here until goldens are
  // re-verified (dependency policy).
  assert(lua::language_version() == 504);
  assert(lua::release() == "Lua 5.4.9");
}

void test_check_script() {
  assert(lua::check_script("return 1").ok());
  assert(lua::check_script("return math.sin(t)").ok());
  assert(!lua::check_script("").ok());
  assert(!lua::check_script("return 1+").ok());
  assert(!lua::check_script("return `backtick`").ok());
  const std::string big(signal::kMaxLuaScriptBytes + 1, ' ');
  assert(!lua::check_script(std::string_view(big.data(), big.size())).ok());
}

struct LuaRun {
  core::Runner runner;
  solvers::SignalSolver signal;
  std::unique_ptr<lua::LuaSignalSolver> lua;
  LuaRun(double dt, lua::LuaConfig cfg = {})
      : runner({.dt = dt}), lua(lua::create_lua_solver(cfg)) {
    runner.add_solver(*lua); // extras step first (custom-source contract)
    runner.add_solver(signal);
  }
};

world::Entity spawn_lua(core::Runner &runner, std::string_view script,
                        signal::QuantityId qty = signal::QuantityId::kVoltage) {
  world::Entity e = runner.state().spawn_entity();
  assert(lua::configure_lua_source(e, runner.state().signals, script, qty,
                                   runner.state().sim_time)
             .ok());
  return e;
}

void test_sandbox_rejections() {
  // Every escape hatch fails the step loudly (nil global or read-only
  // library); nothing prints, nothing executes, nothing persists.
  const char *evil[] = {
      "return os.execute('x')",
      "return io.open('x')",
      "x = require('y')",
      "return load('return 1')",
      "print('x')",
      "return debug.getinfo(1)",
      "c = coroutine.create(function() end)\nreturn 1",
      "return tostring({})",
      "return string.format('%p', {})",
      "return collectgarbage('count')",
      "dofile('x')",
      "math.sin = 1\nreturn 1", // existing-key write: proxy __newindex fires
      "math.foo = 1\nreturn 1", // new-key write: same gate
      "string.len = 1\nreturn 1",
      "string.foo = 1\nreturn 1",
      "return getmetatable(math)",
      "return math.random()",
      "return 1/0", // Lua float division: inf, caught by the finite check
      "return 0/0", // nan, same gate
      "return nil",
      "return 'x'",
  };
  for (const char *script : evil) {
    LuaRun r(0.01);
    const world::Entity e = spawn_lua(r.runner, script);
    assert(r.runner.state().signals.value[e.index] == 0.0); // pre-step sample
    const core::Status st = r.runner.run(1);
    assert(!st.ok() && st.code == core::StatusCode::kInvalidArgument);
    assert(r.runner.state().step_index == 0); // frozen
    assert(!r.lua->last_error().empty());
    assert(r.lua->failures() == 1 && r.lua->evals() == 0);
  }
}

void test_allowed_surface() {
  LuaRun r(0.1);
  spawn_lua(r.runner, "return math.floor(2.5)+string.len('ab')");
  assert(r.runner.run(1).ok());
  assert(r.runner.state().signals.value[0] == 4.0);
  assert(r.lua->evals() == 1 && r.lua->last_error().empty());

  // pairs() iterates the real library through the proxy (__pairs): the
  // count pins the sandboxed lib surface exactly (nil-stripped entries
  // do not iterate).
  LuaRun r2(0.1);
  spawn_lua(r2.runner, "local n=0 for k,v in pairs(math) do n=n+1 end return n");
  assert(r2.runner.run(1).ok());
  assert(r2.runner.state().signals.value[0] == 25.0);
}

void test_sine_golden() {
  // Lua sine matches the parametric sine at shared times. Both call the
  // same libm on the same doubles, so the cross-check is exact.
  LuaRun r(0.001);
  spawn_lua(r.runner, "return 2*math.sin(2*math.pi*t)");
  assert(r.runner.run(250).ok());
  assert(near(r.runner.state().signals.value[0], 2.0, 1e-9));

  core::Runner ref({.dt = 0.001});
  solvers::SignalSolver s;
  ref.add_solver(s);
  signal::SignalParams p;
  p.freq_hz = 1.0;
  p.amp = 2.0;
  world::Entity e = ref.state().spawn_entity();
  assert(world::configure_source(e, ref.state().signals, p, 0.0).ok());
  assert(ref.run(250).ok());
  assert(r.runner.state().signals.value[0] == ref.state().signals.value[0]);
}

void test_inputs() {
  // All four inputs in one script: t (source time), seed (serial 0 here),
  // step (1-based step being computed), dt.
  LuaRun r(0.1);
  spawn_lua(r.runner, "return t+seed+step+dt");
  assert(r.runner.run(1).ok());
  assert(near(r.runner.state().signals.value[0], 1.2, 1e-9)); // .1+0+1+.1
  assert(r.runner.run(1).ok());
  assert(near(r.runner.state().signals.value[0], 2.3, 1e-9)); // .2+0+2+.1

  // Seed exposure: second source in the same state sees serial 1.
  LuaRun r2(0.1);
  spawn_lua(r2.runner, "return 0");
  const world::Entity e = spawn_lua(r2.runner, "return seed");
  assert(r2.runner.run(1).ok());
  assert(r2.runner.state().signals.value[e.index] == 1.0);
}

void test_budget() {
  // Infinite loop fails on the instruction budget with time/index frozen.
  LuaRun r(0.01);
  spawn_lua(r.runner, "while true do end");
  const core::Status st = r.runner.run(10);
  assert(!st.ok() && st.code == core::StatusCode::kInvalidArgument);
  assert(r.runner.state().step_index == 0);
  assert(r.lua->last_error().find("budget") != std::string::npos);

  // Budgets are configurable: heavy-but-finite work fails small, passes big.
  LuaRun small(0.01, lua::LuaConfig{100});
  spawn_lua(small.runner, "local s=0 for i=1,1000 do s=s+i end return s");
  assert(!small.runner.run(1).ok());

  LuaRun big(0.01, lua::LuaConfig{10000000});
  spawn_lua(big.runner, "local s=0 for i=1,100000 do s=s+i end return s");
  assert(big.runner.run(1).ok());
  assert(big.runner.state().signals.value[0] == 5000050000.0);

  // Zero budget rejects everything.
  LuaRun zero(0.01, lua::LuaConfig{0});
  spawn_lua(zero.runner, "return 1");
  assert(!zero.runner.run(1).ok());
}

void test_determinism_and_purity() {
  // A stateful-looking script cannot actually keep state (fresh _ENV):
  // the counter restarts every call, so every sample is exactly 1.
  LuaRun r(0.01);
  spawn_lua(r.runner, "n = (n or 0) + 1 return n");
  assert(r.runner.run(5).ok());
  assert(r.runner.state().signals.value[0] == 1.0);

  // Identical full runs are bit-identical (pool text included in ==).
  LuaRun r1(0.01);
  spawn_lua(r1.runner, "return math.sin(7*t)+seed");
  assert(r1.runner.run(5).ok());
  LuaRun r2(0.01);
  spawn_lua(r2.runner, "return math.sin(7*t)+seed");
  assert(r2.runner.run(5).ok());
  assert(r1.runner.state() == r2.runner.state());
}

void test_save_load_continue() {
  // Save/load/continue is exact with a FRESH solver (cache rebuild proof):
  // scripts are pure functions of their inputs, so no Lua state crosses.
  LuaRun r(0.1);
  spawn_lua(r.runner, "return 2*t+seed");
  assert(r.runner.run(3).ok());
  std::ostringstream bytes;
  assert(world::save_state(r.runner.state(), bytes).ok());

  world::State loaded;
  std::istringstream in(bytes.str());
  assert(world::load_state(loaded, in).ok());
  assert(loaded == r.runner.state());

  LuaRun r2(0.1); // fresh solver, empty cache
  r2.runner.state() = loaded;
  assert(r.runner.run(2).ok());
  assert(r2.runner.run(2).ok());
  assert(r2.runner.state() == r.runner.state());
  assert(world::take_snapshot(r2.runner.state()) ==
         world::take_snapshot(r.runner.state()));
}

void test_headless_extras() {
  // Batch Lua through the headless driver with an extra solver.
  core::RunConfig cfg;
  cfg.loop.dt = 0.1;
  cfg.steps = 2;
  cfg.solver = "signal";
  cfg.particles = 0;
  cfg.signal.params.kind = signal::SignalKind::kLua;
  cfg.signal.lua = "return 2*t+1";
  cfg.signal.set = true;
  auto extra = lua::create_lua_solver();
  std::array<core::ISolver *, 1> extras{extra.get()};
  world::State out;
  assert(solvers::run_headless(cfg, std::span<emit::ISink *>{}, out,
                               std::span(extras.data(), extras.size()))
             .ok());
  assert(out.signals.live_count == 1);
  assert(near(out.signals.value[0], 1.4, 1e-9));

  // Rejections: no script, no extra solver, cross-kind payloads.
  world::State sink_state;
  core::RunConfig bad = cfg;
  bad.signal.lua.clear();
  assert(!solvers::run_headless(bad, std::span<emit::ISink *>{}, sink_state,
                                std::span(extras.data(), extras.size()))
              .ok());
  assert(!solvers::run_headless(cfg, std::span<emit::ISink *>{}, sink_state).ok());
  bad = cfg;
  bad.signal.params.kind = signal::SignalKind::kSine;
  assert(!solvers::run_headless(bad, std::span<emit::ISink *>{}, sink_state,
                                std::span(extras.data(), extras.size()))
              .ok());
  bad = cfg;
  bad.signal.table = {{0.0, 1.0}, {1.0, 2.0}};
  assert(!solvers::run_headless(bad, std::span<emit::ISink *>{}, sink_state,
                                std::span(extras.data(), extras.size()))
              .ok());
  bad = cfg;
  bad.signal.params.expr = signal::parse_expression("t").value;
  assert(!solvers::run_headless(bad, std::span<emit::ISink *>{}, sink_state,
                                std::span(extras.data(), extras.size()))
              .ok());
  // Null extra entries are rejected, never dereferenced.
  std::array<core::ISolver *, 1> null_extra{nullptr};
  assert(!solvers::run_headless(cfg, std::span<emit::ISink *>{}, sink_state,
                                std::span(null_extra.data(), null_extra.size()))
              .ok());
}

void test_sum_over_lua() {
  // Sums over Lua operands read fresh values: the extra solver steps
  // before SignalSolver (extras-first contract).
  LuaRun r(0.1);
  const world::Entity le = spawn_lua(r.runner, "return 2*t");
  signal::SignalParams dc;
  dc.kind = signal::SignalKind::kDc;
  dc.offset = 3.0;
  world::Entity de = r.runner.state().spawn_entity();
  assert(world::configure_source(de, r.runner.state().signals, dc, 0.0).ok());
  signal::SignalParams sum;
  sum.kind = signal::SignalKind::kSum;
  sum.ops[0] = r.runner.state().signals.serial[le.index];
  sum.ops[1] = r.runner.state().signals.serial[de.index];
  sum.op_count = 2;
  world::Entity se = r.runner.state().spawn_entity();
  assert(world::configure_source(se, r.runner.state().signals, sum, 0.0).ok());
  assert(r.runner.run(2).ok()); // t = 0.2: 0.4 + 3.0
  assert(near(r.runner.state().signals.value[se.index], 3.4, 1e-9));
}

void test_configure_rejections() {
  LuaRun r(0.01);
  const std::size_t live_before = r.runner.state().signals.live_count;
  world::Entity e = r.runner.state().spawn_entity();
  assert(!lua::configure_lua_source(e, r.runner.state().signals, "",
                                    signal::QuantityId::kVoltage, 0.0)
              .ok());
  assert(!lua::configure_lua_source(e, r.runner.state().signals, "return 1+",
                                    signal::QuantityId::kVoltage, 0.0)
              .ok());
  const std::string big(signal::kMaxLuaScriptBytes + 1, ' ');
  assert(!lua::configure_lua_source(e, r.runner.state().signals,
                                    std::string_view(big.data(), big.size()),
                                    signal::QuantityId::kVoltage, 0.0)
              .ok());
  assert(r.runner.state().signals.live_count == live_before);
  // The params-only path rejects Lua (needs the runtime — see above).
  signal::SignalParams p;
  p.kind = signal::SignalKind::kLua;
  assert(!world::configure_source(e, r.runner.state().signals, p, 0.0).ok());
  assert(r.runner.state().signals.live_count == live_before);
}

void test_examples() {
  // Shipped examples stay runnable and keep their documented goldens.
  // Paths assume the ctest working directory (build/); a missing file
  // fails loudly rather than silently skipping.
  const char *names[] = {"sine", "chirp", "burst", "seed_detune", "softclip"};
  for (const char *name : names) {
    const std::string path =
        std::string("../examples/lua/") + name + ".lua";
    std::ifstream in(path);
    assert(static_cast<bool>(in));
    std::ostringstream buf;
    buf << in.rdbuf();
    const std::string text = buf.str();
    assert(!text.empty());
    assert(lua::check_script(std::string_view(text.data(), text.size())).ok());
    LuaRun r(0.001);
    spawn_lua(r.runner, std::string_view(text.data(), text.size()));
    assert(r.runner.run(10).ok());
    assert(std::isfinite(r.runner.state().signals.value[0]));
  }
  // Documented goldens: sine hits 2 at t = 0.25 s, chirp hits -1 at t = 0.5 s.
  {
    std::ifstream in("../examples/lua/sine.lua");
    std::ostringstream buf;
    buf << in.rdbuf();
    const std::string text = buf.str();
    LuaRun r(0.001);
    spawn_lua(r.runner, std::string_view(text.data(), text.size()));
    assert(r.runner.run(250).ok());
    assert(near(r.runner.state().signals.value[0], 2.0, 1e-9));
  }
  {
    std::ifstream in("../examples/lua/chirp.lua");
    std::ostringstream buf;
    buf << in.rdbuf();
    const std::string text = buf.str();
    LuaRun r(0.25);
    spawn_lua(r.runner, std::string_view(text.data(), text.size()));
    assert(r.runner.run(2).ok());
    assert(near(r.runner.state().signals.value[0], -1.0, 1e-9));
  }
}

void test_serialize_v5() {
  LuaRun r(0.1);
  spawn_lua(r.runner, "return 2*t+1", signal::QuantityId::kCurrent);
  assert(r.runner.run(3).ok());
  std::ostringstream bytes;
  assert(world::save_state(r.runner.state(), bytes).ok());
  world::State dst;
  std::istringstream in(bytes.str());
  assert(world::load_state(dst, in).ok());
  assert(dst == r.runner.state());
  assert(world::take_snapshot(dst) == world::take_snapshot(r.runner.state()));
  assert(dst.signals.lua_len[0] == dst.signals.lua_text.size());
  assert(dst.signals.lua_text == "return 2*t+1");

  // v4 blob fallback: one sine slot with expr/table columns but no Lua
  // columns or blob — loads with zeroed script ranges and empty text.
  std::ostringstream v4;
  v4.put('W');
  v4.put('E');
  v4.put('N');
  v4.put('G');
  world::detail::put_u32_le(v4, 4);
  world::detail::put_f64_le(v4, 0.5);
  world::detail::put_u64_le(v4, 7);
  world::detail::put_u32_le(v4, 0); // entities
  world::detail::put_u32_le(v4, 0); // freelist
  world::detail::put_u32_le(v4, 0); // particles
  world::detail::put_u32_le(v4, 1); // one signal slot
  world::detail::put_u8(v4, 1); // present
  world::detail::put_u8(v4, 0); // kind sine
  world::detail::put_u8(v4, 0); // quantity U
  world::detail::put_u8(v4, 1); // running
  world::detail::put_u8(v4, 0); // op_count
  world::detail::put_f64_le(v4, 1.0); // freq
  world::detail::put_f64_le(v4, 2.0); // amp
  world::detail::put_f64_le(v4, 0.0); // offset
  world::detail::put_f64_le(v4, 0.0); // phase
  world::detail::put_f64_le(v4, 0.5); // duty
  world::detail::put_f64_le(v4, 0.0); // t0
  world::detail::put_f64_le(v4, 0.25); // value
  world::detail::put_u64_le(v4, 1); // seed
  world::detail::put_u64_le(v4, 9); // rng
  world::detail::put_u64_le(v4, 5); // serial
  for (int k = 0; k < 8; ++k) {
    world::detail::put_u64_le(v4, 0); // ops
  }
  world::detail::put_u8(v4, 0); // expr_len
  for (int k = 0; k < 64; ++k) {
    world::detail::put_u8(v4, 0); // expr_op
  }
  for (int k = 0; k < 64; ++k) {
    world::detail::put_f64_le(v4, 0.0); // expr_arg
  }
  world::detail::put_u64_le(v4, 0); // table_off
  world::detail::put_u64_le(v4, 0); // table_len
  world::detail::put_u32_le(v4, 0); // empty table store
  world::State legacy;
  std::istringstream vin(v4.str());
  assert(world::load_state(legacy, vin).ok());
  assert(legacy.signals.live_count == 1);
  assert(legacy.signals.serial[0] == 5);
  assert(legacy.signals.value[0] == 0.25);
  assert(legacy.signals.next_serial == 6);
  assert(legacy.signals.lua_len[0] == 0);
  assert(legacy.signals.lua_text.empty());
}

void test_save_load_dead_lua() {
  // Despawned Lua sources retain their script range; save/load round-trips
  // exactly, slot reuse mints fresh serials over the shared blob, and a
  // corrupted dead range still fails the load.
  LuaRun r(0.1);
  const world::Entity e = spawn_lua(r.runner, "return 2*t+1"); // serial 0
  assert(r.runner.run(3).ok());
  assert(r.runner.state().despawn_entity(e));
  std::ostringstream bytes;
  assert(world::save_state(r.runner.state(), bytes).ok());
  world::State dst;
  std::istringstream in(bytes.str());
  assert(world::load_state(dst, in).ok());
  assert(dst == r.runner.state());
  assert(world::take_snapshot(dst) == world::take_snapshot(r.runner.state()));
  assert(dst.signals.next_serial == 1);

  // Reuse the dead slot with a new script: the blob holds both ranges and
  // the live source reads only its own.
  LuaRun r2(0.1);
  r2.runner.state() = dst;
  const world::Entity e2 = spawn_lua(r2.runner, "return 5*t");
  assert(r2.runner.state().signals.serial[e2.index] == 1);
  assert(r2.runner.run(2).ok());
  // Restored clock (t0 = 0.3 s): two 0.1 s steps -> t = 0.2 s -> 5*0.2.
  assert(near(r2.runner.state().signals.value[e2.index], 1.0, 1e-9));
  std::ostringstream bytes2;
  assert(world::save_state(r2.runner.state(), bytes2).ok());
  world::State dst2;
  std::istringstream in2(bytes2.str());
  assert(world::load_state(dst2, in2).ok());
  assert(dst2 == r2.runner.state());
  LuaRun r3(0.1);
  r3.runner.state() = dst2;
  assert(r2.runner.run(2).ok() && r3.runner.run(2).ok());
  assert(r2.runner.state() == r3.runner.state());

  // Corrupt dead script range behind configure's back: load must reject.
  {
    world::State tampered = r.runner.state();
    tampered.signals.lua_off[e.index] =
        static_cast<std::uint64_t>(tampered.signals.lua_text.size() + 64);
    std::ostringstream bad;
    assert(world::save_state(tampered, bad).ok());
    world::State rej;
    std::istringstream bin(bad.str());
    assert(!world::load_state(rej, bin).ok());
  }
}

double run_lua_once(std::string_view script, double dt = 0.01) {
  LuaRun r(dt);
  spawn_lua(r.runner, script);
  assert(r.runner.run(1).ok());
  return r.runner.state().signals.value[0];
}

void expect_lua_step_failure(std::string_view script) {
  LuaRun r(0.01);
  spawn_lua(r.runner, script);
  const std::uint64_t evals_before = r.lua->evals();
  const core::Status st = r.runner.run(1);
  assert(!st.ok() && st.code == core::StatusCode::kInvalidArgument);
  assert(r.runner.state().step_index == 0); // time/index frozen
  assert(!r.lua->last_error().empty());
  assert(r.lua->evals() == evals_before && r.lua->failures() == 1);
}

void test_pairs_deterministic_across_states() {
  // Review repro: {a=1e16, b=-1e16, c=1} summed via pairs. Stock iteration
  // visits string keys in per-state hash order (13x 0, 11x 1 over 24 fresh
  // CLI processes); sandbox order (byte order: a, b, c) sums to exactly 1
  // every time: 1e16 + -1e16 is exact, then +1.
  constexpr std::string_view kScript =
      "local t = {a=1e16, b=-1e16, c=1}\n"
      "local s = 0\n"
      "for _, v in pairs(t) do s = s + v end\n"
      "return s";
  for (int k = 0; k < 16; ++k) {
    // Fresh interpreter per iteration (fresh lua_State, fresh hash seed).
    assert(run_lua_once(kScript) == 1.0);
  }
}

void test_iteration_key_order() {
  // Total key order: booleans (false < true), numbers (ascending), strings
  // (byte order). The accumulator spells the visit order as decimal digits.
  constexpr std::string_view kMixed =
      "local t = {[-2]=1, [1.5]=2, [3]=3, a=4, b=5, [false]=6, [true]=7}\n"
      "local acc = 0\n"
      "for _, v in pairs(t) do acc = acc * 10 + v end\n"
      "return acc";
  assert(run_lua_once(kMixed) == 6712345.0); // 6,7,1,2,3,4,5

  // Byte order is case-sensitive with prefix handling: "B" < "a" < "b" < "bb".
  constexpr std::string_view kStrings =
      "local t = {b=1, a=2, bb=3, B=4}\n"
      "local acc = 0\n"
      "for _, v in pairs(t) do acc = acc * 10 + v end\n"
      "return acc";
  assert(run_lua_once(kStrings) == 4213.0);

  // Direct next() follows the same order, ends with nil, and pins the
  // stock tail protocol.
  constexpr std::string_view kNext =
      "local t = {[10]=1, [2]=2, [1]=3}\n"
      "local k1 = next(t)\n"
      "local k2 = next(t, k1)\n"
      "local k3 = next(t, k2)\n"
      "if next(t, k3) ~= nil then error('tail') end\n"
      "return k1 * 100 + k2 * 10 + k3";
  assert(run_lua_once(kNext) == 130.0); // 1, 2, 10
  constexpr std::string_view kNextEmpty =
      "if next({}) ~= nil then error('empty') end\n"
      "return 42";
  assert(run_lua_once(kNextEmpty) == 42.0);
  // Unknown keys fail like stock next (never a silent nil walk).
  expect_lua_step_failure("local t = {a=1}\nlocal k = next(t, 'b')\nreturn 1");
}

void test_iteration_rejects_identity_keys() {
  // Identity-based keys have no deterministic order: loud step failure
  // (time/index frozen), never hash-order iteration.
  expect_lua_step_failure("local t = {}\nt[{}] = 1\nfor _ in pairs(t) do end\nreturn 1");
  expect_lua_step_failure(
      "local t = {}\nt[function() end] = 1\nfor _ in pairs(t) do end\nreturn 1");
  expect_lua_step_failure("local t = {}\nt[{}] = 1\nlocal k = next(t)\nreturn 1");
  expect_lua_step_failure(
      "local t = {a=1}\nt[{}] = 2\nlocal s = 0\nfor _, v in pairs(t) do s = s + v end\nreturn s");
  expect_lua_step_failure("local t = {a=1}\nlocal k = next(t, {})\nreturn 1");
  // NaN is unorderable (NaN ~= NaN, so even lookup is meaningless).
  expect_lua_step_failure(
      "local z = 0/0\nlocal t = {}\nt[z] = 1\nfor _ in pairs(t) do end\nreturn 1");
}

void test_pairs_snapshot_mutation() {
  // pairs() snapshots keys at call time: added keys are never visited,
  // removed keys yield nil values. Both outcomes are exact here because
  // byte order visits "a" first deterministically.
  constexpr std::string_view kAdd =
      "local t = {a=1, b=2, c=3}\n"
      "local visits = 0\n"
      "local sum = 0\n"
      "for k, v in pairs(t) do\n"
      "  visits = visits + 1\n"
      "  if v ~= nil then sum = sum + v end\n"
      "  if k == 'a' then t.d = 100 end\n"
      "end\n"
      "return visits * 1000 + sum";
  assert(run_lua_once(kAdd) == 3006.0); // 3 visits, d unseen, 1+2+3
  constexpr std::string_view kRemove =
      "local t = {a=1, b=2, c=3}\n"
      "local visits = 0\n"
      "local nils = 0\n"
      "for k, v in pairs(t) do\n"
      "  visits = visits + 1\n"
      "  if v == nil then nils = nils + 1 end\n"
      "  if k == 'a' then t.b = nil end\n"
      "end\n"
      "return visits * 100 + nils";
  assert(run_lua_once(kRemove) == 301.0); // 3 visits, b reads nil
}

void test_iteration_save_load_continue() {
  // An iterating script is still a pure function of its inputs:
  // save/load/continue matches an uninterrupted run exactly.
  constexpr std::string_view kScript =
      "local t = {a=1e16, b=-1e16, c=1}\n"
      "local s = seed\n"
      "for _, v in pairs(t) do s = s + v end\n"
      "return s + step";
  LuaRun r(0.1);
  spawn_lua(r.runner, kScript);
  assert(r.runner.run(2).ok());
  std::ostringstream bytes;
  assert(world::save_state(r.runner.state(), bytes).ok());
  world::State loaded;
  std::istringstream in(bytes.str());
  assert(world::load_state(loaded, in).ok());
  assert(loaded == r.runner.state());
  LuaRun r2(0.1);
  r2.runner.state() = loaded;
  assert(r.runner.run(2).ok() && r2.runner.run(2).ok());
  assert(r2.runner.state() == r.runner.state());
}

} // namespace

int main() {
  test_pin();
  test_check_script();
  test_sandbox_rejections();
  test_allowed_surface();
  test_sine_golden();
  test_inputs();
  test_budget();
  test_determinism_and_purity();
  test_save_load_continue();
  test_headless_extras();
  test_sum_over_lua();
  test_configure_rejections();
  test_examples();
  test_serialize_v5();
  test_save_load_dead_lua();
  test_pairs_deterministic_across_states();
  test_iteration_key_order();
  test_iteration_rejects_identity_keys();
  test_pairs_snapshot_mutation();
  test_iteration_save_load_continue();
  std::cout << "test_lua: OK\n";
  return 0;
}
