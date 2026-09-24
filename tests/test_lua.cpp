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
  std::cout << "test_lua: OK\n";
  return 0;
}
