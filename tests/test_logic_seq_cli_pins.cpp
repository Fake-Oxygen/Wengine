// Named sequential pins + NQ negated output (FR-010/FR-011).
//
// Links `wengine_logic_yaml` (save/load round-trip through the versioned
// store) plus the header-only engine (parse, wire, solve, seq_step):
// alias parsing both directions, wrong-kind rejections, NQ inversion
// goldens (power-on + post-step + wired-sink scalar/word solve), fanout
// and interface counts with NQ wired/dangling/ignored, and a YAML
// round-trip carrying NQ sources, named pins, and the ignore_nq key.

#include "wengine/logic/eval.hpp"
#include "wengine/logic/netlist.hpp"
#include "wengine/logic/seq_state.hpp"
#include "wengine/logic/seq_step.hpp"
#include "wengine/logic/types.hpp"
#include "wengine/logic/yaml_store.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>

using namespace wengine;
using wengine::logic::GateKind;
using wengine::logic::LogicValue;
using wengine::logic::Netlist;
using wengine::logic::SeqState;

namespace {

constexpr LogicValue kL = LogicValue::kLow;
constexpr LogicValue kH = LogicValue::kHigh;

bool contains(const std::string &hay, const std::string &needle) {
  return hay.find(needle) != std::string::npos;
}

// Extended structural equality: the T005 state (polarity, NQ fanout,
// NQ ignore, full 18-wide counters) must round-trip exactly.
bool netlists_equal_full(const Netlist &a, const Netlist &b) {
  if (a.live_count != b.live_count) {
    return false;
  }
  for (std::size_t k = 0; k < a.next_counter.size(); ++k) {
    if (a.next_counter[k] != b.next_counter[k]) {
      return false;
    }
  }
  for (std::size_t f = 0; f < logic::kMaxGates * logic::kMaxInputs; ++f) {
    if (a.tie_rep[f] != b.tie_rep[f] || a.driver_inv[f] != b.driver_inv[f]) {
      return false;
    }
  }
  for (std::uint16_t s = 0; s < logic::kMaxGates; ++s) {
    const logic::Gate &x = a.gates[s];
    const logic::Gate &y = b.gates[s];
    if (x.live != y.live || x.kind != y.kind || x.counter != y.counter ||
        x.input_count != y.input_count || x.ignored_mask != y.ignored_mask ||
        x.ignore_output != y.ignore_output || x.ignore_nq_output != y.ignore_nq_output ||
        x.output_fanout != y.output_fanout || x.nq_fanout != y.nq_fanout ||
        x.propagation_delay_ps != y.propagation_delay_ps) {
      return false;
    }
    for (std::size_t i = 0; i < logic::kMaxInputs; ++i) {
      if (x.drivers[i] != y.drivers[i]) {
        return false;
      }
    }
  }
  return true;
}

void test_named_parse() {
  Netlist nl;
  const std::uint16_t d = logic::add_gate(nl, GateKind::kDff, 2).value;
  const std::uint16_t l = logic::add_gate(nl, GateKind::kDlatch, 2).value;
  const std::uint16_t a = logic::add_gate(nl, GateKind::kAnd, 2).value;
  (void)a;
  // Named forms resolve to the right slots/inputs (case-insensitive).
  auto dd = logic::parse_pin(nl, "DFF1.D");
  assert(dd.ok() && dd.value.slot == d && !dd.value.is_output && dd.value.input == 0);
  auto dc = logic::parse_pin(nl, "DFF1.CLK");
  assert(dc.ok() && dc.value.slot == d && !dc.value.is_output && dc.value.input == 1);
  auto dq = logic::parse_pin(nl, "DFF1.Q");
  assert(dq.ok() && dq.value.slot == d && dq.value.is_output && !dq.value.negated);
  auto dn = logic::parse_pin(nl, "DFF1.NQ");
  assert(dn.ok() && dn.value.slot == d && dn.value.is_output && dn.value.negated);
  auto ld = logic::parse_pin(nl, "dlatch1.d");
  assert(ld.ok() && ld.value.slot == l && !ld.value.is_output && ld.value.input == 0);
  auto le = logic::parse_pin(nl, "DLATCH1.en");
  assert(le.ok() && le.value.slot == l && !le.value.is_output && le.value.input == 1);
  auto ln = logic::parse_pin(nl, "Dlatch1.Nq");
  assert(ln.ok() && ln.value.slot == l && ln.value.is_output && ln.value.negated);
  // Numeric aliases keep working on sequential gates.
  auto d1 = logic::parse_pin(nl, "DFF1.1");
  assert(d1.ok() && d1.value.slot == d && !d1.value.is_output && d1.value.input == 0);
  auto d2 = logic::parse_pin(nl, "DFF1.2");
  assert(d2.ok() && d2.value.slot == d && !d2.value.is_output && d2.value.input == 1);
  auto dy = logic::parse_pin(nl, "DFF1.Y");
  assert(dy.ok() && dy.value.slot == d && dy.value.is_output && !dy.value.negated);
  // Named pins on combinational gates are loud errors.
  assert(!logic::parse_pin(nl, "AND1.D").ok());
  assert(!logic::parse_pin(nl, "AND1.CLK").ok());
  assert(!logic::parse_pin(nl, "AND1.Q").ok());
  assert(!logic::parse_pin(nl, "AND1.NQ").ok());
  // Out-of-range numerics stay loud.
  assert(!logic::parse_pin(nl, "DFF1.0").ok());
  assert(!logic::parse_pin(nl, "DFF1.3").ok());
  assert(!logic::parse_pin(nl, "NOPE1.D").ok());
}

void test_nq_wiring_counts() {
  Netlist nl;
  const std::uint16_t d = logic::add_gate(nl, GateKind::kDff, 2).value;
  const std::uint16_t n = logic::add_gate(nl, GateKind::kNot, 1).value;
  (void)n;
  // NQ sources wire by name through the text API.
  assert(logic::wire_text(nl, "DFF1.NQ", "NOT1.1").ok());
  const logic::Gate &g = nl.gates[d];
  assert(g.output_fanout == 0); // Q untouched
  assert(g.nq_fanout == 1);
  assert(nl.driver_inv[static_cast<std::size_t>(1) * logic::kMaxInputs + 0] != 0);
  // Q and NQ are both wired: the flop contributes no block outputs
  // (a dangling NQ would be hidden by the default ignore instead).
  assert(!logic::wire_text(nl, "DFF1.Q", "NOT1.1").ok()); // occupied input
  // Fresh flop with dangling Q: NQ is ignored by default, so one output.
  Netlist nl2;
  assert(logic::add_gate(nl2, GateKind::kDff, 2).ok());
  assert(logic::block_output_count(nl2) == 1); // Q only
  // Ignoring the already-ignored NQ is a no-op; `use` re-exposes it.
  assert(logic::ignore_pin(nl2, logic::parse_pin(nl2, "DFF1.NQ").value).ok());
  assert(logic::block_output_count(nl2) == 1);
  assert(logic::use_pin(nl2, logic::parse_pin(nl2, "DFF1.NQ").value).ok());
  assert(logic::block_output_count(nl2) == 2); // Q then NQ
  // Unwire clears the polarity with the wire.
  assert(logic::unwire(nl, 1, 0).ok());
  assert(nl.gates[d].nq_fanout == 0);
  assert(nl.driver_inv[static_cast<std::size_t>(1) * logic::kMaxInputs + 0] == 0);
  assert(logic::block_output_count(nl) == 2); // DFF.Q + NOT.Y (NQ ignored)
}

void test_nq_read_time_values() {
  // NQ is read-time NOT Q: power-on Low/High, then tracks every step.
  // NQ is explicitly `use`d here (ignored by default, so no output column).
  Netlist nl;
  assert(logic::add_gate(nl, GateKind::kDff, 2).ok());
  assert(logic::ignore_pin(nl, logic::parse_pin(nl, "DFF1.CLK").value).ok());
  assert(logic::use_pin(nl, logic::parse_pin(nl, "DFF1.NQ").value).ok());
  SeqState st;
  LogicValue d = kH;
  LogicValue out[2] = {kL, kL};
  assert(logic::seq_step(nl, st, std::span<const LogicValue>(&d, 1), std::span<LogicValue>(out, 2)).ok());
  assert(out[0] == kH); // Q
  assert(out[1] == kL); // NQ
  d = kL;
  assert(logic::seq_step(nl, st, std::span<const LogicValue>(&d, 1), std::span<LogicValue>(out, 2)).ok());
  assert(out[0] == kL);
  assert(out[1] == kH);
}

void test_nq_sink_solve() {
  // Wired-NQ inversion flows through scalar solve: flop Q reads Low in
  // combinational eval (eval_gate default), so NQ reads High.
  Netlist nl;
  assert(logic::add_gate(nl, GateKind::kDff, 2).ok());
  assert(logic::add_gate(nl, GateKind::kNot, 1).ok());
  assert(logic::ignore_pin(nl, logic::parse_pin(nl, "DFF1.CLK").value).ok());
  assert(logic::wire_text(nl, "DFF1.NQ", "NOT1.1").ok());
  // Block inputs: DFF1.D. Block outputs: DFF1.Q, NOT1.Y (NQ is wired).
  assert(logic::block_input_count(nl) == 1);
  assert(logic::block_output_count(nl) == 2);
  LogicValue in[1] = {kL};
  LogicValue out[2] = {kL, kL};
  assert(logic::solve(nl, std::span<const LogicValue>(in, 1), std::span<LogicValue>(out, 2)).ok());
  assert(out[0] == kL); // Q (combinational eval default)
  assert(out[1] == kL); // NOT(NQ=High) = Low; polarity ignored would give High
}

void test_nq_sink_word() {
  // Per-sink polarity in the bit-parallel path: NOT1 sees Q, NOT2 sees NQ.
  Netlist nl;
  assert(logic::add_gate(nl, GateKind::kDff, 2).ok());
  assert(logic::add_gate(nl, GateKind::kNot, 1).ok());
  assert(logic::add_gate(nl, GateKind::kNot, 1).ok());
  assert(logic::ignore_pin(nl, logic::parse_pin(nl, "DFF1.CLK").value).ok());
  assert(logic::wire_text(nl, "DFF1.Q", "NOT1.1").ok());
  assert(logic::wire_text(nl, "DFF1.NQ", "NOT2.1").ok());
  assert(logic::block_input_count(nl) == 1);   // DFF1.D
  assert(logic::block_output_count(nl) == 2);  // NOT1.Y, NOT2.Y
  std::uint64_t scratch[logic::kMaxGates] = {};
  std::uint64_t words[2] = {};
  assert(logic::eval_block_word(nl, 0, std::span<std::uint64_t>(scratch, logic::kMaxGates),
                                std::span<std::uint64_t>(words, 2))
             .ok());
  // 1 input -> 2 rows, mask 0b11. Q reads Low: NOT1 = ~0 & mask = 0b11,
  // NQ reads High: NOT2 = ~all-ones & mask = 0b00.
  assert((words[0] & 0x3ULL) == 0x3ULL);
  assert((words[1] & 0x3ULL) == 0x0ULL);
}

void test_yaml_nq_round_trip() {
  // BUF1 -> DFF1.D, DFF1.CLK ignored, DFF1.Q -> NOT1.1, DFF1.NQ -> AND1.1
  // with ignore_nq stored; AND1.2 + BUF1.1 dangle (block inputs).
  Netlist nl;
  assert(logic::add_gate(nl, GateKind::kBuf, 1).ok());
  assert(logic::add_gate(nl, GateKind::kDff, 2).ok());
  assert(logic::add_gate(nl, GateKind::kNot, 1).ok());
  assert(logic::add_gate(nl, GateKind::kAnd, 2).ok());
  assert(logic::wire_text(nl, "BUF1.Y", "DFF1.D").ok());
  assert(logic::ignore_pin(nl, logic::parse_pin(nl, "DFF1.CLK").value).ok());
  assert(logic::wire_text(nl, "DFF1.Q", "NOT1.1").ok());
  assert(logic::wire_text(nl, "DFF1.NQ", "AND1.1").ok());
  assert(logic::ignore_pin(nl, logic::parse_pin(nl, "DFF1.NQ").value).ok());
  assert(logic::block_input_count(nl) == 2);   // BUF1.1, AND1.2
  assert(logic::block_output_count(nl) == 2);  // NOT1.Y, AND1.Y
  std::string text;
  std::array<LogicValue, logic::kMaxGates> inits{};
  inits.fill(kL);
  logic::yaml::save_text(nl, inits, text);
  assert(contains(text, "to: DFF1.D"));
  assert(contains(text, "from: DFF1.NQ"));
  assert(contains(text, "ignore_nq: true"));
  assert(contains(text, "to: NOT1.1")); // combinational pins stay numeric
  logic::yaml::Workspace ws;
  std::string err;
  assert(logic::yaml::load_text(text, ws, err));
  assert(netlists_equal_full(nl, ws.netlist));
  std::string again;
  logic::yaml::save_text(ws.netlist, ws.init, again);
  assert(text == again); // byte-identical round-trip
  // Counters survive the trip: the next DFF is DFF2.
  assert(logic::add_gate(ws.netlist, GateKind::kDff, 2).ok());
  assert(ws.netlist.gates[4].counter == 2);
}

} // namespace

int main() {
  test_named_parse();
  test_nq_wiring_counts();
  test_nq_read_time_values();
  test_nq_sink_solve();
  test_nq_sink_word();
  test_yaml_nq_round_trip();
  std::cout << "test_logic_seq_cli_pins: OK\n";
  return 0;
}
