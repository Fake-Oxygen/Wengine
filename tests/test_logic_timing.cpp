// 014-delay-honoring-evaluator: opt-in transport-delay wheel tests.
//
// Hand-derived goldens only (pulse widths, drain times, race offsets,
// 4-state transient windows — never the implementation's readback),
// CLI representation, byte-identity of pre-feature flows, parity
// (wheel end-state == zero-delay; step trajectories equal), and
// determinism re-runs. Reference: IEEE 1364 gate-level semantics
// plus path-delay arithmetic (constitution I/III, FR-014).
//
// Temp files (if any) use /tmp/wengine_timing_* — never /tmp/opencode
// (unwritable in some sandboxes).

#include "wengine/logic/block_interface.hpp"
#include "wengine/logic/eval.hpp"
#include "wengine/logic/netlist.hpp"
#include "wengine/logic/seq_state.hpp"
#include "wengine/logic/timing.hpp"
#include "wengine/logic/types.hpp"

#include "wengine/cli/args.hpp"
#include "wengine/cli/logic_registry.hpp"
#include "wengine/cli/shell.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace wengine;
using wengine::logic::LogicValue;

namespace {

constexpr LogicValue L = LogicValue::kLow;
constexpr LogicValue H = LogicValue::kHigh;
constexpr LogicValue X = LogicValue::kUnknown;
constexpr LogicValue Z = LogicValue::kHighZ;

int run_line(const std::string &line, std::string &text) {
  wengine::cli::Shell sh;
  sh.set_history_file("");
  std::ostringstream out;
  const int rc = sh.execute(line, out);
  text = out.str();
  return rc;
}

// Hazard net: IN -> NOT1 -> NOT2 -> NOT3 -> AND1.2, IN -> AND1.1.
// Two block inputs (NOT1.1 ordinal 0, AND1.1 ordinal 1). Slots:
// NOT1=0, NOT2=1, NOT3=2, AND1=3.
struct HazardNet {
  logic::Netlist nl;
  std::uint16_t n1 = 0;
  std::uint16_t n2 = 0;
  std::uint16_t n3 = 0;
  std::uint16_t a = 0;
};

HazardNet make_hazard() {
  HazardNet h;
  h.n1 = logic::add_gate(h.nl, logic::GateKind::kNot, 1).value;
  h.n2 = logic::add_gate(h.nl, logic::GateKind::kNot, 1).value;
  h.n3 = logic::add_gate(h.nl, logic::GateKind::kNot, 1).value;
  h.a = logic::add_gate(h.nl, logic::GateKind::kAnd, 2).value;
  assert(h.n1 == 0 && h.n2 == 1 && h.n3 == 2 && h.a == 3);
  assert(logic::wire_pins(h.nl, logic::PinRef{.slot = h.n1, .is_output = true}, logic::PinRef{.slot = h.n2}).ok());
  assert(logic::wire_pins(h.nl, logic::PinRef{.slot = h.n2, .is_output = true}, logic::PinRef{.slot = h.n3}).ok());
  assert(logic::wire_pins(h.nl, logic::PinRef{.slot = h.n3, .is_output = true},
                          logic::PinRef{.slot = h.a, .is_output = false, .input = 1})
             .ok());
  return h;
}

// Pre-state of the all-zero vector: NOT1.Y=1, NOT2.Y=0, NOT3.Y=1,
// AND1.Y=0. Both block inputs driven High.
std::vector<logic::TimingSeed> hazard_seeds() {
  return std::vector<logic::TimingSeed>{
      logic::TimingSeed{.slot = 0, .input = 0, .is_flop = false, .fire_ps = 0, .new_value = H},
      logic::TimingSeed{.slot = 3, .input = 0, .is_flop = false, .fire_ps = 0, .new_value = H},
  };
}

struct ExpectedEvent {
  std::uint64_t t;
  std::uint16_t slot;
  std::uint8_t input;
  bool is_input;
  bool nq;
  LogicValue old_v;
  LogicValue new_v;
};

void check_trace(const logic::TimingTrace &trace, const ExpectedEvent *want, std::size_t n, std::uint64_t drain,
                 std::size_t n_annotated) {
  assert(trace.n_events == n);
  assert(trace.drain_ps == drain);
  assert(trace.n_annotated == n_annotated);
  assert(trace.unit_ps == 1);
  for (std::size_t i = 0; i < n; ++i) {
    const logic::TimingEvent &e = trace.events[i];
    assert(e.t_ps == want[i].t);
    assert(e.slot == want[i].slot);
    assert(e.input == want[i].input);
    assert(e.is_input == want[i].is_input);
    assert(e.nq == want[i].nq);
    assert(e.old_value == want[i].old_v);
    assert(e.new_value == want[i].new_v);
  }
}

// US1 (T013): the hidden pulse — hand-derived path-delay arithmetic.
// Three unit inverters against a direct path: AND1.Y goes 0->1 at
// t=1 (direct arrival) and 1->0 at t=4 (chain arrival), pulse width
// 3 x unit. Pre-state: NOT1.Y=1, NOT2.Y=0, NOT3.Y=1, AND1.Y=0.
void test_pulse_unit_golden() {
  HazardNet h = make_hazard();
  logic::TimingScratch scratch{};
  const std::vector<LogicValue> zero{logic::LogicValue::kLow, logic::LogicValue::kLow};
  assert(logic::timing_settle_initial(h.nl, zero, scratch, logic::EdgeFilter::kAll).ok());
  // Pre-state spot check (the zero-delay fixed point, independently).
  assert(scratch.cur[0] == H && scratch.cur[1] == L && scratch.cur[2] == H && scratch.cur[3] == L);
  const std::vector<logic::TimingSeed> seeds = hazard_seeds();
  logic::TimingTrace trace;
  assert(logic::run_timing_window(h.nl, seeds, trace, scratch, false).ok());
  const ExpectedEvent want[] = {
      {0, 0, 0, true, false, L, H},  // t=0 NOT1.1: 0->1 (seed)
      {0, 3, 0, true, false, L, H},  // t=0 AND1.1: 0->1 (seed)
      {1, 0, 0, false, false, H, L}, // t=1 NOT1.Y: 1->0
      {1, 3, 0, false, false, L, H}, // t=1 AND1.Y: 0->1 (direct arrival)
      {2, 1, 0, false, false, L, H}, // t=2 NOT2.Y: 0->1
      {3, 2, 0, false, false, H, L}, // t=3 NOT3.Y: 1->0
      {4, 3, 0, false, false, H, L}, // t=4 AND1.Y: 1->0 (chain arrival)
  };
  check_trace(trace, want, 7, 4, 0);
}

// US1 (T013): annotated variant — every transition lands exactly its
// stored annotation after its trigger. NOT=5ps, AND=2ps: direct
// arrival at t=2, chain arrival at t=17, pulse width 15 = 3 x 5.
void test_pulse_annotated_golden() {
  HazardNet h = make_hazard();
  h.nl.gates[h.n1].propagation_delay_ps = 5;
  h.nl.gates[h.n2].propagation_delay_ps = 5;
  h.nl.gates[h.n3].propagation_delay_ps = 5;
  h.nl.gates[h.a].propagation_delay_ps = 2;
  logic::TimingScratch scratch{};
  const std::vector<LogicValue> zero{logic::LogicValue::kLow, logic::LogicValue::kLow};
  assert(logic::timing_settle_initial(h.nl, zero, scratch, logic::EdgeFilter::kAll).ok());
  const std::vector<logic::TimingSeed> seeds = hazard_seeds();
  logic::TimingTrace trace;
  assert(logic::run_timing_window(h.nl, seeds, trace, scratch, false).ok());
  const ExpectedEvent want[] = {
      {0, 0, 0, true, false, L, H},   // t=0 NOT1.1: 0->1
      {0, 3, 0, true, false, L, H},   // t=0 AND1.1: 0->1
      {2, 3, 0, false, false, L, H},  // t=2 AND1.Y: 0->1 (AND annotation)
      {5, 0, 0, false, false, H, L},  // t=5 NOT1.Y: 1->0
      {10, 1, 0, false, false, L, H}, // t=10 NOT2.Y: 0->1
      {15, 2, 0, false, false, H, L}, // t=15 NOT3.Y: 1->0
      {17, 3, 0, false, false, H, L}, // t=17 AND1.Y: 1->0
  };
  check_trace(trace, want, 7, 17, 4);
}

// US1 (T014): transport semantics — every input-net transition
// schedules its own recomputation at its own offset; nothing cancels.
// G = AND(D, E) with d=10: D arrives at t=0 (direct seed), E arrives
// at t=3 (two-deep BUF chain, d=2+1). Two pending evaluations coexist
// at t=10 and t=13; the first fires 0->1 and the second is silent.
// An inertial discipline would cancel the t=10 pending on the t=3
// arrival and fire once at t=13 — the drain (10 vs 13) and the G
// timestamp separate the disciplines (spec FR-003).
void test_transport_no_cancellation() {
  logic::Netlist nl;
  const std::uint16_t b1 = logic::add_gate(nl, logic::GateKind::kBuf, 1).value;
  const std::uint16_t b2 = logic::add_gate(nl, logic::GateKind::kBuf, 1).value;
  const std::uint16_t g = logic::add_gate(nl, logic::GateKind::kAnd, 2).value;
  assert(logic::wire_pins(nl, logic::PinRef{.slot = b1, .is_output = true}, logic::PinRef{.slot = b2}).ok());
  assert(logic::wire_pins(nl, logic::PinRef{.slot = b2, .is_output = true},
                          logic::PinRef{.slot = g, .is_output = false, .input = 1})
             .ok());
  nl.gates[b1].propagation_delay_ps = 2;
  nl.gates[g].propagation_delay_ps = 10;
  // Block inputs in slot order: BUF1.1 (ord 0), G.1 (ord 1).
  logic::TimingScratch scratch{};
  const std::vector<LogicValue> zero{logic::LogicValue::kLow, logic::LogicValue::kLow};
  assert(logic::timing_settle_initial(nl, zero, scratch, logic::EdgeFilter::kAll).ok());
  assert(scratch.cur[b1] == L && scratch.cur[b2] == L && scratch.cur[g] == L);
  const std::vector<logic::TimingSeed> seeds{
      logic::TimingSeed{.slot = b1, .is_flop = false, .fire_ps = 0, .new_value = H},
      logic::TimingSeed{.slot = g, .input = 0, .is_flop = false, .fire_ps = 0, .new_value = H},
  };
  logic::TimingTrace trace;
  assert(logic::run_timing_window(nl, seeds, trace, scratch, false).ok());
  const ExpectedEvent want[] = {
      {0, 0, 0, true, false, L, H},  // t=0 BUF1.1: 0->1
      {0, 2, 0, true, false, L, H},  // t=0 G.1: 0->1
      {2, 0, 0, false, false, L, H}, // t=2 BUF1.Y: 0->1
      {3, 1, 0, false, false, L, H}, // t=3 BUF2.Y: 0->1 (E arrives)
      {10, 2, 0, false, false, L, H}, // t=10 G.Y: 0->1 (direct arrival + 10)
  };
  check_trace(trace, want, 5, 10, 2);
}

// US1 (T015): unit floor — un-annotated gates take exactly 1ps (the
// field's smallest storable nonzero value); stored 0 stays
// un-annotated; the header names the unit. Empty window drains 0ps.
void test_unit_floor() {
  logic::Netlist nl;
  const std::uint16_t n = logic::add_gate(nl, logic::GateKind::kNot, 1).value;
  assert(nl.gates[n].propagation_delay_ps == 0);
  logic::TimingScratch scratch{};
  const std::vector<LogicValue> zero{logic::LogicValue::kLow};
  assert(logic::timing_settle_initial(nl, zero, scratch, logic::EdgeFilter::kAll).ok());
  // Empty window: no seeds, no events, drain 0.
  {
    const std::vector<logic::TimingSeed> none;
    logic::TimingTrace trace;
    assert(logic::run_timing_window(nl, none, trace, scratch, false).ok());
    assert(trace.n_events == 0);
    assert(trace.drain_ps == 0);
    assert(trace.n_annotated == 0);
    assert(trace.unit_ps == 1);
  }
  // One seed: the un-annotated gate fires at exactly t=1.
  {
    const std::vector<logic::TimingSeed> seeds{logic::TimingSeed{.slot = n, .is_flop = false, .fire_ps = 0, .new_value = H}};
    logic::TimingTrace trace;
    assert(logic::timing_settle_initial(nl, zero, scratch, logic::EdgeFilter::kAll).ok());
    assert(logic::run_timing_window(nl, seeds, trace, scratch, false).ok());
    const ExpectedEvent want[] = {
        {0, 0, 0, true, false, L, H},
        {1, 0, 0, false, false, H, L},
    };
    check_trace(trace, want, 2, 1, 0);
  }
}

// US1 (T016): same-instant order is (time, slot order) and repeats
// byte-identically — never arrival order, never evaluation whim.
void test_same_instant_order_and_determinism() {
  auto run = []() {
    HazardNet h = make_hazard();
    logic::TimingScratch scratch{};
    const std::vector<LogicValue> zero{logic::LogicValue::kLow, logic::LogicValue::kLow};
    assert(logic::timing_settle_initial(h.nl, zero, scratch, logic::EdgeFilter::kAll).ok());
    const std::vector<logic::TimingSeed> seeds = hazard_seeds();
    logic::TimingTrace trace;
    assert(logic::run_timing_window(h.nl, seeds, trace, scratch, false).ok());
    return trace;
  };
  const logic::TimingTrace a = run();
  const logic::TimingTrace b = run();
  assert(a.n_events == b.n_events && a.drain_ps == b.drain_ps);
  for (std::size_t i = 0; i < a.n_events; ++i) {
    const logic::TimingEvent &x = a.events[i];
    const logic::TimingEvent &y = b.events[i];
    assert(x.t_ps == y.t_ps && x.slot == y.slot && x.input == y.input && x.is_input == y.is_input &&
           x.nq == y.nq && x.old_value == y.old_value && x.new_value == y.new_value);
  }
  // The two t=1 events list in slot order (NOT1 @0 before AND1 @3).
  assert(a.n_events == 7);
  assert(a.events[2].t_ps == 1 && a.events[2].slot == 0);
  assert(a.events[3].t_ps == 1 && a.events[3].slot == 3);
}

// US1 (T017): combinational parity — the wheel's end-state equals the
// zero-delay solve for the same assignment (the trace adds
// transients, never different endpoints).
void test_wheel_end_state_parity() {
  HazardNet h = make_hazard();
  h.nl.gates[h.n2].propagation_delay_ps = 7; // stagger: parity must hold regardless
  logic::TimingScratch scratch{};
  const std::vector<LogicValue> applied{logic::LogicValue::kHigh, logic::LogicValue::kHigh};
  const std::vector<LogicValue> zero{logic::LogicValue::kLow, logic::LogicValue::kLow};
  assert(logic::timing_settle_initial(h.nl, zero, scratch, logic::EdgeFilter::kAll).ok());
  const std::vector<logic::TimingSeed> seeds = hazard_seeds();
  logic::TimingTrace trace;
  assert(logic::run_timing_window(h.nl, seeds, trace, scratch, false).ok());
  // Zero-delay reference on the same assignment.
  std::vector<LogicValue> ref_out(1, L);
  assert(logic::solve(h.nl, applied, ref_out, nullptr).ok());
  assert(ref_out.size() == 1);
  // The single block output is AND1.Y (slot 3, only dangling output).
  assert(scratch.cur[h.a] == ref_out[0]);
}

// US1 (T020): CLI representation — the --with-delays block grammar,
// the empty window, and the byte-identical settled tail.
void test_solve_with_delays_cli() {
  std::string text;
  assert(run_line(std::string("logic load ") + WENGINE_LOGIC_EXAMPLE_DIR "/glitch_chain.yaml", text) == 0);
  // Flag-off: settled values only, no trace text.
  assert(run_line("logic solve --in 1", text) == 0);
  const std::string flag_off = text;
  assert(flag_off == "in: 1 (NOT1.1=1)\nAND1.Y=0\n");
  // Flag-on: same echo, timing block, byte-identical settled tail.
  assert(run_line("logic solve --in 1 --with-delays", text) == 0);
  const std::string want = "in: 1 (NOT1.1=1)\n"
                           "timing: unit 1ps, 0 annotated\n"
                           "  t=0ps NOT1.1: 0->1\n"
                           "  t=1ps NOT1.Y: 1->0\n"
                           "  t=1ps AND1.Y: 0->1\n"
                           "  t=2ps NOT2.Y: 0->1\n"
                           "  t=3ps NOT3.Y: 1->0\n"
                           "  t=4ps AND1.Y: 1->0\n"
                           "settled in 4ps\n"
                           "AND1.Y=0\n";
  assert(text == want);
  // Empty window: assignment equals the all-zero pre-vector.
  assert(run_line("logic solve --in 0 --with-delays", text) == 0);
  assert(text == "in: 0 (NOT1.1=0)\ntiming: unit 1ps, 0 annotated\nsettled in 0ps\nAND1.Y=0\n");
  // Annotated variant scales by the annotations (unit floor named).
  assert(run_line("logic set-delay NOT1 5ps", text) == 0);
  assert(run_line("logic set-delay NOT2 5ps", text) == 0);
  assert(run_line("logic set-delay NOT3 5ps", text) == 0);
  assert(run_line("logic set-delay AND1 2ps", text) == 0);
  assert(run_line("logic solve --in 1 --with-delays", text) == 0);
  assert(text == "in: 1 (NOT1.1=1)\n"
                 "timing: unit 1ps, 4 annotated\n"
                 "  t=0ps NOT1.1: 0->1\n"
                 "  t=2ps AND1.Y: 0->1\n"
                 "  t=5ps NOT1.Y: 1->0\n"
                 "  t=10ps NOT2.Y: 0->1\n"
                 "  t=15ps NOT3.Y: 1->0\n"
                 "  t=17ps AND1.Y: 1->0\n"
                 "settled in 17ps\n"
                 "AND1.Y=0\n");
  assert(run_line("logic reset", text) == 0);
}

// US2 (T021): flag-off byte-identity — every default flow reproduces
// itself exactly, and the timing tail adds nothing to settled lines.
// (Relational pins: identity is the property; absolute goldens live
// in the pre-feature suites this test must never move.)
void test_flag_off_identity() {
  std::string text;
  assert(run_line("logic reset", text) == 0);
  assert(run_line("logic add AND", text) == 0);
  assert(run_line("logic add NOT", text) == 0);
  assert(run_line("logic wire AND1.Y NOT1.1", text) == 0);
  assert(run_line("logic solve --in 10", text) == 0);
  const std::string solve_a = text;
  assert(run_line("logic solve --in 10", text) == 0);
  assert(text == solve_a); // default reproduces itself
  // Timing run's settled tail is the flag-off output byte-for-byte
  // (same echo, timing block inserted, same settled lines after).
  assert(run_line("logic solve --in 10 --with-delays", text) == 0);
  const std::string timed = text;
  const std::string marker = "timing: unit 1ps, 0 annotated\n";
  const std::size_t tpos = timed.find(marker);
  assert(tpos != std::string::npos);
  const std::string settle_marker = "settled in 0ps\n";
  const std::size_t spos = timed.find(settle_marker);
  assert(spos != std::string::npos && spos > tpos);
  const std::string stripped = timed.substr(0, tpos) + timed.substr(spos + settle_marker.size());
  assert(stripped == solve_a);
  // Step and truth defaults reproduce themselves exactly.
  assert(run_line("logic reset", text) == 0);
  assert(run_line("logic add DFF", text) == 0);
  assert(run_line("logic add NOT", text) == 0);
  assert(run_line("logic wire NOT1.Y DFF1.D", text) == 0);
  assert(run_line("logic step", text) == 0);
  const std::string step_a = text;
  assert(run_line("logic peek", text) == 0);
  const std::string peek_a = text;
  assert(run_line("logic reset", text) == 0);
  assert(run_line("logic add DFF", text) == 0);
  assert(run_line("logic add NOT", text) == 0);
  assert(run_line("logic wire NOT1.Y DFF1.D", text) == 0);
  assert(run_line("logic step", text) == 0);
  assert(text == step_a);
  assert(run_line("logic peek", text) == 0);
  assert(text == peek_a);
  assert(run_line("logic reset", text) == 0);
  assert(run_line("logic add AND", text) == 0);
  assert(run_line("logic add NOT", text) == 0);
  assert(run_line("logic wire AND1.Y NOT1.1", text) == 0);
  assert(run_line("logic truth --limit 4", text) == 0);
  const std::string truth_a = text;
  assert(run_line("logic truth --limit 4", text) == 0);
  assert(text == truth_a);
  assert(run_line("logic reset", text) == 0);
}

// US2 (T022): annotation blindness — the same netlist with and
// without propagation_delay_ps annotations evaluates identically in
// default mode (013's inertness promise, kept: only --with-delays
// reads the field).
void test_annotation_blindness() {
  std::string text;
  assert(run_line("logic reset", text) == 0);
  assert(run_line("logic add AND", text) == 0);
  assert(run_line("logic add NOT", text) == 0);
  assert(run_line("logic wire AND1.Y NOT1.1", text) == 0);
  assert(run_line("logic solve --in 11", text) == 0);
  const std::string plain_solve = text;
  assert(run_line("logic truth --limit 4", text) == 0);
  const std::string plain_truth = text;
  // Heavily annotate every gate, including an hour-scale value.
  assert(run_line("logic set-delay AND1 1.5h", text) == 0);
  assert(run_line("logic set-delay NOT1 9999999999999999999", text) == 0);
  assert(run_line("logic solve --in 11", text) == 0);
  assert(text == plain_solve);
  assert(run_line("logic truth --limit 4", text) == 0);
  assert(text == plain_truth);
  // Sequential flows are annotation-blind too.
  assert(run_line("logic reset", text) == 0);
  assert(run_line("logic add DFF", text) == 0);
  assert(run_line("logic add NOT", text) == 0);
  assert(run_line("logic wire NOT1.Y DFF1.D", text) == 0);
  assert(run_line("logic step", text) == 0);
  const std::string plain_step = text;
  assert(run_line("logic set-delay DFF1 42ps", text) == 0);
  assert(run_line("logic reset", text) == 0);
  assert(run_line("logic add DFF", text) == 0);
  assert(run_line("logic add NOT", text) == 0);
  assert(run_line("logic wire NOT1.Y DFF1.D", text) == 0);
  assert(run_line("logic set-delay DFF1 42ps", text) == 0);
  assert(run_line("logic step", text) == 0);
  assert(text == plain_step);
  assert(run_line("logic reset", text) == 0);
}

// US3 (T025): clock-to-Q golden — the edge flop's Q transitions
// exactly its annotation after the edge; NQ shares the instant
// (never lags). DFF1 (cq=7) feeds NOT1: Q/NQ at t=7, ripple at t=8.
void test_edge_flop_clock_to_q() {
  logic::Netlist nl;
  const std::uint16_t d = logic::add_gate(nl, logic::GateKind::kDff, 2).value;
  const std::uint16_t n = logic::add_gate(nl, logic::GateKind::kNot, 1).value;
  assert(logic::wire_pins(nl, logic::PinRef{.slot = d, .is_output = true}, logic::PinRef{.slot = n}).ok());
  nl.gates[d].propagation_delay_ps = 7;
  logic::TimingScratch scratch{};
  // Step-like pre-state: DFF1.D/DFF1.CLK block inputs Low, Q=0.
  const std::vector<LogicValue> zero{logic::LogicValue::kLow, logic::LogicValue::kLow};
  assert(logic::timing_settle_initial(nl, zero, scratch, logic::EdgeFilter::kAll).ok());
  assert(scratch.cur[n] == H); // NOT(0)=1
  // Post-edge: Q captured 1; the unchanged stepper committed this.
  const std::vector<logic::TimingSeed> seeds{
      logic::TimingSeed{.slot = d, .is_flop = true, .fire_ps = 7, .new_value = H}};
  logic::TimingTrace trace;
  assert(logic::run_timing_window(nl, seeds, trace, scratch, true).ok());
  const ExpectedEvent want[] = {
      {7, 0, 0, false, false, L, H}, // t=7 DFF1.Q: 0->1 (clock-to-Q)
      {7, 0, 0, false, true, H, L},  // t=7 DFF1.NQ: 1->0 (same instant)
      {8, 1, 0, false, false, H, L}, // t=8 NOT1.Y: 1->0
  };
  check_trace(trace, want, 3, 8, 1);
}

// US3 (T026): D-arrival race offset — DFF1.Q (cq=7) through BUF1
// (d=3) lands on DFF2.D at t=10, the hand-derived clock-to-Q +
// downstream path sum. The trace exposes ordering; the committed
// state trajectory is the unchanged stepper's (pinned at CLI level).
void test_d_arrival_race_offset() {
  logic::Netlist nl;
  const std::uint16_t d1 = logic::add_gate(nl, logic::GateKind::kDff, 2).value;
  const std::uint16_t b = logic::add_gate(nl, logic::GateKind::kBuf, 1).value;
  const std::uint16_t d2 = logic::add_gate(nl, logic::GateKind::kDff, 2).value;
  assert(logic::wire_pins(nl, logic::PinRef{.slot = d1, .is_output = true}, logic::PinRef{.slot = b}).ok());
  assert(logic::wire_pins(nl, logic::PinRef{.slot = b, .is_output = true},
                          logic::PinRef{.slot = d2, .is_output = false, .input = 0})
             .ok());
  nl.gates[d1].propagation_delay_ps = 7;
  nl.gates[b].propagation_delay_ps = 3;
  logic::TimingScratch scratch{};
  // Block inputs: DFF1.D, DFF1.CLK, DFF2.CLK (DFF2.D is wired).
  const std::vector<LogicValue> zero{logic::LogicValue::kLow, logic::LogicValue::kLow, logic::LogicValue::kLow};
  assert(logic::timing_settle_initial(nl, zero, scratch, logic::EdgeFilter::kAll).ok());
  const std::vector<logic::TimingSeed> seeds{
      logic::TimingSeed{.slot = d1, .is_flop = true, .fire_ps = 7, .new_value = H}};
  logic::TimingTrace trace;
  assert(logic::run_timing_window(nl, seeds, trace, scratch, true).ok());
  const ExpectedEvent want[] = {
      {7, 0, 0, false, false, L, H},  // t=7 DFF1.Q: 0->1
      {7, 0, 0, false, true, H, L},   // t=7 DFF1.NQ: 1->0
      {10, 1, 0, false, false, L, H}, // t=10 BUF1.Y: 0->1 (D arrives at DFF2.D)
  };
  check_trace(trace, want, 3, 10, 2);
}

// US3 (T027): level-latch response lands at its annotation; released
// enable holds (the standing level rules are unchanged — the wheel
// only observes).
void test_latch_response_delay() {
  auto build = []() {
    logic::Netlist nl;
    const std::uint16_t l = logic::add_gate(nl, logic::GateKind::kDlatch, 2).value;
    const std::uint16_t n = logic::add_gate(nl, logic::GateKind::kNot, 1).value;
    assert(logic::wire_pins(nl, logic::PinRef{.slot = l, .is_output = true}, logic::PinRef{.slot = n}).ok());
    nl.gates[l].propagation_delay_ps = 4;
    return std::pair<logic::Netlist, std::uint16_t>(std::move(nl), l);
  };
  // Transparent (EN=1): D 0->1 lands on Q at t=4, ripple at t=5.
  {
    auto [nl, l] = build();
    (void)l;
    logic::TimingScratch scratch{};
    const std::vector<LogicValue> zero{logic::LogicValue::kLow, logic::LogicValue::kLow};
    assert(logic::timing_settle_initial(nl, zero, scratch, logic::EdgeFilter::kAll).ok());
    const std::vector<logic::TimingSeed> seeds{
        logic::TimingSeed{.slot = 0, .input = 0, .is_flop = false, .fire_ps = 0, .new_value = H},
        logic::TimingSeed{.slot = 0, .input = 1, .is_flop = false, .fire_ps = 0, .new_value = H},
    };
    logic::TimingTrace trace;
    assert(logic::run_timing_window(nl, seeds, trace, scratch, true).ok());
    const ExpectedEvent want[] = {
        {0, 0, 0, true, false, L, H},  // t=0 DLATCH1.D: 0->1
        {0, 0, 1, true, false, L, H},  // t=0 DLATCH1.EN: 0->1
        {4, 0, 0, false, false, L, H}, // t=4 DLATCH1.Q: 0->1 (response delay)
        {5, 1, 0, false, false, H, L}, // t=5 NOT1.Y: 1->0
    };
    check_trace(trace, want, 4, 5, 1);
  }
  // Held (EN=0): the D seed schedules a recompute that holds — silent.
  {
    auto [nl, l] = build();
    (void)l;
    logic::TimingScratch scratch{};
    const std::vector<LogicValue> zero{logic::LogicValue::kLow, logic::LogicValue::kLow};
    assert(logic::timing_settle_initial(nl, zero, scratch, logic::EdgeFilter::kAll).ok());
    const std::vector<logic::TimingSeed> seeds{
        logic::TimingSeed{.slot = 0, .input = 0, .is_flop = false, .fire_ps = 0, .new_value = H},
    };
    logic::TimingTrace trace;
    assert(logic::run_timing_window(nl, seeds, trace, scratch, true).ok());
    const ExpectedEvent want[] = {
        {0, 0, 0, true, false, L, H}, // t=0 DLATCH1.D: 0->1 (held, no Q event)
    };
    check_trace(trace, want, 1, 0, 1);
  }
}

// US3 (T028): transient 4-state windows (reference: IEEE 1364
// dominant-value tables, research D5). Transients render X visibly,
// never coerced; settled contracts ride the unchanged paths.
// Net: NOT1 (E) -> TBUF1.EN, data/TBUF1.1 + AND1.2 block inputs,
// TBUF1.Y -> AND1.1. Pre (all zero): NOT1.Y=1 (EN asserted),
// TBUF1.Y=data=0, AND1.Y=0.
struct TransientNet {
  logic::Netlist nl;
  std::uint16_t n = 0; // NOT1 (EN source)
  std::uint16_t t = 0; // TBUF1
  std::uint16_t a = 0; // AND1
};

TransientNet make_transient() {
  TransientNet h;
  h.n = logic::add_gate(h.nl, logic::GateKind::kNot, 1).value;
  h.t = logic::add_gate(h.nl, logic::GateKind::kTbuf, 2).value;
  h.a = logic::add_gate(h.nl, logic::GateKind::kAnd, 2).value;
  assert(h.n == 0 && h.t == 1 && h.a == 2);
  assert(logic::wire_pins(h.nl, logic::PinRef{.slot = h.n, .is_output = true},
                          logic::PinRef{.slot = h.t, .is_output = false, .input = 1})
             .ok());
  assert(logic::wire_pins(h.nl, logic::PinRef{.slot = h.t, .is_output = true},
                          logic::PinRef{.slot = h.a, .is_output = false, .input = 0})
             .ok());
  return h;
}

// AND(0,X)=0: EN falls at t=1 while data=1 rides through — the
// TBUF output goes 1...0->Z, AND's second input stays 0, so the
// dominant 0 holds and AND1.Y shows no pulse (X is correct, never
// a false-positive glitch).
void test_transient_and_dominant_zero() {
  TransientNet h = make_transient();
  logic::TimingScratch scratch{};
  // Block inputs: TBUF1.1 data (ord 0), NOT1.1 E (ord 1), AND1.2 (ord 2).
  const std::vector<LogicValue> zero{logic::LogicValue::kLow, logic::LogicValue::kLow, logic::LogicValue::kLow};
  assert(logic::timing_settle_initial(h.nl, zero, scratch, logic::EdgeFilter::kAll).ok());
  assert(scratch.cur[h.t] == L); // pre: EN asserted (NOT(0)=1), data 0 -> Y=0
  const std::vector<logic::TimingSeed> seeds{
      logic::TimingSeed{.slot = h.t, .input = 0, .is_flop = false, .fire_ps = 0, .new_value = H},
      logic::TimingSeed{.slot = h.n, .input = 0, .is_flop = false, .fire_ps = 0, .new_value = H},
  };
  logic::TimingTrace trace;
  assert(logic::run_timing_window(h.nl, seeds, trace, scratch, false).ok());
  // data=1 rides: TBUF enabled at fire time (EN falls at the same
  // instant but the NOT slot pops first, so TBUF already sees EN=0
  // and releases instead): Y goes 0->Z directly.
  const ExpectedEvent want[] = {
      {0, 1, 0, true, false, L, H},  // t=0 TBUF1.1: 0->1
      {0, 0, 0, true, false, L, H},  // t=0 NOT1.1: 0->1
      {1, 0, 0, false, false, H, L},  // t=1 NOT1.Y: 1->0 (EN falls)
      {1, 1, 0, false, false, L, Z},  // t=1 TBUF1.Y: 0->Z (released)
  };
  check_trace(trace, want, 4, 1, 0);
  assert(scratch.cur[h.a] == L); // AND(0,X)=0: no pulse, no event
}

// AND(1,X)=X: same window with AND1.2=1 — the transient unknown
// propagates visibly as a 0->X transition (parity has no dominant
// value to stabilize it).
void test_transient_and_unknown() {
  TransientNet h = make_transient();
  logic::TimingScratch scratch{};
  const std::vector<LogicValue> zero{logic::LogicValue::kLow, logic::LogicValue::kLow, logic::LogicValue::kLow};
  assert(logic::timing_settle_initial(h.nl, zero, scratch, logic::EdgeFilter::kAll).ok());
  const std::vector<logic::TimingSeed> seeds{
      logic::TimingSeed{.slot = h.t, .input = 0, .is_flop = false, .fire_ps = 0, .new_value = H},
      logic::TimingSeed{.slot = h.n, .input = 0, .is_flop = false, .fire_ps = 0, .new_value = H},
      logic::TimingSeed{.slot = h.a, .input = 1, .is_flop = false, .fire_ps = 0, .new_value = H},
  };
  logic::TimingTrace trace;
  assert(logic::run_timing_window(h.nl, seeds, trace, scratch, false).ok());
  const ExpectedEvent want[] = {
      {0, 1, 0, true, false, L, H},  // t=0 TBUF1.1: 0->1
      {0, 0, 0, true, false, L, H},  // t=0 NOT1.1: 0->1
      {0, 2, 1, true, false, L, H},  // t=0 AND1.2: 0->1
      {1, 0, 0, false, false, H, L},  // t=1 NOT1.Y: 1->0
      {1, 1, 0, false, false, L, Z},  // t=1 TBUF1.Y: 0->Z
      {1, 2, 0, false, false, L, X},  // t=1 AND1.Y: 0->X (transient, visible)
  };
  check_trace(trace, want, 6, 1, 0);
}

// Transient S=R=1 renders X: differential arrivals (S at t=1, R at
// t=5 through a d=4 BUF) open a both-high window mid-flight; the
// wheel shows X at those instants instead of coercing. Settled S=R=1
// still refuses on the unchanged level path (pinned by the
// pre-feature seq suites; the wheel never owns validation).
void test_transient_sr_both_high() {
  logic::Netlist nl;
  const std::uint16_t b1 = logic::add_gate(nl, logic::GateKind::kBuf, 1).value;
  const std::uint16_t b2 = logic::add_gate(nl, logic::GateKind::kBuf, 1).value;
  const std::uint16_t sr = logic::add_gate(nl, logic::GateKind::kSrlatch, 2).value;
  assert(logic::wire_pins(nl, logic::PinRef{.slot = b1, .is_output = true},
                          logic::PinRef{.slot = sr, .is_output = false, .input = 0})
             .ok());
  assert(logic::wire_pins(nl, logic::PinRef{.slot = b2, .is_output = true},
                          logic::PinRef{.slot = sr, .is_output = false, .input = 1})
             .ok());
  nl.gates[b2].propagation_delay_ps = 4;
  logic::TimingScratch scratch{};
  const std::vector<LogicValue> zero{logic::LogicValue::kLow, logic::LogicValue::kLow};
  assert(logic::timing_settle_initial(nl, zero, scratch, logic::EdgeFilter::kAll).ok());
  const std::vector<logic::TimingSeed> seeds{
      logic::TimingSeed{.slot = b1, .is_flop = false, .fire_ps = 0, .new_value = H},
      logic::TimingSeed{.slot = b2, .is_flop = false, .fire_ps = 0, .new_value = H},
  };
  logic::TimingTrace trace;
  assert(logic::run_timing_window(nl, seeds, trace, scratch, true).ok());
  const ExpectedEvent want[] = {
      {0, 0, 0, true, false, L, H},  // t=0 BUF1.1: 0->1
      {0, 1, 0, true, false, L, H},  // t=0 BUF2.1: 0->1
      {1, 0, 0, false, false, L, H},  // t=1 BUF1.Y: 0->1 (S arrives)
      {2, 2, 0, false, false, L, H},  // t=2 SRLATCH1.Q: 0->1 (set)
      {4, 1, 0, false, false, L, H},  // t=4 BUF2.Y: 0->1 (R arrives)
      {5, 2, 0, false, false, H, X},  // t=5 SRLATCH1.Q: 1->X (both high)
  };
  check_trace(trace, want, 6, 5, 1);
}

// US3 (T029): race-window determinism — repeated invocations over
// the same clocked net are byte-identical (pinned total order).
void test_race_determinism() {
  auto run = []() {
    logic::Netlist nl;
    const std::uint16_t d1 = logic::add_gate(nl, logic::GateKind::kDff, 2).value;
    const std::uint16_t b = logic::add_gate(nl, logic::GateKind::kBuf, 1).value;
    const std::uint16_t d2 = logic::add_gate(nl, logic::GateKind::kDff, 2).value;
    assert(logic::wire_pins(nl, logic::PinRef{.slot = d1, .is_output = true}, logic::PinRef{.slot = b}).ok());
    assert(logic::wire_pins(nl, logic::PinRef{.slot = b, .is_output = true},
                            logic::PinRef{.slot = d2, .is_output = false, .input = 0})
               .ok());
    nl.gates[d1].propagation_delay_ps = 7;
    nl.gates[b].propagation_delay_ps = 3;
    logic::TimingScratch scratch{};
    const std::vector<LogicValue> zero{logic::LogicValue::kLow, logic::LogicValue::kLow, logic::LogicValue::kLow};
    assert(logic::timing_settle_initial(nl, zero, scratch, logic::EdgeFilter::kAll).ok());
    const std::vector<logic::TimingSeed> seeds{
        logic::TimingSeed{.slot = d1, .is_flop = true, .fire_ps = 7, .new_value = H}};
    logic::TimingTrace trace;
    assert(logic::run_timing_window(nl, seeds, trace, scratch, true).ok());
    return trace;
  };
  const logic::TimingTrace a = run();
  const logic::TimingTrace b = run();
  assert(a.n_events == 3 && b.n_events == 3 && a.drain_ps == 10 && b.drain_ps == 10);
  for (std::size_t i = 0; i < a.n_events; ++i) {
    assert(a.events[i].t_ps == b.events[i].t_ps && a.events[i].slot == b.events[i].slot &&
           a.events[i].nq == b.events[i].nq && a.events[i].old_value == b.events[i].old_value &&
           a.events[i].new_value == b.events[i].new_value);
  }
}

// US3 (T030): loud refusals — the only two new failure vocabularies.
// The event cap guards transport multiplication over reconvergent
// fan-out (research D9): G_1 = XOR(SA, BUF(SB)) toggles twice, and
// each level pairs G_i with a delayed copy BUF(G_i, K_i) into
// G_{i+1} = XOR(G_i, copy) with K_i = 10*4^(i-1) keeping the two
// arrival combs disjoint — so level i toggles 2^i times and the
// cumulative 2*(2+...+2^10) = 4092 plus seeds/leaf fires trips
// kMaxTimingEvents. Breach refuses loudly with no partial trace,
// and the netlist/state/settled output are untouched (the adapter
// refuses before printing).
void test_event_cap_breach() {
  logic::Netlist nl;
  // 512 BUF leaves with ODD delays 1,3,...,1023 feeding a 511-XOR
  // binary tree (1023 gates, live_count asserted below). Odd spacing
  // keeps every pair of sibling fire-combs at cross-difference >= 2,
  // so no evaluation ever coincides with the other child's arrival:
  // each level doubles the per-node toggle count (2^j toggles x
  // 2^(9-j) nodes = 512 events per level, 9 levels = 4608) plus 512
  // seeds and 512 leaf fires — past kMaxTimingEvents. Breach refuses
  // loudly with no partial trace (netlist/state/output untouched:
  // the adapter refuses before printing).
  std::vector<std::uint16_t> level;
  level.reserve(512);
  for (int k = 0; k < 512; ++k) {
    const std::uint16_t b = logic::add_gate(nl, logic::GateKind::kBuf, 1).value;
    nl.gates[b].propagation_delay_ps = static_cast<std::uint64_t>(2 * k + 1);
    level.push_back(b);
  }
  while (level.size() > 1) {
    std::vector<std::uint16_t> next;
    next.reserve(level.size() / 2);
    for (std::size_t i = 0; i < level.size(); i += 2) {
      const std::uint16_t a = logic::add_gate(nl, logic::GateKind::kXor, 2).value;
      assert(logic::wire_pins(nl, logic::PinRef{.slot = level[i], .is_output = true}, logic::PinRef{.slot = a}).ok());
      assert(logic::wire_pins(nl, logic::PinRef{.slot = level[i + 1], .is_output = true},
                              logic::PinRef{.slot = a, .is_output = false, .input = 1})
                 .ok());
      next.push_back(a);
    }
    level = next;
  }
  assert(nl.live_count == 1023);
  logic::TimingScratch scratch{};
  std::vector<LogicValue> zero(512, logic::LogicValue::kLow);
  assert(logic::timing_settle_initial(nl, zero, scratch, logic::EdgeFilter::kAll).ok());
  std::vector<logic::TimingSeed> seeds;
  seeds.reserve(512);
  for (std::uint16_t s = 0; s < 512; ++s) {
    seeds.push_back(logic::TimingSeed{.slot = s, .input = 0, .is_flop = false, .fire_ps = 0, .new_value = H});
  }
  logic::TimingTrace trace;
  const core::Status st = logic::run_timing_window(nl, seeds, trace, scratch, false);
  assert(!st.ok() && st.code == core::StatusCode::kInternal);
  assert(trace.n_events == 0 && trace.drain_ps == 0); // no partial trace
}

// Time-axis overflow refuses loudly (never wraps): a gate annotated
// near the u64 cap fires its event at t=MAX, and scheduling its
// consumer at MAX+1 overflows.
void test_time_overflow() {
  logic::Netlist nl;
  const std::uint16_t g1 = logic::add_gate(nl, logic::GateKind::kNot, 1).value;
  const std::uint16_t g2 = logic::add_gate(nl, logic::GateKind::kNot, 1).value;
  assert(logic::wire_pins(nl, logic::PinRef{.slot = g1, .is_output = true}, logic::PinRef{.slot = g2}).ok());
  nl.gates[g1].propagation_delay_ps = 0xFFFFFFFFFFFFFFFFULL;
  logic::TimingScratch scratch{};
  const std::vector<LogicValue> zero{logic::LogicValue::kLow};
  assert(logic::timing_settle_initial(nl, zero, scratch, logic::EdgeFilter::kAll).ok());
  const std::vector<logic::TimingSeed> seeds{
      logic::TimingSeed{.slot = g1, .is_flop = false, .fire_ps = 0, .new_value = H},
  };
  logic::TimingTrace trace;
  const core::Status st = logic::run_timing_window(nl, seeds, trace, scratch, false);
  assert(!st.ok() && st.code == core::StatusCode::kInternal);
  assert(trace.n_events == 0 && trace.drain_ps == 0); // no partial trace
}

// US3 (T031): step CLI — one window per edge, NQ pairing, settled
// tail byte-identical, trajectories equal across modes.
void test_step_with_delays_cli() {
  std::string text;
  assert(run_line("logic reset", text) == 0);
  assert(run_line("logic add DFF", text) == 0);
  assert(run_line("logic add NOT", text) == 0);
  assert(run_line("logic wire NOT1.Y DFF1.D", text) == 0);
  assert(run_line("logic wire DFF1.Q NOT1.1", text) == 0);
  assert(run_line("logic set-delay DFF1 7ps", text) == 0);
  assert(run_line("logic step --with-delays", text) == 0);
  assert(text == "clk: cycle 1 (1 edges)\n"
                 "timing (edge 1): unit 1ps, 1 annotated\n"
                 "  t=7ps DFF1.Q: 0->1\n"
                 "  t=7ps DFF1.NQ: 1->0\n"
                 "  t=8ps NOT1.Y: 1->0\n"
                 "settled in 8ps\n"
                 "DFF1.Q: 0->1 (NQ 1->0)\n");
  assert(run_line("logic peek", text) == 0);
  const std::string peek_timed = text;
  // Same trajectory without the flag.
  assert(run_line("logic reset", text) == 0);
  assert(run_line("logic add DFF", text) == 0);
  assert(run_line("logic add NOT", text) == 0);
  assert(run_line("logic wire NOT1.Y DFF1.D", text) == 0);
  assert(run_line("logic wire DFF1.Q NOT1.1", text) == 0);
  assert(run_line("logic set-delay DFF1 7ps", text) == 0);
  assert(run_line("logic step", text) == 0);
  const std::string step_plain = text;
  assert(run_line("logic peek", text) == 0);
  assert(text == peek_timed); // trajectories equal step-for-step
  // The flag-off tail matches the flag-on tail after the windows.
  assert(run_line("logic reset", text) == 0);
  assert(run_line("logic add DFF", text) == 0);
  assert(run_line("logic add NOT", text) == 0);
  assert(run_line("logic wire NOT1.Y DFF1.D", text) == 0);
  assert(run_line("logic wire DFF1.Q NOT1.1", text) == 0);
  assert(run_line("logic set-delay DFF1 7ps", text) == 0);
  assert(run_line("logic step --cycles 2 --with-delays", text) == 0);
  assert(text == "clk: cycle 2 (2 edges)\n"
                 "timing (edge 1): unit 1ps, 1 annotated\n"
                 "  t=7ps DFF1.Q: 0->1\n"
                 "  t=7ps DFF1.NQ: 1->0\n"
                 "  t=8ps NOT1.Y: 1->0\n"
                 "settled in 8ps\n"
                 "timing (edge 2): unit 1ps, 1 annotated\n"
                 "  t=7ps DFF1.Q: 1->0\n"
                 "  t=7ps DFF1.NQ: 0->1\n"
                 "  t=8ps NOT1.Y: 0->1\n"
                 "settled in 8ps\n"
                 "DFF1.Q: 0 (no change)\n");
  assert(step_plain == "clk: cycle 1 (1 edges)\nDFF1.Q: 0->1 (NQ 1->0)\n");
  // n=0 keeps the no-op report (no edges, no windows).
  assert(run_line("logic step --cycles 0 --with-delays", text) == 0);
  assert(text.find("timing") == std::string::npos);
  assert(text.find("no-op") != std::string::npos);
  // Settled refusal rides the unchanged path identically in both
  // modes (released TBUF drives D with Z): same message, same exit,
  // workspace untouched.
  assert(run_line("logic reset", text) == 0);
  assert(run_line("logic add TBUF", text) == 0);
  assert(run_line("logic add DFF", text) == 0);
  assert(run_line("logic wire TBUF1.Y DFF1.D", text) == 0);
  assert(run_line("logic step", text) == 1);
  const std::string refuse_plain = text;
  assert(run_line("logic step --with-delays", text) == 1);
  assert(text == refuse_plain);
  assert(run_line("logic peek", text) == 0);
  const std::string peek_after = text;
  assert(run_line("logic reset", text) == 0);
  assert(run_line("logic add TBUF", text) == 0);
  assert(run_line("logic add DFF", text) == 0);
  assert(run_line("logic wire TBUF1.Y DFF1.D", text) == 0);
  assert(run_line("logic peek", text) == 0);
  assert(text == peek_after); // state untouched by either refusal
  assert(run_line("logic reset", text) == 0);
}

// US4 (T034): one vocabulary — the Shell dispatch path and the
// direct spec entry (what headless argv calls) print byte-identical
// text for the same timing invocation (the standing parity
// construction; no TUI change ships).
void test_session_direct_parity() {
  std::string text;
  assert(run_line("logic reset", text) == 0);
  assert(run_line("logic add DFF", text) == 0);
  assert(run_line("logic add NOT", text) == 0);
  assert(run_line("logic wire NOT1.Y DFF1.D", text) == 0);
  assert(run_line("logic wire DFF1.Q NOT1.1", text) == 0);
  assert(run_line("logic set-delay DFF1 7ps", text) == 0);
  assert(run_line("logic step --with-delays", text) == 0);
  const std::string via_shell = text;
  // Rebuild identically, then call the spec entry directly.
  assert(run_line("logic reset", text) == 0);
  assert(run_line("logic add DFF", text) == 0);
  assert(run_line("logic add NOT", text) == 0);
  assert(run_line("logic wire NOT1.Y DFF1.D", text) == 0);
  assert(run_line("logic wire DFF1.Q NOT1.1", text) == 0);
  assert(run_line("logic set-delay DFF1 7ps", text) == 0);
  std::ostringstream direct;
  const std::string_view args[] = {"--with-delays"};
  wengine::cli::ArgCursor cursor{std::span<const std::string_view>{args, 1}};
  assert(wengine::cli::logic::LogicStepSpec::run(cursor, direct) == 0);
  assert(direct.str() == via_shell);
  // Same for solve.
  assert(run_line("logic reset", text) == 0);
  assert(run_line(std::string("logic load ") + WENGINE_LOGIC_EXAMPLE_DIR "/glitch_chain.yaml", text) == 0);
  assert(run_line("logic solve --in 1 --with-delays", text) == 0);
  const std::string solve_shell = text;
  assert(run_line("logic reset", text) == 0);
  assert(run_line(std::string("logic load ") + WENGINE_LOGIC_EXAMPLE_DIR "/glitch_chain.yaml", text) == 0);
  std::ostringstream solve_direct;
  const std::string_view sargs[] = {"--in", "1", "--with-delays"};
  wengine::cli::ArgCursor scursor{std::span<const std::string_view>{sargs, 3}};
  assert(wengine::cli::logic::LogicSolveSpec::run(scursor, solve_direct) == 0);
  assert(solve_direct.str() == solve_shell);
  // Same for truth (values plus settle column).
  assert(run_line("logic truth --with-delays", text) == 0);
  const std::string truth_shell = text;
  std::ostringstream truth_direct;
  const std::string_view targs[] = {"--with-delays"};
  wengine::cli::ArgCursor tcursor{std::span<const std::string_view>{targs, 1}};
  assert(wengine::cli::logic::LogicTruthSpec::run(tcursor, truth_direct) == 0);
  assert(truth_direct.str() == truth_shell);
  assert(run_line("logic reset", text) == 0);
}

// US4 (T035): the selection never sticks — per-invocation only.
// Flag-on then flag-off reproduces the default; save/load carries
// annotations (013) but no timing state (schema untouched).
void test_mode_never_persists() {
  std::string text;
  const std::string ws = "/tmp/wengine_timing_persist.yaml";
  assert(run_line(std::string("logic load ") + WENGINE_LOGIC_EXAMPLE_DIR "/glitch_chain.yaml", text) == 0);
  assert(run_line("logic set-delay NOT1 5ps", text) == 0);
  assert(run_line("logic solve --in 1 --with-delays", text) == 0);
  const std::string timed = text;
  assert(timed.find("timing: unit 1ps, 1 annotated") != std::string::npos);
  // Next invocation starts in default mode (nothing stuck).
  assert(run_line("logic solve --in 1", text) == 0);
  assert(text.find("timing:") == std::string::npos);
  assert(text == "in: 1 (NOT1.1=1)\nAND1.Y=0\n");
  // Save/load round-trips annotations (013); the mode itself is not
  // serialized — the reloaded net replays the same trace, and the
  // file carries no timing-mode state (still `version: 1`).
  std::remove(ws.c_str());
  assert(run_line(std::string("logic save ") + ws, text) == 0);
  {
    std::ostringstream saved;
    std::ifstream f(ws, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    assert(content.find("version: 1") == 0);
    assert(content.find("delay_ps: 5") != std::string::npos);
    assert(content.find("timing") == std::string::npos);
    (void)saved;
  }
  assert(run_line("logic reset", text) == 0);
  assert(run_line(std::string("logic load ") + ws, text) == 0);
  assert(run_line("logic solve --in 1 --with-delays", text) == 0);
  assert(text == timed); // identical replay, mode selected afresh
  std::remove(ws.c_str());
  assert(run_line("logic reset", text) == 0);
}

// US4 (T036): refusals follow the module conventions — misspelled
// flag is a usage error (exit 2) with the workspace untouched; the
// wheel's budget/overflow failures are runtime errors (exit 1) with
// no trace block printed and state untouched.
void test_timing_refusal_conventions() {
  std::string text;
  assert(run_line(std::string("logic load ") + WENGINE_LOGIC_EXAMPLE_DIR "/glitch_chain.yaml", text) == 0);
  assert(run_line("logic solve --in 1", text) == 0);
  const std::string before = text;
  assert(run_line("logic solve --in 1 --with-del", text) == 2);
  assert(run_line("logic solve --in 1", text) == 0);
  assert(text == before); // workspace untouched by the usage error
  // Time-axis overflow at the CLI: hour-scale-free, u64-max annotation
  // on a two-NOT chain; the settled solve still succeeds flag-off.
  assert(run_line("logic reset", text) == 0);
  assert(run_line("logic add NOT", text) == 0);
  assert(run_line("logic add NOT", text) == 0);
  assert(run_line("logic wire NOT1.Y NOT2.1", text) == 0);
  assert(run_line("logic set-delay NOT1 18446744073709551615", text) == 0);
  assert(run_line("logic solve --in 1", text) == 0);
  assert(text == "in: 1 (NOT1.1=1)\nNOT2.Y=1\n");
  assert(run_line("logic solve --in 1 --with-delays", text) == 1);
  assert(text.find("timing window refused") != std::string::npos);
  assert(text.find("t=") == std::string::npos); // no partial block
  assert(run_line("logic solve --in 1", text) == 0); // still fine
  assert(text == "in: 1 (NOT1.1=1)\nNOT2.Y=1\n");
  assert(run_line("logic reset", text) == 0);
}

// US5 (T039): truth drain goldens — each row's settle equals its
// hand-derived longest activated annotated path; values identical
// to the flag-off table.
void test_truth_settle_cli() {
  std::string text;
  assert(run_line(std::string("logic load ") + WENGINE_LOGIC_EXAMPLE_DIR "/glitch_chain.yaml", text) == 0);
  assert(run_line("logic truth", text) == 0);
  const std::string plain = text;
  assert(run_line("logic truth --with-delays", text) == 0);
  // Row 0 (E=0): empty window, drain 0. Row 1 (E=1): the 4ps pulse.
  assert(text == "NOT1.1 | AND1.Y | settle\n"
                 "-------+--------+-------\n"
                 "  0    |   0    |      0\n"
                 "  1    |   0    |      4\n");
  // Values identical to the flag-off table (strip the settle column).
  assert(plain == "NOT1.1 | AND1.Y\n-------+-------\n  0    |   0   \n  1    |   0   \n");
  // CSV form gains the trailing settle_ps column; summary unchanged.
  const std::string csv = "/tmp/wengine_timing_truth.csv";
  std::remove(csv.c_str());
  assert(run_line(std::string("logic truth --csv ") + csv + " --with-delays", text) == 0);
  assert(text == "wrote 2 rows to '" + csv + "' (1 inputs, 1 outputs)\n");
  {
    std::ifstream f(csv, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    assert(content == "NOT1.1,AND1.Y,settle_ps\n0,0,0\n1,0,4\n");
  }
  std::remove(csv.c_str());
  // Flag-off CSV has no settle column.
  assert(run_line(std::string("logic truth --csv ") + csv, text) == 0);
  {
    std::ifstream f(csv, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    assert(content == "NOT1.1,AND1.Y\n0,0\n1,0\n");
  }
  std::remove(csv.c_str());
  assert(run_line("logic reset", text) == 0);
}

// US5 (T040): standing refusals inherit identically — shared nets
// with 2+ attachments and live buffers refuse the word path the
// same way with and without the flag.
void test_truth_refusal_inheritance() {
  std::string text;
  assert(run_line("logic reset", text) == 0);
  assert(run_line("logic add AND", text) == 0);
  assert(run_line("logic add AND", text) == 0);
  assert(run_line("logic add OR", text) == 0);
  assert(run_line("logic wire AND1.Y OR1.1", text) == 0);
  assert(run_line("logic wire AND2.Y OR1.1 --shared", text) == 0);
  assert(run_line("logic truth", text) == 1);
  const std::string plain = text;
  assert(run_line("logic truth --with-delays", text) == 1);
  assert(text == plain);
  assert(run_line("logic reset", text) == 0);
  assert(run_line("logic add TBUF", text) == 0);
  assert(run_line("logic truth", text) == 1);
  const std::string buf_plain = text;
  assert(run_line("logic truth --with-delays", text) == 1);
  assert(text == buf_plain);
  assert(run_line("logic reset", text) == 0);
}

// Convergence F1 (T048): shared-net transient contention in timing
// mode. Two drivers on one net with staggered delays: AND2 (unit)
// lands at t=1 while AND1 (5ps) still drives 0, so the shipped
// funnel resolves X over t=1..5 and NOT1.Y shows 1->X at t=2,
// closing X->0 at t=6 once both drivers agree. Hand-derived from
// path-delay arithmetic + the funnel's contention rule ({0,1} -> X):
// never implementation readback. FR-009.
void test_shared_contention_transient_window() {
  std::string text;
  assert(run_line("logic reset", text) == 0);
  assert(run_line("logic add AND", text) == 0);
  assert(run_line("logic add AND", text) == 0);
  assert(run_line("logic add NOT", text) == 0);
  assert(run_line("logic wire AND1.Y NOT1.1", text) == 0);
  assert(run_line("logic wire AND2.Y NOT1.1 --shared", text) == 0);
  assert(run_line("logic set-delay AND1 5ps", text) == 0);
  const std::string from = "logic solve --from AND1.1=1 --from AND1.2=1 --from AND2.1=1 --from AND2.2=1";
  assert(run_line(from + " --with-delays", text) == 0);
  const std::string want = "in: 1111 (AND1.1=1 AND1.2=1 AND2.1=1 AND2.2=1)\n"
                           "timing: unit 1ps, 1 annotated\n"
                           "  t=0ps AND1.1: 0->1\n"
                           "  t=0ps AND1.2: 0->1\n"
                           "  t=0ps AND2.1: 0->1\n"
                           "  t=0ps AND2.2: 0->1\n"
                           "  t=1ps AND2.Y: 0->1\n"
                           "  t=2ps NOT1.Y: 1->X\n"
                           "  t=5ps AND1.Y: 0->1\n"
                           "  t=6ps NOT1.Y: X->0\n"
                           "settled in 6ps\n"
                           "NOT1.Y=0\n"
                           "contention: NOT1.1=0 (total 0)\n";
  assert(text == want);
  // Settled tail byte-identical to the flag-off run: the trace adds
  // transients, never different endpoints.
  assert(run_line(from, text) == 0);
  assert(text == "in: 1111 (AND1.1=1 AND1.2=1 AND2.1=1 AND2.2=1)\n"
                 "NOT1.Y=0\n"
                 "contention: NOT1.1=0 (total 0)\n");
  assert(run_line("logic reset", text) == 0);
}

// Convergence F2 (T049): race_window.yaml replay pins the shipped
// sequential demo. Asserts the header's expected trace verbatim
// (clock-to-Q 7ps, NQ pairing Q's timestamp, BUF1 3ps downstream
// arrival at t=10, drain 10ps, unchanged settled tail) plus the
// header's second-step capture of the arrived 1 into DFF2.Q. FR-014.
void test_race_window_example_replay() {
  std::string text;
  assert(run_line(std::string("logic load ") + WENGINE_LOGIC_EXAMPLE_DIR "/race_window.yaml", text) == 0);
  assert(run_line("logic poke DFF1.D=1", text) == 0);
  assert(text == "poked DFF1.D=1 (leader DFF1.D)\n");
  assert(run_line("logic step --with-delays", text) == 0);
  assert(text == "clk: cycle 1 (1 edges)\n"
                 "timing (edge 1): unit 1ps, 2 annotated\n"
                 "  t=7ps DFF1.Q: 0->1\n"
                 "  t=7ps DFF1.NQ: 1->0\n"
                 "  t=10ps BUF1.Y: 0->1\n"
                 "settled in 10ps\n"
                 "DFF1.Q: 0->1 (NQ 1->0)\n"
                 "DFF2.Q: 0 (no change)\n"
                 "DFF2.Q=0\n");
  assert(run_line("logic step", text) == 0);
  assert(text == "clk: cycle 2 (1 edges)\n"
                 "DFF1.Q: 1 (no change)\n"
                 "DFF2.Q: 0->1 (NQ 1->0)\n"
                 "DFF2.Q=1\n");
  assert(run_line("logic reset", text) == 0);
}

// Dangling-idle prelude parity (the twin-deletion pin): the settled
// wheel rides the single BlockInterface seam — `timing_settle_initial`
// blk output is element-identical to `fill_resolved` over a net
// exercising every dangling kind (block input, tied pair, ignored,
// pull-up, pull-down, wired). Pulls/ties/ignores are disjoint by
// construction, so each needs its own gate here.
void test_settle_prelude_parity_with_fill_resolved() {
  logic::Netlist nl;
  const std::uint16_t a = logic::add_gate(nl, logic::GateKind::kAnd, 2).value;
  const std::uint16_t b = logic::add_gate(nl, logic::GateKind::kOr, 2).value;
  const std::uint16_t c = logic::add_gate(nl, logic::GateKind::kNot, 1).value;
  const std::uint16_t d = logic::add_gate(nl, logic::GateKind::kNot, 1).value;
  const std::uint16_t e = logic::add_gate(nl, logic::GateKind::kBuf, 1).value;
  assert(logic::tie_pins(nl, logic::PinRef{.slot = a, .is_output = false, .input = 1},
                         logic::PinRef{.slot = b, .is_output = false, .input = 0})
             .ok());
  assert(logic::ignore_pin(nl, logic::PinRef{.slot = b, .is_output = false, .input = 1}).ok());
  assert(logic::set_pull(nl, c, 0, 1).ok()); // up -> idle High
  assert(logic::set_pull(nl, d, 0, 2).ok()); // down -> idle Low
  assert(logic::wire_pins(nl, logic::PinRef{.slot = a, .is_output = true},
                          logic::PinRef{.slot = e, .is_output = false, .input = 0})
             .ok());
  // Interface order in slot order: A.1 (ord 0), A.2/B.1 tie leader
  // (ord 1). Pulled/ignored/wired pins consume no column.
  assert(logic::block_input_count(nl) == 2);
  const std::vector<LogicValue> inputs{H, L};
  LogicValue resolved[logic::kMaxGates * logic::kMaxInputs];
  assert(logic::fill_resolved(nl, inputs, resolved).ok());
  // Spot the idle rule before the elementwise comparison.
  assert(resolved[logic::tie_flat(a, 0)] == H);
  assert(resolved[logic::tie_flat(a, 1)] == L);
  assert(resolved[logic::tie_flat(b, 0)] == L); // tied member fans out
  assert(resolved[logic::tie_flat(b, 1)] == L); // ignored ties off
  assert(resolved[logic::tie_flat(c, 0)] == H); // pull-up terminates
  assert(resolved[logic::tie_flat(d, 0)] == L); // pull-down terminates
  logic::TimingScratch scratch{};
  for (auto &v : scratch.cur) {
    v = L;
  }
  assert(logic::timing_settle_initial(nl, inputs, scratch, logic::EdgeFilter::kAll).ok());
  for (std::size_t f = 0; f < logic::kMaxGates * logic::kMaxInputs; ++f) {
    assert(scratch.blk[f] == resolved[f]);
  }
  // Settled outputs agree with the zero-delay solve on the same
  // assignment (AND(H,L)=0, OR(L,L)=0, NOT(1)=0, NOT(0)=1, BUF(0)=0).
  assert(scratch.cur[a] == L && scratch.cur[b] == L && scratch.cur[c] == L && scratch.cur[d] == H &&
         scratch.cur[e] == L);
  std::vector<LogicValue> ref_out(logic::block_output_count(nl), L);
  assert(logic::solve(nl, inputs, ref_out, nullptr).ok());
  assert(ref_out.size() == 4); // B.Y, C.Y, D.Y, E.Y (A.Y is wired)
  const std::vector<LogicValue> want{L, L, H, L};
  for (std::size_t k = 0; k < want.size(); ++k) {
    assert(ref_out[k] == want[k]);
  }
}

// Pull-up timing golden (the defect class the twin shipped):
// a pulled-up dangling input terminates at High in the settle, so the
// wheel end-state matches `solve()` — previously the wheel read Low
// (NOT(0)=1 vs the correct NOT(1)=0). Hand-derived, never readback.
void test_settle_pull_up_golden() {
  // Terminated NOT: no block inputs, empty window drains 0.
  {
    logic::Netlist nl;
    const std::uint16_t n = logic::add_gate(nl, logic::GateKind::kNot, 1).value;
    assert(logic::set_pull(nl, n, 0, 1).ok());
    assert(logic::block_input_count(nl) == 0);
    const std::vector<LogicValue> empty;
    std::vector<LogicValue> ref_out(1, L);
    assert(logic::solve(nl, empty, ref_out, nullptr).ok());
    assert(ref_out[0] == L); // NOT(1)=0
    logic::TimingScratch scratch{};
    for (auto &v : scratch.cur) {
      v = L;
    }
    assert(logic::timing_settle_initial(nl, empty, scratch, logic::EdgeFilter::kAll).ok());
    assert(scratch.blk[logic::tie_flat(n, 0)] == H);
    assert(scratch.cur[n] == L);
    assert(scratch.cur[n] == ref_out[0]); // wheel/solve parity
    const std::vector<logic::TimingSeed> none;
    logic::TimingTrace trace;
    assert(logic::run_timing_window(nl, none, trace, scratch, false).ok());
    assert(trace.n_events == 0 && trace.drain_ps == 0);
  }
  // Pull-down mirrors: NOT(0)=1.
  {
    logic::Netlist nl;
    const std::uint16_t n = logic::add_gate(nl, logic::GateKind::kNot, 1).value;
    assert(logic::set_pull(nl, n, 0, 2).ok());
    const std::vector<LogicValue> empty;
    logic::TimingScratch scratch{};
    for (auto &v : scratch.cur) {
      v = L;
    }
    assert(logic::timing_settle_initial(nl, empty, scratch, logic::EdgeFilter::kAll).ok());
    assert(scratch.blk[logic::tie_flat(n, 0)] == L);
    assert(scratch.cur[n] == H); // NOT(0)=1
  }
  // Driven golden: AND(pull-up, block input). Pre (in=0): AND(1,0)=0;
  // seed in 0->1 at t=0, AND.Y 0->1 at t=1 (unit delay), drain 1.
  {
    logic::Netlist nl;
    const std::uint16_t g = logic::add_gate(nl, logic::GateKind::kAnd, 2).value;
    assert(logic::set_pull(nl, g, 0, 1).ok());
    assert(logic::block_input_count(nl) == 1);
    const std::vector<LogicValue> zero{L};
    const std::vector<LogicValue> one{H};
    std::vector<LogicValue> ref0(1, L), ref1(1, L);
    assert(logic::solve(nl, zero, ref0, nullptr).ok());
    assert(logic::solve(nl, one, ref1, nullptr).ok());
    assert(ref0[0] == L && ref1[0] == H);
    logic::TimingScratch scratch{};
    for (auto &v : scratch.cur) {
      v = L;
    }
    assert(logic::timing_settle_initial(nl, zero, scratch, logic::EdgeFilter::kAll).ok());
    assert(scratch.cur[g] == L);
    const std::vector<logic::TimingSeed> seeds{
        logic::TimingSeed{.slot = g, .input = 1, .is_flop = false, .fire_ps = 0, .new_value = H},
    };
    logic::TimingTrace trace;
    assert(logic::run_timing_window(nl, seeds, trace, scratch, false).ok());
    const ExpectedEvent want[] = {
        {0, 0, 1, true, false, L, H},  // t=0 AND1.2: 0->1 (seed)
        {1, 0, 0, false, false, L, H}, // t=1 AND1.Y: 0->1 (unit delay)
    };
    check_trace(trace, want, 2, 1, 0);
    assert(scratch.cur[g] == ref1[0]); // end-state parity with solve
  }
}

} // namespace

int main() {
  test_pulse_unit_golden();
  fprintf(stderr, "pulse_unit: OK\n");
  test_pulse_annotated_golden();
  fprintf(stderr, "pulse_annotated: OK\n");
  test_transport_no_cancellation();
  fprintf(stderr, "transport: OK\n");
  test_unit_floor();
  fprintf(stderr, "unit_floor: OK\n");
  test_same_instant_order_and_determinism();
  fprintf(stderr, "order: OK\n");
  test_wheel_end_state_parity();
  fprintf(stderr, "parity: OK\n");
  test_solve_with_delays_cli();
  fprintf(stderr, "solve_cli: OK\n");
  test_flag_off_identity();
  fprintf(stderr, "flag_off: OK\n");
  test_annotation_blindness();
  fprintf(stderr, "blindness: OK\n");
  test_edge_flop_clock_to_q();
  fprintf(stderr, "clock_to_q: OK\n");
  test_d_arrival_race_offset();
  fprintf(stderr, "race_offset: OK\n");
  test_latch_response_delay();
  fprintf(stderr, "latch: OK\n");
  test_transient_and_dominant_zero();
  fprintf(stderr, "transient0: OK\n");
  test_transient_and_unknown();
  fprintf(stderr, "transientX: OK\n");
  test_transient_sr_both_high();
  fprintf(stderr, "transientSR: OK\n");
  test_race_determinism();
  fprintf(stderr, "race_determinism: OK\n");
  test_event_cap_breach();
  fprintf(stderr, "cap: OK\n");
  test_time_overflow();
  fprintf(stderr, "overflow: OK\n");
  test_step_with_delays_cli();
  fprintf(stderr, "step_cli: OK\n");
  test_session_direct_parity();
  fprintf(stderr, "parity: OK\n");
  test_mode_never_persists();
  fprintf(stderr, "nopersist: OK\n");
  test_timing_refusal_conventions();
  fprintf(stderr, "refusals: OK\n");
  test_truth_settle_cli();
  fprintf(stderr, "truth: OK\n");
  test_truth_refusal_inheritance();
  fprintf(stderr, "truth_refusals: OK\n");
  test_shared_contention_transient_window();
  fprintf(stderr, "contention: OK\n");
  test_race_window_example_replay();
  fprintf(stderr, "race_replay: OK\n");
  test_settle_prelude_parity_with_fill_resolved();
  fprintf(stderr, "prelude_parity: OK\n");
  test_settle_pull_up_golden();
  fprintf(stderr, "pullup: OK\n");
  std::cout << "test_logic_timing: OK\n";
  return 0;
}
