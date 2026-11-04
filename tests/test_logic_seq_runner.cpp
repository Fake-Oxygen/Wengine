#include "wengine/core/runner.hpp"
#include "wengine/emit/logic_sink.hpp"
#include "wengine/logic/clock_domains.hpp"
#include "wengine/logic/netlist.hpp"
#include "wengine/logic/seq_state.hpp"
#include "wengine/logic/seq_step.hpp"
#include "wengine/logic/types.hpp"
#include "wengine/solvers/configured_run.hpp"
#include "wengine/solvers/run.hpp"
#include "wengine/solvers/sequential.hpp"
#include "wengine/world/state.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <sstream>
#include <vector>

using namespace wengine;
using wengine::logic::GateKind;
using wengine::logic::LogicValue;
using wengine::logic::Netlist;

namespace {

constexpr LogicValue kL = LogicValue::kLow;
constexpr LogicValue kH = LogicValue::kHigh;

void ignore_input(Netlist &nl, std::uint16_t slot, std::uint8_t input) {
  logic::PinRef pin{};
  pin.slot = slot;
  pin.is_output = false;
  pin.input = input;
  assert(logic::ignore_pin(nl, pin).ok());
}

// 2-bit binary counter (Q1=MSB, Q0=LSB), zero primary inputs:
//   D0 = NOT Q0, D1 = Q1 XOR Q0. CLKs ignored. No block I/O.
Netlist make_counter() {
  Netlist nl;
  const std::uint16_t q0 = logic::add_gate(nl, GateKind::kDff).value;
  const std::uint16_t q1 = logic::add_gate(nl, GateKind::kDff).value;
  const std::uint16_t n = logic::add_gate(nl, GateKind::kNot, 1).value;
  const std::uint16_t x = logic::add_gate(nl, GateKind::kXor, 2).value;
  assert(logic::wire(nl, n, q0, 0).ok()); // NOT.Y -> Q0.D (flop D first)
  assert(logic::wire(nl, q0, n, 0).ok()); // Q0 -> NOT.1
  assert(logic::wire(nl, x, q1, 0).ok()); // XOR.Y -> Q1.D
  assert(logic::wire(nl, q0, x, 0).ok()); // Q0 -> XOR.1
  assert(logic::wire(nl, q1, x, 1).ok()); // Q1 -> XOR.2
  ignore_input(nl, q0, 1);
  ignore_input(nl, q1, 1);
  assert(logic::validate_sequential(nl).ok());
  assert(logic::block_input_count(nl) == 0);
  assert(logic::block_output_count(nl) == 0);
  return nl;
}

// Single T flop divider: T driven High toggles every edge (divide by 2).
// T dangling (1 block input), CLK ignored, Q dangling (1 block output).
Netlist make_tdivider() {
  Netlist nl;
  const std::uint16_t t = logic::add_gate(nl, GateKind::kT).value;
  ignore_input(nl, t, 1); // CLK ignored (edge is the call)
  assert(logic::validate_sequential(nl).ok());
  assert(logic::block_input_count(nl) == 1);
  assert(logic::block_output_count(nl) == 1);
  return nl;
}

// Cascaded T divider (divide by 4): T0 driven High, Q0 -> T1.T.
Netlist make_tdiv4() {
  Netlist nl;
  const std::uint16_t t0 = logic::add_gate(nl, GateKind::kT).value;
  const std::uint16_t t1 = logic::add_gate(nl, GateKind::kT).value;
  assert(logic::wire(nl, t0, t1, 0).ok()); // Q0 -> T1.T (flop D first)
  ignore_input(nl, t0, 1);
  ignore_input(nl, t1, 1);
  assert(logic::validate_sequential(nl).ok());
  assert(logic::block_input_count(nl) == 1);  // T0.T (SER)
  assert(logic::block_output_count(nl) == 1); // Q1 (Q0 is wired)
  return nl;
}

// Latch-only net: one DLATCH, D + EN dangling (2 block inputs, 1 output).
Netlist make_latch() {
  Netlist nl;
  const std::uint16_t l = logic::add_gate(nl, GateKind::kDlatch).value;
  assert(logic::validate_sequential(nl).ok());
  assert(logic::block_input_count(nl) == 2);
  assert(logic::block_output_count(nl) == 1);
  (void)l;
  return nl;
}

void populate_seq_pool(world::State &s, const Netlist &nl, std::vector<LogicValue> stimulus) {
  s.netlist = nl;
  logic::init_seq_state(s.seq, s.netlist);
  s.seq_inputs = std::move(stimulus);
  s.seq_members.clear();
  s.seq_outputs.assign(logic::block_output_count(s.netlist), kL);
}

// --- T009: solver engine goldens (SequentialSolver via Runner) ---

void test_solver_counter_4steps() {
  // Hand-derived: (Q1,Q0) 00 -> 01 -> 10 -> 11 -> 00 over 4 fixed steps.
  world::State s;
  populate_seq_pool(s, make_counter(), {});
  solvers::SequentialSolver solver;
  core::Runner runner{{.dt = 0.01}};
  runner.add_solver(solver);
  // Move the pool into the runner state (Runner owns its State).
  runner.state().netlist = s.netlist;
  runner.state().seq = s.seq;
  runner.state().seq_inputs = s.seq_inputs;
  runner.state().seq_members = s.seq_members;
  runner.state().seq_outputs = s.seq_outputs;
  const LogicValue want[5][2] = {{kL, kL}, {kL, kH}, {kH, kL}, {kH, kH}, {kL, kL}};
  for (int step = 1; step <= 4; ++step) {
    assert(runner.run(1).ok());
    assert(runner.state().step_index == static_cast<std::uint64_t>(step));
    assert(runner.state().seq.q[1] == want[step][0]);
    assert(runner.state().seq.q[0] == want[step][1]);
  }
  const world::Snapshot snap = world::take_snapshot(runner.state());
  assert(snap.seq_serial.size() == 2);
  assert(snap.seq_value.size() == 2);
  assert(snap.seq_output.empty());
  // Slot order: DFF1 (slot 0) then DFF2 (slot 1); both back to Low.
  assert(snap.seq_value[0] == 0 && snap.seq_value[1] == 0);
  assert(snap.seq_serial[0] == 0 && snap.seq_serial[1] == 1);
}

void test_solver_tdivider() {
  // Divide by 2: T=High every step toggles Q: 0,1,0,1 over 4 steps.
  world::State s;
  populate_seq_pool(s, make_tdivider(), {kH});
  solvers::SequentialSolver solver;
  core::Runner runner{{.dt = 0.01}};
  runner.add_solver(solver);
  runner.state().netlist = s.netlist;
  runner.state().seq = s.seq;
  runner.state().seq_inputs = s.seq_inputs;
  runner.state().seq_members = s.seq_members;
  runner.state().seq_outputs = s.seq_outputs;
  const LogicValue want_q[4] = {kH, kL, kH, kL};
  const LogicValue want_out[4] = {kH, kL, kH, kL};
  for (int step = 0; step < 4; ++step) {
    assert(runner.run(1).ok());
    assert(runner.state().seq.q[0] == want_q[step]);
    assert(runner.state().seq_outputs[0] == want_out[step]);
  }
  // T=Low holds: two more steps keep Q at Low.
  runner.state().seq_inputs[0] = kL;
  assert(runner.run(1).ok());
  assert(runner.state().seq.q[0] == kL);
  assert(runner.run(1).ok());
  assert(runner.state().seq.q[0] == kL);
}

void test_solver_tdiv4() {
  // Divide by 4 cascade: Q1 reads 0,1,1,0 over 4 steps, back to (0,0).
  world::State s;
  populate_seq_pool(s, make_tdiv4(), {kH});
  solvers::SequentialSolver solver;
  core::Runner runner{{.dt = 0.01}};
  runner.add_solver(solver);
  runner.state().netlist = s.netlist;
  runner.state().seq = s.seq;
  runner.state().seq_inputs = s.seq_inputs;
  runner.state().seq_members = s.seq_members;
  runner.state().seq_outputs = s.seq_outputs;
  const LogicValue want_q1[4] = {kL, kH, kH, kL};
  for (int step = 0; step < 4; ++step) {
    assert(runner.run(1).ok());
    assert(runner.state().seq.q[1] == want_q1[step]);
    assert(runner.state().seq_outputs[0] == want_q1[step]);
  }
  assert(runner.state().seq.q[0] == kL && runner.state().seq.q[1] == kL);
}

void test_solver_latch_hold() {
  // Latch-only net: transparent while EN=High, frozen while EN=Low,
  // across fixed steps with no edge flop present.
  world::State s;
  populate_seq_pool(s, make_latch(), {kL, kL});
  solvers::SequentialSolver solver;
  core::Runner runner{{.dt = 0.01}};
  runner.add_solver(solver);
  runner.state().netlist = s.netlist;
  runner.state().seq = s.seq;
  runner.state().seq_inputs = s.seq_inputs;
  runner.state().seq_members = s.seq_members;
  runner.state().seq_outputs = s.seq_outputs;
  auto drive = [&](LogicValue d, LogicValue en) {
    runner.state().seq_inputs[0] = d;
    runner.state().seq_inputs[1] = en;
    assert(runner.run(1).ok());
  };
  drive(kH, kL); // frozen: D moves, Q holds 0
  assert(runner.state().seq.q[0] == kL);
  drive(kH, kH); // transparent: follows D to 1
  assert(runner.state().seq.q[0] == kH);
  assert(runner.state().seq_outputs[0] == kH);
  drive(kL, kL); // frozen: holds 1 across D change
  assert(runner.state().seq.q[0] == kH);
  drive(kL, kL); // still frozen
  assert(runner.state().seq.q[0] == kH);
  assert(runner.state().step_index == 4);
}

void test_solver_bad_dt_rejected() {
  world::State s;
  populate_seq_pool(s, make_counter(), {});
  solvers::SequentialSolver solver;
  const double inf = std::numeric_limits<double>::infinity();
  const double nan = std::numeric_limits<double>::quiet_NaN();
  assert(!solver.step(s, 0.0).ok());
  assert(!solver.step(s, -0.01).ok());
  assert(!solver.step(s, inf).ok());
  assert(!solver.step(s, -inf).ok());
  assert(!solver.step(s, nan).ok());
  // Rejected steps mutate nothing: pool still power-on, clocks untouched.
  assert(s.seq.q[0] == kL && s.seq.q[1] == kL);
  assert(s.sim_time == 0.0 && s.step_index == 0);
}

void test_solver_determinism_replay() {
  // Identical runs from the same starting net produce identical snapshots.
  auto run_once = [] {
    world::State s;
    populate_seq_pool(s, make_counter(), {});
    solvers::SequentialSolver solver;
    core::Runner runner{{.dt = 0.01}};
    runner.add_solver(solver);
    runner.state().netlist = s.netlist;
    runner.state().seq = s.seq;
    runner.state().seq_inputs = s.seq_inputs;
    runner.state().seq_members = s.seq_members;
    runner.state().seq_outputs = s.seq_outputs;
    assert(runner.run(10).ok());
    return world::take_snapshot(runner.state());
  };
  const world::Snapshot a = run_once();
  const world::Snapshot b = run_once();
  assert(a == b);
  assert(a.seq_serial.size() == 2 && a.seq_value.size() == 2);
}

// --- T010: driver goldens (run_headless with the logic section) ---

core::RunConfig make_logic_config(const Netlist &nl, std::vector<LogicValue> stimulus,
                                  std::uint64_t steps) {
  core::RunConfig cfg;
  cfg.loop.dt = 0.01;
  cfg.loop.emit_every = 1;
  cfg.steps = steps;
  cfg.solver = "logic";
  cfg.particles = 0;
  cfg.seq.set = true;
  cfg.seq.netlist = nl;
  cfg.seq.stimulus = std::move(stimulus);
  cfg.seq.step_all_domains = true;
  return cfg;
}

void test_driver_counter_sequence() {
  core::RunConfig cfg = make_logic_config(make_counter(), {}, 4);
  world::State out;
  assert(solvers::run_headless(cfg, std::span<emit::ISink *>{}, out).ok());
  assert(out.step_index == 4);
  assert(out.seq.q[0] == kL && out.seq.q[1] == kL); // 00 after 4 steps
  const world::Snapshot snap = world::take_snapshot(out);
  assert(snap.seq_serial.size() == 2 && snap.seq_value.size() == 2);
  assert(snap.seq_value[0] == 0 && snap.seq_value[1] == 0);
}

void test_driver_identical_replay() {
  core::RunConfig cfg = make_logic_config(make_tdiv4(), {kH}, 10);
  world::State a;
  assert(solvers::run_headless(cfg, std::span<emit::ISink *>{}, a).ok());
  world::State b;
  assert(solvers::run_headless(cfg, std::span<emit::ISink *>{}, b).ok());
  assert(a == b);
  assert(world::take_snapshot(a) == world::take_snapshot(b));
}

void test_driver_bad_config_rejected() {
  world::State out;
  const std::span<emit::ISink *> none{};
  {
    // steps < 1 rejected.
    core::RunConfig cfg = make_logic_config(make_counter(), {}, 0);
    assert(!solvers::run_headless(cfg, none, out).ok());
  }
  {
    // Bad dt rejected through the shared edge.
    core::RunConfig cfg = make_logic_config(make_counter(), {}, 4);
    cfg.loop.dt = 0.0;
    assert(!solvers::run_headless(cfg, none, out).ok());
    cfg.loop.dt = std::numeric_limits<double>::quiet_NaN();
    assert(!solvers::run_headless(cfg, none, out).ok());
  }
  {
    // Section set with a non-logic solver rejected.
    core::RunConfig cfg = make_logic_config(make_counter(), {}, 4);
    cfg.solver = "signal";
    assert(!solvers::run_headless(cfg, none, out).ok());
  }
  {
    // Logic solver without the section rejected (no defaults run).
    core::RunConfig cfg;
    cfg.loop.dt = 0.01;
    cfg.steps = 4;
    cfg.solver = "logic";
    cfg.particles = 0;
    assert(!solvers::run_headless(cfg, none, out).ok());
  }
  {
    // Stimulus size mismatch rejected.
    core::RunConfig cfg = make_logic_config(make_tdivider(), {}, 2);
    assert(!solvers::run_headless(cfg, none, out).ok());
  }
  {
    // Non-binary stimulus rejected.
    core::RunConfig cfg = make_logic_config(make_tdivider(), {LogicValue::kHighZ}, 2);
    assert(!solvers::run_headless(cfg, none, out).ok());
  }
  assert(out.step_index == 0); // rejected before stepping
}

void test_driver_empty_net_run() {
  // Empty sequential net (no gates): vacuous run succeeds, clocks advance.
  core::RunConfig cfg;
  cfg.loop.dt = 0.01;
  cfg.steps = 3;
  cfg.solver = "logic";
  cfg.particles = 0;
  cfg.seq.set = true;
  world::State out;
  assert(solvers::run_headless(cfg, std::span<emit::ISink *>{}, out).ok());
  assert(out.step_index == 3);
  const world::Snapshot snap = world::take_snapshot(out);
  assert(snap.seq_serial.empty() && snap.seq_value.empty() && snap.seq_output.empty());
}

void test_driver_section_inits_power_on() {
  // Section power-on values seed the pool (006): the T divider driven
  // High toggles from its seeded value, not from all-Low.
  core::RunConfig cfg = make_logic_config(make_tdivider(), {kH}, 1);
  cfg.seq.init_q[0] = kH; // T1 @ slot 0 powers on High
  world::State out;
  assert(solvers::run_headless(cfg, std::span<emit::ISink *>{}, out).ok());
  assert(out.step_index == 1);
  assert(out.seq.q[0] == kL); // 1 -> toggle -> 0 after one edge
}

void test_driver_default_inits_low() {
  // Direct-constructed sections keep the legacy all-Low fresh power-on.
  core::RunConfig cfg = make_logic_config(make_tdivider(), {kH}, 1);
  world::State out;
  assert(solvers::run_headless(cfg, std::span<emit::ISink *>{}, out).ok());
  assert(out.step_index == 1);
  assert(out.seq.q[0] == kH); // 0 -> toggle -> 1 after one edge
}

// --- T015: selection goldens (multi-clock member subsets) ---

// Two independent T flops (dangling CLKs form the implicit domain each) plus
// a BUF observing Q1: T0.T + T1.T are the 2 block inputs (slot order),
// Q0 + BUF.Y are the 2 block outputs. Selecting only T0's slot must
// advance T0 while T1 holds and the settled BUF output tracks the held Q.
Netlist make_two_domain() {
  Netlist nl;
  const std::uint16_t t0 = logic::add_gate(nl, GateKind::kT).value;
  const std::uint16_t t1 = logic::add_gate(nl, GateKind::kT).value;
  const std::uint16_t b = logic::add_gate(nl, GateKind::kBuf, 1).value;
  assert(logic::wire(nl, t1, b, 0).ok()); // Q1 -> BUF.1 (flop D first: sole wire)
  ignore_input(nl, t0, 1);
  ignore_input(nl, t1, 1);
  assert(logic::validate_sequential(nl).ok());
  assert(logic::block_input_count(nl) == 2);
  assert(logic::block_output_count(nl) == 2);
  return nl;
}

core::RunConfig make_select_config(const Netlist &nl, std::vector<LogicValue> stimulus,
                                   std::vector<std::uint16_t> members, bool step_all,
                                   std::uint64_t steps) {
  core::RunConfig cfg;
  cfg.loop.dt = 0.01;
  cfg.loop.emit_every = 1;
  cfg.steps = steps;
  cfg.solver = "logic";
  cfg.particles = 0;
  cfg.seq.set = true;
  cfg.seq.netlist = nl;
  cfg.seq.stimulus = std::move(stimulus);
  cfg.seq.members = std::move(members);
  cfg.seq.step_all_domains = step_all;
  return cfg;
}

void test_selection_selective_advance_hold() {
  // Select only T0 (slot 0): 3 steps with T0=T1=High. T0 toggles 3x
  // (0->1->0->1), T1 holds 0, BUF settles around the held Q (stays 0).
  Netlist nl = make_two_domain();
  core::RunConfig cfg = make_select_config(nl, {kH, kH}, {0}, false, 3);
  world::State out;
  assert(solvers::run_headless(cfg, std::span<emit::ISink *>{}, out).ok());
  assert(out.step_index == 3);
  assert(out.seq.q[0] == kH); // advanced 3 times
  assert(out.seq.q[1] == kL); // held at power-on
  assert(out.seq_outputs.size() == 2);
  assert(out.seq_outputs[0] == kH); // Q0 output follows the advanced flop
  assert(out.seq_outputs[1] == kL); // BUF.Y settles around the held flop
  const world::Snapshot snap = world::take_snapshot(out);
  assert(snap.seq_serial.size() == 2 && snap.seq_value.size() == 2);
  assert(snap.seq_value[0] == 1 && snap.seq_value[1] == 0);
  // Members retained into the pool for the run.
  solvers::ConfiguredRun run;
  assert(solvers::ConfiguredRun::build(
             cfg, std::span<core::ISolver *>{}, false, run)
             .ok());
  assert(run.state().seq_members.size() == 1 && run.state().seq_members[0] == 0);
  assert(run.run(3).ok());
  assert(run.state() == out);
}

void test_selection_all_domains_advance() {
  // Same net, default all-domains path: both flops toggle 3x (both High),
  // BUF.Y follows Q1 High.
  Netlist nl = make_two_domain();
  core::RunConfig cfg = make_select_config(nl, {kH, kH}, {}, true, 3);
  world::State out;
  assert(solvers::run_headless(cfg, std::span<emit::ISink *>{}, out).ok());
  assert(out.step_index == 3);
  assert(out.seq.q[0] == kH && out.seq.q[1] == kH);
  assert(out.seq_outputs.size() == 2);
  assert(out.seq_outputs[0] == kH && out.seq_outputs[1] == kH);
  assert(out.seq_members.empty()); // all-domains retains the empty list
}

void test_selection_dangling_clk_membership() {
  // Dangling-CLK flops (ignored CLK pins, the implicit domain) step when
  // selected: single T flop with ignored CLK, members={slot} advances;
  // the same net under all-domains advances identically.
  Netlist nl = make_tdivider();
  core::RunConfig sel = make_select_config(nl, {kH}, {0}, false, 2);
  world::State a;
  assert(solvers::run_headless(sel, std::span<emit::ISink *>{}, a).ok());
  core::RunConfig all = make_select_config(nl, {kH}, {}, true, 2);
  world::State b;
  assert(solvers::run_headless(all, std::span<emit::ISink *>{}, b).ok());
  assert(a.seq.q[0] == kL && b.seq.q[0] == kL); // toggled twice -> Low
  // Same observable evolution (pools differ only in the retained member
  // list: {0} vs all-domains empty).
  assert(world::take_snapshot(a) == world::take_snapshot(b));
  assert(a.seq_members.size() == 1 && b.seq_members.empty());
}

void test_selection_member_rejections_untouched() {
  // Every member-list violation fails before any step with state untouched.
  Netlist nl = make_two_domain();
  const std::uint16_t buf_slot = 2; // combinational BUF gate
  Netlist with_latch = make_latch(); // slot 0 is a DLATCH (level, no domain)
  world::State out;
  const std::span<emit::ISink *> none{};
  {
    // Empty selection with step_all=false.
    core::RunConfig cfg = make_select_config(nl, {kH, kH}, {}, false, 3);
    assert(!solvers::run_headless(cfg, none, out).ok());
  }
  {
    // Non-empty members with step_all=true.
    core::RunConfig cfg = make_select_config(nl, {kH, kH}, {0}, true, 3);
    assert(!solvers::run_headless(cfg, none, out).ok());
  }
  {
    // Dead slot.
    core::RunConfig cfg = make_select_config(nl, {kH, kH}, {7}, false, 3);
    assert(!solvers::run_headless(cfg, none, out).ok());
  }
  {
    // Combinational gate is not an edge flop.
    core::RunConfig cfg = make_select_config(nl, {kH, kH}, {buf_slot}, false, 3);
    assert(!solvers::run_headless(cfg, none, out).ok());
  }
  {
    // Level latch belongs to no domain.
    core::RunConfig cfg = make_select_config(with_latch, {kH, kH}, {0}, false, 3);
    assert(!solvers::run_headless(cfg, none, out).ok());
  }
  {
    // Duplicate slots.
    core::RunConfig cfg = make_select_config(nl, {kH, kH}, {0, 0}, false, 3);
    assert(!solvers::run_headless(cfg, none, out).ok());
  }
  assert(out.step_index == 0); // rejected before stepping
}

void test_derived_domain_selection_with_inits() {
  // Derived-domain selection composes with power-on inits (006 T018):
  // resolve the "BUF1.Y" domain by NAME through the engine derivation,
  // flatten its members, and run — the driven flop captures from its
  // seeded value while the implicit-domain flop holds its own seeded
  // value (a stepped DFF2 would have captured NOT(1)=0 instead).
  Netlist nl;
  assert(logic::add_gate(nl, GateKind::kBuf, 1).value == 0); // BUF1 @0
  assert(logic::add_gate(nl, GateKind::kDff).value == 1);     // DFF1 @1
  assert(logic::add_gate(nl, GateKind::kDff).value == 2);     // DFF2 @2
  assert(logic::add_gate(nl, GateKind::kNot, 1).value == 3);  // NOT1 @3
  assert(logic::add_gate(nl, GateKind::kNot, 1).value == 4);  // NOT2 @4
  assert(logic::wire(nl, 0, 1, 1).ok());                     // BUF1.Y -> DFF1.CLK
  assert(logic::wire(nl, 1, 3, 0).ok());                     // DFF1.Q -> NOT1.1
  assert(logic::wire(nl, 3, 1, 0).ok());                     // NOT1.Y -> DFF1.D
  assert(logic::wire(nl, 2, 4, 0).ok());                     // DFF2.Q -> NOT2.1
  assert(logic::wire(nl, 4, 2, 0).ok());                     // NOT2.Y -> DFF2.D
  assert(logic::validate_sequential(nl).ok());
  const std::vector<logic::ClockDomain> domains = logic::derive_clock_domains(nl);
  assert(domains.size() == 2);
  std::vector<std::uint16_t> members;
  for (const auto &d : domains) {
    if (d.name == "BUF1.Y") {
      members = d.members;
    }
  }
  assert(members.size() == 1 && members[0] == 1);
  core::RunConfig sel = make_select_config(nl, {kL, kL}, members, false, 1);
  sel.seq.init_q[1] = kL;
  sel.seq.init_q[2] = kH;
  world::State a;
  assert(solvers::run_headless(sel, std::span<emit::ISink *>{}, a).ok());
  assert(a.seq.q[1] == kH); // captured NOT(0)=1
  assert(a.seq.q[2] == kH); // held at seeded 1
  core::RunConfig all = make_select_config(nl, {kL, kL}, {}, true, 1);
  all.seq.init_q[1] = kL;
  all.seq.init_q[2] = kH;
  world::State b;
  assert(solvers::run_headless(all, std::span<emit::ISink *>{}, b).ok());
  assert(b.seq.q[1] == kH && b.seq.q[2] == kL); // both captured
}

// --- T021: observation goldens (snapshot / CSV / replay parity) ---

void test_observation_fill_snapshot_order_recycle() {
  // Slot order in, snapshot order out; serials stable across slot recycle
  // (removed flop retires its serial, the recycled slot mints a fresh one
  // that never aliases a live serial).
  Netlist nl;
  const std::uint16_t f0 = logic::add_gate(nl, GateKind::kDff).value;
  const std::uint16_t f1 = logic::add_gate(nl, GateKind::kDff).value;
  ignore_input(nl, f0, 0);
  ignore_input(nl, f0, 1);
  ignore_input(nl, f1, 0);
  ignore_input(nl, f1, 1);
  world::State s;
  s.netlist = nl;
  logic::init_seq_state(s.seq, s.netlist);
  s.seq_inputs.assign(2, kL);
  s.seq_outputs.assign(logic::block_output_count(s.netlist), kL);
  assert(s.seq.live_count == 2);
  const std::uint64_t first0 = s.seq.serial[0];
  const std::uint64_t first1 = s.seq.serial[1];
  assert(first0 == 0 && first1 == 1);
  world::Snapshot before = world::take_snapshot(s);
  assert(before.seq_serial.size() == 2);
  assert(before.seq_serial[0] == 0 && before.seq_serial[1] == 1);
  // Remove flop 1, resync, re-add: slot 1 recycles with a fresh serial.
  assert(logic::remove_gate(s.netlist, f1).ok());
  logic::init_seq_state(s.seq, s.netlist);
  assert(s.seq.live_count == 1 && s.seq.valid[0] != 0 && s.seq.valid[1] == 0);
  const std::uint16_t f2 = logic::add_gate(s.netlist, GateKind::kDff).value;
  assert(f2 == 1); // lowest free slot recycles
  ignore_input(s.netlist, f2, 0);
  ignore_input(s.netlist, f2, 1);
  logic::init_seq_state(s.seq, s.netlist);
  assert(s.seq.serial[0] == first0); // survivor keeps serial + Q
  assert(s.seq.serial[1] == 2);      // recycled slot mints a fresh serial
  s.seq_inputs.assign(logic::block_input_count(s.netlist), kL);
  s.seq_outputs.assign(logic::block_output_count(s.netlist), kL);
  world::Snapshot after = world::take_snapshot(s);
  assert(after.seq_serial.size() == 2);
  assert(after.seq_serial[0] == 0 && after.seq_serial[1] == 2);
  assert(after.seq_value.size() == 2); // slot order preserved, never sorted
}

void test_observation_take_snapshot_parity() {
  // fill_snapshot into a reused buffer is value-identical to take_snapshot.
  world::State s;
  populate_seq_pool(s, make_tdiv4(), {kH});
  solvers::SequentialSolver solver;
  core::Runner runner{{.dt = 0.01}};
  runner.add_solver(solver);
  runner.state().netlist = s.netlist;
  runner.state().seq = s.seq;
  runner.state().seq_inputs = s.seq_inputs;
  runner.state().seq_members = s.seq_members;
  runner.state().seq_outputs = s.seq_outputs;
  assert(runner.run(3).ok());
  world::Snapshot warm;
  world::fill_snapshot(runner.state(), warm);
  world::fill_snapshot(runner.state(), warm); // second fill reuses capacity
  const world::Snapshot owned = world::take_snapshot(runner.state());
  assert(warm == owned);
  assert(warm.seq_serial.size() == 2 && warm.seq_output.size() == 1);
}

struct RecSink : emit::ISink {
  std::vector<world::Snapshot> snaps;
  void on_snapshot(const world::Snapshot &s) override { snaps.push_back(s); }
};

void test_observation_csv_reread() {
  // CSV rows from a 3-step T-divider run re-read == on the recorded
  // columns (serials + Q in slot order, exact step/time).
  const std::string path = "/tmp/wengine_test_logic_seq.csv";
  std::remove(path.c_str());
  RecSink rec;
  {
    emit::LogicCsvSink csv(path);
    assert(csv.ok());
    core::RunConfig cfg = make_logic_config(make_tdivider(), {kH}, 3);
    std::array<emit::ISink *, 2> sinks{&csv, &rec};
    world::State out;
    assert(solvers::run_headless(cfg, std::span<emit::ISink *>(sinks.data(), sinks.size()), out).ok());
    assert(csv.flush().ok());
    assert(csv.rows_written() == 3); // 1 flop x 3 steps
  }
  assert(rec.snaps.size() == 3);
  std::ifstream in(path);
  std::vector<world::Snapshot> rows;
  std::string error;
  assert(emit::parse_logic_log(in, rows, error).ok());
  assert(rows.size() == rec.snaps.size());
  const std::uint8_t want_q[3] = {1, 0, 1}; // T=High toggles each step
  for (std::size_t k = 0; k < rows.size(); ++k) {
    assert(rows[k].step_index == rec.snaps[k].step_index);
    assert(rows[k].sim_time == rec.snaps[k].sim_time);
    assert(rows[k].seq_serial == rec.snaps[k].seq_serial);
    assert(rows[k].seq_value == rec.snaps[k].seq_value);
    assert(rows[k].seq_serial.size() == 1 && rows[k].seq_serial[0] == 0);
    assert(rows[k].seq_value.size() == 1 && rows[k].seq_value[0] == want_q[k]);
  }
  // Malformed logs refuse loudly with line numbers.
  {
    std::istringstream bad("step,time,seq_id,value\n1,0.01,0,2\n");
    std::vector<world::Snapshot> o;
    std::string e;
    assert(!emit::parse_logic_log(bad, o, e).ok());
  }
  {
    std::istringstream bad("step,time,seq_id,value\n1,0.01,0,1\n1,0.01,0,1\n");
    std::vector<world::Snapshot> o;
    std::string e;
    assert(!emit::parse_logic_log(bad, o, e).ok());
  }
  {
    std::istringstream bad("step,time,seq_id,VALUE\n");
    std::vector<world::Snapshot> o;
    std::string e;
    assert(!emit::parse_logic_log(bad, o, e).ok());
  }
  std::remove(path.c_str());
}

void test_observation_midrun_replay() {
  // Two identical recorded runs agree entry-for-entry, and the last
  // recorded entry matches the live readback.
  auto record = [] {
    RecSink rec;
    core::RunConfig cfg = make_logic_config(make_tdiv4(), {kH}, 4);
    std::array<emit::ISink *, 1> sinks{&rec};
    world::State out;
    assert(solvers::run_headless(cfg, std::span<emit::ISink *>(sinks.data(), sinks.size()), out).ok());
    assert(rec.snaps.size() == 4);
    assert(rec.snaps.back() == world::take_snapshot(out));
    return rec.snaps;
  };
  const std::vector<world::Snapshot> a = record();
  const std::vector<world::Snapshot> b = record();
  assert(a.size() == b.size());
  for (std::size_t k = 0; k < a.size(); ++k) {
    assert(a[k] == b[k]);
    assert(a[k].seq_serial.size() == 2 && a[k].seq_output.size() == 1);
  }
}

// --- T025: refusal/control goldens (legality + priority parity) ---

// Toggling T flop driving an SR latch: S follows Q0, R is held High, so
// step 2 resolves S=R=1 and must abort mid-run (step index frozen, prior
// entries intact, pool untouched at the failing boundary).
Netlist make_midrun_illegal() {
  Netlist nl;
  const std::uint16_t t = logic::add_gate(nl, GateKind::kT).value;
  const std::uint16_t sr = logic::add_gate(nl, GateKind::kSrlatch, 2).value;
  assert(logic::wire(nl, t, sr, 0).ok()); // Q0 -> SR.S
  ignore_input(nl, t, 1);                 // T CLK ignored (edge is the call)
  assert(logic::validate_sequential(nl).ok());
  assert(logic::block_input_count(nl) == 2);  // T.T, SR.R
  assert(logic::block_output_count(nl) == 1); // SR.Q (Q0 is wired)
  return nl;
}

void test_refusal_midrun_sr_abort() {
  RecSink rec;
  core::RunConfig cfg = make_logic_config(make_midrun_illegal(), {kH, kH}, 4);
  solvers::ConfiguredRun run;
  assert(solvers::ConfiguredRun::build(
             cfg, std::span<core::ISolver *>{}, false, run)
             .ok());
  run.add_sink(rec);
  const core::Status st = run.run(4);
  assert(!st.ok() && st.code == core::StatusCode::kInvalidArgument);
  // Step index frozen at the failing step; time advanced only by step 1.
  assert(run.state().step_index == 1);
  // Prior entries intact: exactly the completed step was emitted.
  assert(rec.snaps.size() == 1);
  // Pool untouched at the failing boundary: Q values are the post-step-1
  // values (counter advanced, latch reset), never the illegal commit.
  assert(run.state().seq.q[0] == kH); // T toggled 0 -> 1 in step 1
  assert(run.state().seq.q[1] == kL); // SR reset (S=0, R=1) in step 1
  assert(run.state().seq_outputs[0] == kL);
  // The failure latches: further driving refuses immediately.
  assert(!run.run(1).ok());
  assert(run.state().step_index == 1);
  assert(rec.snaps.size() == 1);
}

void test_refusal_xz_stimulus() {
  // Non-binary stimulus never reaches a step (driver gate, state untouched).
  world::State out;
  const std::span<emit::ISink *> none{};
  {
    core::RunConfig cfg = make_logic_config(make_tdivider(), {LogicValue::kHighZ}, 2);
    assert(!solvers::run_headless(cfg, none, out).ok());
  }
  {
    core::RunConfig cfg = make_logic_config(make_tdivider(), {LogicValue::kUnknown}, 2);
    assert(!solvers::run_headless(cfg, none, out).ok());
  }
  assert(out.step_index == 0);
}

// Full-control DFF (D/CLK/RST/SET/EN): 4 block inputs (CLK ignored),
// Q dangling. RST > SET > EN-hold > D, identical to interactive stepping.
Netlist make_full_dff() {
  Netlist nl;
  const std::uint16_t d = logic::add_gate(nl, GateKind::kDff, 5).value;
  ignore_input(nl, d, 1); // CLK ignored (edge is the call)
  assert(logic::validate_sequential(nl).ok());
  assert(logic::block_input_count(nl) == 4);  // D, RST, SET, EN
  assert(logic::block_output_count(nl) == 1); // Q
  return nl;
}

void test_control_rst_beats_set() {
  // RST and SET both asserted, D=High: reset wins (Q reads Low).
  core::RunConfig cfg = make_logic_config(make_full_dff(), {kH, kH, kH, kH}, 1);
  world::State out;
  assert(solvers::run_headless(cfg, std::span<emit::ISink *>{}, out).ok());
  assert(out.seq.q[0] == kL);
  assert(out.seq_outputs[0] == kL);
  // SET alone with D=High: set wins over D (Q reads High).
  core::RunConfig cfg2 = make_logic_config(make_full_dff(), {kH, kL, kH, kH}, 1);
  world::State out2;
  assert(solvers::run_headless(cfg2, std::span<emit::ISink *>{}, out2).ok());
  assert(out2.seq.q[0] == kH);
  // Async priority holds under explicit member selection too (vs clocks).
  core::RunConfig cfg3 = make_select_config(make_full_dff(), {kH, kH, kH, kH}, {0}, false, 1);
  world::State out3;
  assert(solvers::run_headless(cfg3, std::span<emit::ISink *>{}, out3).ok());
  assert(out3.seq.q[0] == kL);
}

void test_control_en_hold() {
  // EN=Low holds Q across steps regardless of D (synchronous hold).
  Netlist nl = make_full_dff();
  world::State s;
  populate_seq_pool(s, nl, {kH, kL, kH, kH}); // SET drives Q High...
  solvers::SequentialSolver solver;
  core::Runner runner{{.dt = 0.01}};
  runner.add_solver(solver);
  runner.state().netlist = s.netlist;
  runner.state().seq = s.seq;
  runner.state().seq_inputs = s.seq_inputs;
  runner.state().seq_members = s.seq_members;
  runner.state().seq_outputs = s.seq_outputs;
  assert(runner.run(1).ok());
  assert(runner.state().seq.q[0] == kH);
  // ...then EN=Low freezes it: D=High cannot change Q.
  runner.state().seq_inputs = {kH, kL, kL, kL};
  assert(runner.run(1).ok());
  assert(runner.state().seq.q[0] == kH);
  assert(runner.run(1).ok());
  assert(runner.state().seq.q[0] == kH);
  // Releasing EN (High) with D=Low follows D down at once.
  runner.state().seq_inputs = {kL, kL, kL, kH};
  assert(runner.run(1).ok());
  assert(runner.state().seq.q[0] == kL);
}

} // namespace

int main() {
  test_solver_counter_4steps();
  test_solver_tdivider();
  test_solver_tdiv4();
  test_solver_latch_hold();
  test_solver_bad_dt_rejected();
  test_solver_determinism_replay();
  test_driver_counter_sequence();
  test_driver_identical_replay();
  test_driver_bad_config_rejected();
  test_driver_empty_net_run();
  test_driver_section_inits_power_on();
  test_driver_default_inits_low();
  test_derived_domain_selection_with_inits();
  test_selection_selective_advance_hold();
  test_selection_all_domains_advance();
  test_selection_dangling_clk_membership();
  test_selection_member_rejections_untouched();
  test_observation_fill_snapshot_order_recycle();
  test_observation_take_snapshot_parity();
  test_observation_csv_reread();
  test_observation_midrun_replay();
  test_refusal_midrun_sr_abort();
  test_refusal_xz_stimulus();
  test_control_rst_beats_set();
  test_control_en_hold();
  std::cout << "test_logic_seq_runner: OK\n";
  return 0;
}
