// SIG-10 generic control plane: engine core tests (no sockets).
//
// Covers the text-line verb parser, per-verb apply semantics (add/set/
// start/stop/drop/list/hello/ping/select/table-lua refusals), idempotent
// start/stop, phase-exact pause/resume (sine continuity + noise-sequence
// continuity, incl. save/load while stopped), pool-untouched refusals,
// multi-channel fixed-order drain with last-writer-wins (order, not
// arrival timing), and replay-log exactness across a two-channel session.

#include "wengine/core/runner.hpp"
#include "wengine/signal/control.hpp"
#include "wengine/signal/types.hpp"
#include "wengine/solvers/signal.hpp"
#include "wengine/solvers/signal_control.hpp"
#include "wengine/world/serialize.hpp"
#include "wengine/world/state.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
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

struct Rig {
  core::Runner runner{{.dt = 0.01}};
  solvers::SignalSolver solver;
  solvers::ControlBus bus{7}; // fixed run_id for response assertions
  std::uint32_t local = 0;
  std::uint32_t aux = 0;

  Rig() {
    runner.add_solver(solver);
    local = bus.add_channel();
    aux = bus.add_channel();
  }

  solvers::DrainStats drain() {
    return bus.drain(runner.state(), runner.state().step_index);
  }

  void cmd(std::uint32_t ch, const std::string &line) {
    const bool ok = bus.enqueue(ch, line);
    assert(ok);
  }

  void local_cmd(const std::string &line) { cmd(local, line); }

  std::vector<std::string> responses(std::uint32_t ch) {
    return bus.take_responses(ch);
  }

  void step(std::uint64_t n = 1) {
    const core::Status st = runner.run(n);
    assert(st.ok());
  }

  [[nodiscard]] double value_of(std::uint64_t serial) const {
    const auto slot = world::find_serial(runner.state().signals, serial);
    assert(slot.has_value());
    return runner.state().signals.value[*slot];
  }
};

void test_parse_shapes() {
  using control = signal::control::Command;
  (void)sizeof(control);
  {
    const auto r = signal::control::parse_line("PING");
    assert(r.ok() && r.value.verb == signal::control::Verb::kPing);
  }
  {
    const auto r = signal::control::parse_line("PING x");
    assert(!r.ok());
  }
  {
    const auto r = signal::control::parse_line("HELLO 1 zephyr");
    assert(r.ok() && r.value.verb == signal::control::Verb::kHello);
    assert(r.value.hello_version == 1);
    assert(r.value.client == "zephyr");
  }
  {
    const auto r = signal::control::parse_line("HELLO x");
    assert(!r.ok());
  }
  {
    const auto r = signal::control::parse_line("LIST");
    assert(r.ok());
  }
  {
    const auto r = signal::control::parse_line("STOP 3");
    assert(r.ok() && r.value.serial == 3);
  }
  {
    const auto r = signal::control::parse_line("STOP x");
    assert(!r.ok());
  }
  {
    const auto r = signal::control::parse_line("SET 0 AMP 2.5");
    assert(r.ok() && r.value.serial == 0);
    assert(r.value.param == signal::control::Param::kAmp);
    assert(r.value.number == 2.5);
  }
  {
    const auto r = signal::control::parse_line("SET 0 QUANTITY I");
    assert(r.ok() && r.value.set_quantity == signal::QuantityId::kCurrent);
  }
  {
    const auto r = signal::control::parse_line("SET 0 BOGUS 1");
    assert(!r.ok());
  }
  {
    const auto r = signal::control::parse_line("SET 0 AMP nope");
    assert(!r.ok());
  }
  {
    const auto r = signal::control::parse_line("SET 0 SEED 9");
    assert(r.ok() && r.value.seed == 9);
  }
  {
    const auto r = signal::control::parse_line("ADD sine U FREQ 2 AMP 3");
    assert(r.ok() && r.value.kind == signal::SignalKind::kSine);
    assert(r.value.params.freq_hz == 2.0 && r.value.params.amp == 3.0);
  }
  {
    const auto r = signal::control::parse_line("ADD sum U OPS 0,1");
    assert(r.ok() && r.value.op_count == 2);
    assert(r.value.ops[0] == 0 && r.value.ops[1] == 1);
  }
  {
    const auto r = signal::control::parse_line("ADD sum U");
    assert(!r.ok()); // operands required
  }
  {
    const auto r = signal::control::parse_line("ADD sine U");
    assert(r.ok()); // defaults
  }
  {
    const auto r = signal::control::parse_line("ADD expression U EXPR sin(2*pi*440*t)+1");
    assert(r.ok() && r.value.has_expr);
    assert(r.value.expr == "sin(2*pi*440*t)+1");
  }
  {
    const auto r = signal::control::parse_line("ADD sine U EXPR sin(t)");
    assert(!r.ok()); // EXPR is expression-only
  }
  {
    const auto r = signal::control::parse_line("ADD bogus U");
    assert(!r.ok());
  }
  {
    const auto r = signal::control::parse_line("BOGUS");
    assert(!r.ok());
  }
  {
    const auto r = signal::control::parse_line("");
    assert(!r.ok());
  }
  {
    const auto r = signal::control::parse_line("SELECT 0 2");
    assert(r.ok() && r.value.serial == 0 && r.value.select_index == 2);
  }
  {
    // 9 operands exceed kMaxOps (8).
    const auto r = signal::control::parse_line("ADD sum U OPS 0,1,2,3,4,5,6,7,8");
    assert(!r.ok());
  }
}

void test_hello_ping_list() {
  Rig r;
  r.local_cmd("HELLO 1 zephyr");
  r.local_cmd("PING");
  r.local_cmd("LIST");
  const solvers::DrainStats st = r.drain();
  assert(st.applied == 0 && st.readonly == 3 && st.rejected == 0);
  assert(r.bus.log().empty()); // read-only verbs never log
  const std::vector<std::string> out = r.responses(r.local);
  assert(out.size() == 3);
  assert(out[0] == "OK hello 1 run=7");
  assert(out[1] == "OK pong");
  assert(out[2] == "OK list 0");
  // Wrong protocol version is a loud refusal, never a downgrade.
  r.local_cmd("HELLO 2 zephyr");
  const solvers::DrainStats st2 = r.drain();
  assert(st2.rejected == 1);
  const std::vector<std::string> out2 = r.responses(r.local);
  assert(out2.size() == 1 && out2[0].substr(0, 20) == "ERR invalid_argument");
}

void test_add_set_list_flow() {
  Rig r;
  r.local_cmd("ADD sine U FREQ 1 AMP 2");
  solvers::DrainStats st = r.drain();
  assert(st.applied == 1);
  std::vector<std::string> out = r.responses(r.local);
  assert(out.size() == 1 && out[0] == "OK added 0");
  r.local_cmd("LIST");
  r.drain();
  out = r.responses(r.local);
  assert(out.size() == 2 && out[0] == "OK list 1");
  assert(out[1].substr(0, 12) == "SRC 0 sine U");
  // Retune mid-run: SET FREQ 2 at T=0.1 keeps the time base (t0 untouched),
  // so one step later the sample is the 2 Hz closed form at t=0.11.
  r.step(10);
  r.local_cmd("SET 0 FREQ 2");
  st = r.drain();
  assert(st.applied == 1);
  out = r.responses(r.local);
  assert(out.size() == 1 && out[0] == "OK set 0 FREQ");
  r.step(1);
  const double want = 2.0 * std::sin(2.0 * 3.141592653589793 * 2.0 * 0.11);
  assert(near(r.value_of(0), want));
  // Quantity re-tag is display-only: the SI value is untouched.
  const double before = r.value_of(0);
  r.local_cmd("SET 0 QUANTITY I");
  r.drain();
  assert(near(r.value_of(0), before));
  r.local_cmd("LIST");
  r.drain();
  out = r.responses(r.local);
  assert(out.size() == 3); // OK set + OK list + SRC
  assert(out[2].substr(0, 12) == "SRC 0 sine I");
}

void test_refusals_leave_pool_untouched() {
  Rig r;
  r.local_cmd("ADD square U FREQ 1 DUTY 0.5");
  r.drain();
  r.responses(r.local);
  r.step(3);
  const double freq_before = r.runner.state().signals.freq[0];
  const double duty_before = r.runner.state().signals.duty[0];
  const std::size_t log_before = r.bus.log().size();
  // freq <= 0 and duty outside 0..1 fail validation for square.
  r.local_cmd("SET 0 FREQ 0");
  r.local_cmd("SET 0 DUTY 2");
  r.local_cmd("SET 0 FREQ nope");
  r.local_cmd("SET 9 AMP 1"); // unknown source
  r.local_cmd("BOGUS VERB");
  r.local_cmd("START"); // missing serial
  const solvers::DrainStats st = r.drain();
  assert(st.rejected == 6 && st.applied == 0);
  assert(r.runner.state().signals.freq[0] == freq_before);
  assert(r.runner.state().signals.duty[0] == duty_before);
  assert(r.bus.log().size() == log_before); // refusals never log
  const std::vector<std::string> out = r.responses(r.local);
  assert(out.size() == 6);
  for (const std::string &line : out) {
    assert(line.substr(0, 3) == "ERR");
  }
}

void test_stop_start_pause_resume_exact() {
  // Reference: 15 uninterrupted steps of 1 Hz sine at dt=0.01.
  Rig ref;
  ref.local_cmd("ADD sine U FREQ 1 AMP 2");
  ref.drain();
  ref.responses(ref.local);
  ref.step(15);
  const double ref_value = ref.value_of(0);

  Rig r;
  r.local_cmd("ADD sine U FREQ 1 AMP 2");
  r.drain();
  r.responses(r.local);
  r.step(10);
  const double frozen = r.value_of(0);
  // STOP at step 10 (idempotent: twice is still one freeze).
  r.local_cmd("STOP 0");
  r.local_cmd("STOP 0");
  solvers::DrainStats st = r.drain();
  assert(st.applied == 2);
  std::vector<std::string> out = r.responses(r.local);
  assert(out.size() == 2 && out[0] == "OK stopped 0" && out[1] == "OK stopped 0");
  r.step(5);
  assert(r.value_of(0) == frozen); // value frozen while stopped
  // START at step 15 resumes phase-exactly: 5 more live steps reach the
  // reference (15 live steps), not the wall-clock 20-step sample.
  r.local_cmd("START 0");
  st = r.drain();
  assert(st.applied == 1);
  r.step(5);
  assert(r.value_of(0) == ref_value); // 15 live steps, not 20 wall steps
  assert(r.runner.state().step_index == 20); // the sim clock kept running
}

void test_noise_freeze_and_seed_restart() {
  Rig r;
  r.local_cmd("ADD noise U AMP 1 SEED 42");
  r.drain();
  r.responses(r.local);
  r.step(4);
  const double v4 = r.value_of(0);
  r.local_cmd("STOP 0");
  r.drain();
  r.responses(r.local);
  r.step(3);
  assert(r.value_of(0) == v4); // RNG frozen: identical sample
  r.local_cmd("START 0");
  r.drain();
  r.responses(r.local);
  r.step(1);
  assert(r.value_of(0) != v4); // sequence continues after resume
  // SET SEED restarts the sequence deterministically: the next sample
  // equals the first post-spawn sample of a fresh seed-7 source.
  Rig fresh;
  fresh.local_cmd("ADD noise U AMP 1 SEED 7");
  fresh.drain();
  fresh.responses(fresh.local);
  fresh.step(1);
  const double fresh_first = fresh.value_of(0);
  r.local_cmd("SET 0 SEED 7");
  const solvers::DrainStats st = r.drain();
  assert(st.applied == 1);
  r.responses(r.local);
  r.step(1);
  assert(r.value_of(0) == fresh_first);
}

void test_sum_add_and_dangling_operand() {
  Rig r;
  r.local_cmd("ADD sine U FREQ 1 AMP 1");
  r.local_cmd("ADD dc U OFFSET 0.5");
  r.drain();
  r.responses(r.local);
  r.local_cmd("ADD sum U OPS 0,1");
  solvers::DrainStats st = r.drain();
  assert(st.applied == 1);
  std::vector<std::string> out = r.responses(r.local);
  assert(out.size() == 1 && out[0] == "OK added 2");
  r.step(3);
  assert(near(r.value_of(2), r.value_of(0) + r.value_of(1)));
  // Unknown operand serials and sums-of-sums are refused pre-spawn.
  r.local_cmd("ADD sum U OPS 0,99");
  r.local_cmd("ADD sum U OPS 2");
  st = r.drain();
  assert(st.rejected == 2 && st.applied == 0);
  assert(r.runner.state().signals.live_count == 3); // no half-spawned slot
  // Dropping an operand fails the next step loudly (never a stale read).
  r.local_cmd("DROP 0");
  st = r.drain();
  assert(st.applied == 1);
  r.responses(r.local);
  const core::Status bad = r.runner.run(1);
  assert(!bad.ok() && bad.code == core::StatusCode::kInvalidArgument);
  // Dropping twice addresses a gone source loudly.
  r.local_cmd("DROP 0");
  st = r.drain();
  assert(st.rejected == 1);
  // Serials never recycle: the next ADD mints a fresh serial.
  r.local_cmd("ADD dc U OFFSET 1");
  r.drain();
  out = r.responses(r.local);
  assert(out.size() == 2);
  assert(out[0].substr(0, 3) == "ERR");
  assert(out[1] == "OK added 3");
}

void test_reserved_verbs() {
  Rig r;
  r.local_cmd("ADD sine U");
  r.drain();
  r.responses(r.local);
  r.local_cmd("SELECT 0 1");
  r.local_cmd("ADD table U");
  r.local_cmd("ADD lua U");
  const solvers::DrainStats st = r.drain();
  assert(st.applied == 0 && st.rejected == 3);
  const std::vector<std::string> out = r.responses(r.local);
  assert(out.size() == 3);
  for (const std::string &line : out) {
    assert(line.substr(0, 19) == "ERR not_implemented");
  }
  assert(r.bus.log().size() == 1); // only the ADD logged; refusals never do
}

void test_channel_order_beats_arrival_timing() {
  Rig r;
  r.local_cmd("ADD sine U FREQ 1 AMP 1");
  r.drain();
  r.responses(r.local);
  // Same step, conflicting SETs on two channels: the later channel wins,
  // however the lines arrived (enqueue aux first, local second).
  r.cmd(r.aux, "SET 0 AMP 2");
  r.cmd(r.local, "SET 0 AMP 5");
  solvers::DrainStats st = r.drain();
  assert(st.applied == 2);
  // Drain order is registration order (local=0 before aux=1): aux wins.
  assert(r.runner.state().signals.amp[0] == 2.0);
  // Same channel overwrites in FIFO order (last line wins).
  r.cmd(r.local, "SET 0 AMP 7");
  r.cmd(r.local, "SET 0 AMP 9");
  r.drain();
  assert(r.runner.state().signals.amp[0] == 9.0);
  // Idempotent verbs commute: STOP from both channels is two OKs, one freeze.
  r.cmd(r.local, "STOP 0");
  r.cmd(r.aux, "STOP 0");
  st = r.drain();
  assert(st.applied == 2);
  // Conflicting verbs resolve by order, never timing: START(local) then
  // STOP(aux) in one drain leaves the source stopped.
  r.cmd(r.local, "START 0");
  r.cmd(r.aux, "STOP 0");
  r.drain();
  assert(r.runner.state().signals.running[0] == 0);
}

void test_replay_log_replays_two_channel_session() {
  // Session: two channels drive one sine across 20 steps with mid-run
  // retunes, a stop/start window, and a second source added mid-run.
  Rig a;
  a.cmd(a.local, "ADD sine U FREQ 1 AMP 1");
  a.drain();
  a.responses(a.local);
  a.responses(a.aux);
  for (std::uint64_t step = 0; step < 20; ++step) {
    if (step == 5) {
      a.cmd(a.aux, "SET 0 FREQ 2");
    }
    if (step == 8) {
      a.cmd(a.local, "STOP 0");
    }
    if (step == 10) {
      a.cmd(a.local, "ADD dc U OFFSET 0.25");
    }
    if (step == 12) {
      a.cmd(a.aux, "START 0");
      a.cmd(a.local, "SET 1 OFFSET 0.5");
    }
    a.drain();
    a.step(1);
  }
  const std::vector<solvers::AppliedCommand> log = a.bus.log();
  assert(!log.empty());
  // Replay: fresh rig, same channel count, same per-step lines in log order.
  Rig b;
  std::size_t cursor = 0;
  for (std::uint64_t step = 0; step < 20; ++step) {
    while (cursor < log.size() && log[cursor].step == step) {
      const bool ok = b.bus.enqueue(log[cursor].channel, log[cursor].line);
      assert(ok);
      ++cursor;
    }
    const solvers::DrainStats st = b.drain();
    (void)st;
    b.step(1);
  }
  assert(cursor == log.size());
  assert(b.runner.state() == a.runner.state());
  assert(b.bus.log().size() == log.size());
}

void test_stopped_save_load_resume() {
  Rig r;
  r.local_cmd("ADD sine U FREQ 1 AMP 2");
  r.drain();
  r.responses(r.local);
  r.step(10);
  r.local_cmd("STOP 0");
  r.drain();
  r.responses(r.local);
  r.step(2);
  // Checkpoint while stopped, restore elsewhere, resume: phase-exact.
  std::ostringstream blob;
  assert(world::save_state(r.runner.state(), blob).ok());
  world::State restored;
  std::istringstream in(blob.str());
  assert(world::load_state(restored, in).ok());
  assert(restored == r.runner.state());
  // Resume the restored copy and the original identically.
  Rig r2;
  r2.runner.state() = restored;
  for (Rig *rp : {&r, &r2}) {
    const bool ok = rp->bus.enqueue(rp->local, "START 0");
    assert(ok);
    rp->drain();
    rp->responses(rp->local);
    rp->step(7);
  }
  assert(r.runner.state() == r2.runner.state());
  assert(r.runner.state().signals.running[0] == 1);
}

void test_sum_quantity_reconfig() {
  // Control-plane boundary: mixed-quantity ADDs and quantity retags that
  // would mix a live sum fail loudly with the pool and replay log untouched.
  // Quantity text derives from kQuantities (no per-quantity switch).
  assert(signal::kQuantityCount >= 2);
  const signal::QuantityId q0 = signal::kQuantities[0].id;
  const signal::QuantityId q1 = signal::kQuantities[1].id;
  const std::string s0(signal::quantity_suffix(q0));
  const std::string s1(signal::quantity_suffix(q1));
  Rig r;
  r.local_cmd("ADD dc " + s0 + " OFFSET 1");
  r.local_cmd("ADD dc " + s1 + " OFFSET 2");
  r.local_cmd("ADD dc " + s0 + " OFFSET 4");
  solvers::DrainStats st = r.drain();
  assert(st.applied == 3);
  r.responses(r.local);

  const std::size_t live_before = r.runner.state().signals.live_count;
  const std::size_t log_before = r.bus.log().size();
  // Mixed operands fail for either output tag; same operands with a
  // mismatched output tag fail too.
  r.local_cmd("ADD sum " + s0 + " OPS 0,1");
  r.local_cmd("ADD sum " + s1 + " OPS 0,1");
  r.local_cmd("ADD sum " + s1 + " OPS 0,2");
  st = r.drain();
  assert(st.rejected == 3 && st.applied == 0);
  assert(r.runner.state().signals.live_count == live_before);
  assert(r.bus.log().size() == log_before);
  r.responses(r.local);

  // Same-dimension sum still applies and accumulates exactly.
  r.local_cmd("ADD sum " + s0 + " OPS 0,2");
  st = r.drain();
  assert(st.applied == 1);
  std::vector<std::string> out = r.responses(r.local);
  assert(out.size() == 1 && out[0] == "OK added 3");
  r.step(2);
  assert(r.value_of(3) == 1.0 + 4.0);

  // Reconfiguration cannot bypass the rule: retagging the sum or any of
  // its operands to the other dimension fails with pool/log untouched.
  const std::size_t log_mid = r.bus.log().size();
  r.local_cmd("SET 3 QUANTITY " + s1); // sum retag vs U operands
  r.local_cmd("SET 0 QUANTITY " + s1); // operand retag vs U sum
  st = r.drain();
  assert(st.rejected == 2 && st.applied == 0);
  assert(r.bus.log().size() == log_mid);
  assert(static_cast<signal::QuantityId>(r.runner.state().signals.quantity[3]) == q0);
  assert(static_cast<signal::QuantityId>(r.runner.state().signals.quantity[0]) == q0);
  out = r.responses(r.local);
  assert(out.size() == 2 && out[0].substr(0, 3) == "ERR" && out[1].substr(0, 3) == "ERR");

  // A retag with no dependent sum still applies (existing display-only rule).
  r.local_cmd("SET 1 QUANTITY " + s0);
  st = r.drain();
  assert(st.applied == 1);
  assert(static_cast<signal::QuantityId>(r.runner.state().signals.quantity[1]) == q0);
  assert(near(r.value_of(1), 2.0)); // SI value untouched by the retag
}

} // namespace

int main() {
  test_parse_shapes();
  test_hello_ping_list();
  test_add_set_list_flow();
  test_refusals_leave_pool_untouched();
  test_stop_start_pause_resume_exact();
  test_noise_freeze_and_seed_restart();
  test_sum_add_and_dangling_operand();
  test_sum_quantity_reconfig();
  test_reserved_verbs();
  test_channel_order_beats_arrival_timing();
  test_replay_log_replays_two_channel_session();
  test_stopped_save_load_resume();
  std::cout << "test_signal_control: OK\n";
  return 0;
}
