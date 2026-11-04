// LOGIC-6: versioned YAML workspace store (yaml-cpp backed).
//
// Links `wengine_logic_yaml` (the single TU allowed to know yaml-cpp
// exists) — never `wengine_core` alone, so the engine-header seam stays
// provable: `test_logic` still links core only (see CMakeLists), while
// this target pins the store contract: canonical bytes, byte-identical
// round-trip (ignore marks + fan-in + delay + flop inits + clock domains),
// legacy/foreign-version refusal, and loud `line N:` diagnostics for
// malformed workspaces.

#include "wengine/logic/eval.hpp"
#include "wengine/logic/netlist.hpp"
#include "wengine/logic/types.hpp"
#include "wengine/logic/yaml_store.hpp"

#include <array>
#include <cassert>
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

constexpr logic::LogicValue kL = logic::LogicValue::kLow;
constexpr logic::LogicValue kH = logic::LogicValue::kHigh;

bool contains(const std::string &hay, const std::string &needle) {
  return hay.find(needle) != std::string::npos;
}

// All-Low per-slot power-on values (fresh-attach default).
std::array<logic::LogicValue, logic::kMaxGates> zero_inits() {
  std::array<logic::LogicValue, logic::kMaxGates> in{};
  in.fill(logic::LogicValue::kLow);
  return in;
}

// AND1(2) -> NOT1.1, the canonical two-gate workspace.
logic::Netlist make_pair() {
  logic::Netlist nl;
  assert(logic::add_gate(nl, logic::GateKind::kAnd, 2).ok());
  assert(logic::add_gate(nl, logic::GateKind::kNot, 1).ok());
  assert(logic::wire(nl, 0, 1, 0).ok());
  return nl;
}

bool netlists_equal(const logic::Netlist &a, const logic::Netlist &b) {
  if (a.live_count != b.live_count) {
    return false;
  }
  for (std::size_t k = 0; k < 8; ++k) {
    if (a.next_counter[k] != b.next_counter[k]) {
      return false;
    }
  }
  for (std::size_t f = 0; f < logic::kMaxGates * logic::kMaxInputs; ++f) {
    if (a.tie_rep[f] != b.tie_rep[f]) {
      return false;
    }
  }
  for (std::uint16_t s = 0; s < logic::kMaxGates; ++s) {
    const logic::Gate &x = a.gates[s];
    const logic::Gate &y = b.gates[s];
    if (x.live != y.live || x.kind != y.kind || x.counter != y.counter ||
        x.input_count != y.input_count || x.ignored_mask != y.ignored_mask ||
        x.ignore_output != y.ignore_output || x.output_fanout != y.output_fanout ||
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

bool workspaces_equal(const logic::yaml::Workspace &a, const logic::yaml::Workspace &b) {
  if (!netlists_equal(a.netlist, b.netlist)) {
    return false;
  }
  for (std::size_t s = 0; s < logic::kMaxGates; ++s) {
    if (a.init[s] != b.init[s]) {
      return false;
    }
  }
  return true;
}

void test_canonical_bytes_v1() {
  // New-generation v1 layout: `version: 1`, required `ties:` + `clocks:`
  // (empty here — combinational net), per-sequential-row inits absent on
  // comb rows, canonical save of a user-superset file.
  const logic::Netlist nl = make_pair();
  std::string text;
  logic::yaml::save_text(nl, zero_inits(), text);
  const std::string want =
      "version: 1\n"
      "gates:\n"
      "  - {id: AND1, kind: and, inputs: 2, ignore: [], ignore_output: false, delay_ps: 0}\n"
      "  - {id: NOT1, kind: not, inputs: 1, ignore: [], ignore_output: false, delay_ps: 0}\n"
      "wires:\n"
      "  - {from: AND1.Y, to: NOT1.1}\n"
      "ties:\n"
      "clocks:\n"
      "meta:\n"
      "  tool: wengine-logic\n";
  assert(text == want);
  // Round-trips through the bundle with all-Low inits.
  logic::yaml::Workspace ws;
  std::string err;
  assert(logic::yaml::load_text(text, ws, err));
  assert(netlists_equal(nl, ws.netlist));
  for (std::size_t s = 0; s < logic::kMaxGates; ++s) {
    assert(ws.init[s] == kL);
  }
}

void test_init_round_trip() {
  // Mixed flop inits persist per sequential row and reload into the
  // bundle; re-save is byte-identical. Both flops dangle their clocks,
  // so they share the implicit `clk` domain.
  logic::Netlist nl;
  assert(logic::add_gate(nl, logic::GateKind::kDff, 2).ok()); // DFF1 @0
  assert(logic::add_gate(nl, logic::GateKind::kJk, 3).ok());  // JK1 @1
  auto inits = zero_inits();
  inits[0] = kH;
  inits[1] = kH;
  std::string text;
  logic::yaml::save_text(nl, inits, text);
  assert(contains(text, "version: 1\n"));
  assert(contains(text, "{id: DFF1, kind: dff, inputs: 2, ignore: [], ignore_output: false, "
                        "ignore_nq: true, delay_ps: 0, init: 1}"));
  assert(contains(text, "{id: JK1, kind: jk, inputs: 3, ignore: [], ignore_output: false, "
                        "ignore_nq: true, delay_ps: 0, init: 1}"));
  assert(contains(text, "clocks:\n  - {name: clk}\n"));
  logic::yaml::Workspace ws;
  std::string err;
  assert(logic::yaml::load_text(text, ws, err));
  assert(netlists_equal(nl, ws.netlist));
  assert(ws.init[0] == kH);
  assert(ws.init[1] == kH);
  for (std::size_t s = 2; s < logic::kMaxGates; ++s) {
    assert(ws.init[s] == kL);
  }
  std::string again;
  logic::yaml::save_text(ws.netlist, ws.init, again);
  assert(again == text); // byte-identical
}

void test_missing_init_defaults_low() {
  // Hand-written v1 without `init:` keys reads every slot Low; the
  // re-save then writes explicit `init: 0` (stability from the second
  // save, same caveat as the comment-superset convention).
  const std::string hand =
      "version: 1\n"
      "gates:\n"
      "  - {id: DFF1, kind: dff, inputs: 2, ignore: [], ignore_output: false, delay_ps: 0}\n"
      "wires:\n"
      "ties:\n"
      "clocks:\n"
      "  - {name: clk}\n"
      "meta:\n"
      "  tool: wengine-logic\n";
  logic::yaml::Workspace ws;
  std::string err;
  assert(logic::yaml::load_text(hand, ws, err));
  assert(ws.netlist.live_count == 1);
  for (std::size_t s = 0; s < logic::kMaxGates; ++s) {
    assert(ws.init[s] == kL);
  }
  std::string text;
  logic::yaml::save_text(ws.netlist, ws.init, text);
  assert(contains(text, "init: 0}"));
}

void test_bad_init_refusals() {
  // `init` on a combinational row, and non-binary values, are loud
  // load errors naming the gate; the bundle is untouched.
  {
    // Combinational row carrying `init`.
    const std::string text =
        "version: 1\ngates:\n"
        "  - {id: AND1, kind: and, inputs: 2, ignore: [], ignore_output: false, delay_ps: 0, init: 1}\n"
        "wires:\nties:\nclocks:\nmeta:\n  tool: wengine-logic\n";
    logic::yaml::Workspace ws;
    std::string err;
    assert(!logic::yaml::load_text(text, ws, err));
    assert(contains(err, "init needs a sequential gate (got 'AND1')"));
    assert(workspaces_equal(ws, logic::yaml::Workspace{})); // atomic
  }
  {
    // Non-binary value on a sequential row.
    const std::string text =
        "version: 1\ngates:\n"
        "  - {id: DFF1, kind: dff, inputs: 2, ignore: [], ignore_output: false, delay_ps: 0, init: 2}\n"
        "wires:\nties:\nclocks:\n  - {name: clk}\nmeta:\n  tool: wengine-logic\n";
    logic::yaml::Workspace ws;
    std::string err;
    assert(!logic::yaml::load_text(text, ws, err));
    assert(contains(err, "bad init value (want 0|1)"));
    assert(workspaces_equal(ws, logic::yaml::Workspace{})); // atomic
  }
}

void test_legacy_refusals() {
  // Both retired generations plus foreign versions are refused loudly,
  // naming the found version; the bundle is untouched on failure.
  logic::yaml::Workspace sentinel;
  sentinel.init.fill(kH); // poison: any commit would show
  const logic::yaml::Workspace before = sentinel;
  {
    // Legacy v1 shape: `version: 1` without the new required sections.
    const std::string legacy1 =
        "version: 1\n"
        "gates:\n"
        "  - {id: AND1, kind: and, inputs: 2, ignore: [], ignore_output: false, delay_ps: 0}\n"
        "wires:\n"
        "meta:\n"
        "  tool: wengine-logic\n";
    std::string err;
    assert(!logic::yaml::load_text(legacy1, sentinel, err));
    assert(contains(err, "legacy workspace version 1 is retired"));
    assert(workspaces_equal(sentinel, before)); // atomic
  }
  {
    // Legacy v2: has `ties:` but no `clocks:`.
    const std::string legacy2 =
        "version: 2\n"
        "gates:\n"
        "wires:\n"
        "ties:\n"
        "meta:\n"
        "  tool: wengine-logic\n";
    std::string err;
    assert(!logic::yaml::load_text(legacy2, sentinel, err));
    assert(contains(err, "workspace version 2 is retired (want 1"));
    assert(workspaces_equal(sentinel, before)); // atomic
  }
  {
    // Foreign future version.
    const std::string future =
        "version: 9\ngates:\nwires:\nties:\nclocks:\nmeta:\n  tool: wengine-logic\n";
    std::string err;
    assert(!logic::yaml::load_text(future, sentinel, err));
    assert(contains(err, "unsupported workspace version 9 (want 1)"));
    assert(workspaces_equal(sentinel, before)); // atomic
  }
}

void test_empty_round_trip() {
  logic::Netlist nl;
  std::string text;
  logic::yaml::save_text(nl, zero_inits(), text);
  assert(text == "version: 1\ngates:\nwires:\nties:\nclocks:\nmeta:\n  tool: wengine-logic\n");
  logic::yaml::Workspace ws;
  std::string err;
  assert(logic::yaml::load_text(text, ws, err));
  assert(ws.netlist.live_count == 0);
  std::string again;
  logic::yaml::save_text(ws.netlist, ws.init, again);
  assert(again == text);
}

void test_round_trip_ignore_fanin_delay() {
  // Ignore marks + wide fan-in + fan-out + stored (not honored) delay.
  logic::Netlist nl;
  assert(logic::add_gate(nl, logic::GateKind::kAnd, 8).ok()); // AND1 @0
  assert(logic::add_gate(nl, logic::GateKind::kOr, 2).ok());  // OR1 @1
  assert(logic::add_gate(nl, logic::GateKind::kNot, 1).ok()); // NOT1 @2
  auto p = logic::parse_pin(nl, "AND1.2");
  assert(p.ok());
  assert(logic::ignore_pin(nl, p.value).ok());
  p = logic::parse_pin(nl, "AND1.8");
  assert(p.ok());
  assert(logic::ignore_pin(nl, p.value).ok());
  p = logic::parse_pin(nl, "NOT1.Y");
  assert(p.ok());
  assert(logic::ignore_pin(nl, p.value).ok());
  assert(logic::wire(nl, 0, 1, 0).ok());
  assert(logic::wire(nl, 0, 2, 0).ok()); // fan-out of 2
  nl.gates[0].propagation_delay_ps = 1500;
  nl.gates[2].propagation_delay_ps = 250;
  std::string text;
  logic::yaml::save_text(nl, zero_inits(), text);
  assert(contains(text, "{id: AND1, kind: and, inputs: 8, ignore: [2, 8], ignore_output: false, delay_ps: 1500}"));
  assert(contains(text, "{id: NOT1, kind: not, inputs: 1, ignore: [], ignore_output: true, delay_ps: 250}"));
  assert(contains(text, "{from: AND1.Y, to: OR1.1}"));
  assert(contains(text, "{from: AND1.Y, to: NOT1.1}"));

  logic::yaml::Workspace ws;
  std::string err;
  assert(logic::yaml::load_text(text, ws, err));
  assert(netlists_equal(nl, ws.netlist));
  std::string again;
  logic::yaml::save_text(ws.netlist, ws.init, again);
  assert(again == text); // byte-identical
}

void test_yaml_superset_tolerance() {
  // User-edited YAML: comments, quoting, block-style maps and lists.
  // Must load to the same netlist and re-save to canonical bytes.
  const logic::Netlist want = make_pair();
  std::string canonical;
  logic::yaml::save_text(want, zero_inits(), canonical);
  const std::string edited =
      "# a user comment up top\n"
      "version: 1  # trailing comment\n"
      "gates:\n"
      "  - id: \"AND1\"\n" // quoted id
      "    kind: and\n"
      "    inputs: 2\n"
      "    ignore: []\n"
      "    ignore_output: false\n"
      "    delay_ps: 0\n"
      "  - {id: NOT1, kind: not, inputs: 1, ignore: [], ignore_output: false, delay_ps: 0}\n"
      "wires:  # block header comment\n"
      "  - from: AND1.Y\n" // block-style wire
      "    to: NOT1.1\n"
      "ties:\n"
      "clocks:\n"
      "meta:\n"
      "  tool: wengine-logic\n";
  logic::yaml::Workspace ws;
  std::string err;
  assert(logic::yaml::load_text(edited, ws, err));
  assert(netlists_equal(want, ws.netlist));
  std::string again;
  logic::yaml::save_text(ws.netlist, ws.init, again);
  assert(again == canonical);
}

void test_foreign_version_refusal() {
  const std::string text = "version: 3\ngates:\nwires:\nties:\nclocks:\nmeta:\n  tool: x\n";
  logic::yaml::Workspace ws;
  ws.netlist = make_pair(); // sentinel: must be untouched
  const logic::yaml::Workspace before = ws;
  std::string err;
  assert(!logic::yaml::load_text(text, ws, err));
  assert(contains(err, "unsupported workspace version 3 (want 1)"));
  assert(contains(err, "line 1:"));
  assert(workspaces_equal(ws, before)); // atomic: untouched on failure
}

void test_two_domain_clocks_round_trip() {
  // Driven plus implicit domains pin by source spelling: BUF1.Y drives
  // DFF1.CLK while DFF2.CLK dangles into `clk`. Reload validates the set
  // and re-saves byte-identically (006 T011; mismatch cases in T016).
  logic::Netlist nl;
  assert(logic::add_gate(nl, logic::GateKind::kBuf, 1).ok()); // BUF1 @0
  assert(logic::add_gate(nl, logic::GateKind::kDff, 2).ok()); // DFF1 @1
  assert(logic::add_gate(nl, logic::GateKind::kDff, 2).ok()); // DFF2 @2
  assert(logic::wire(nl, 0, 1, 1).ok());                      // BUF1.Y -> DFF1.CLK
  auto inits = zero_inits();
  inits[1] = kH;
  std::string text;
  logic::yaml::save_text(nl, inits, text);
  assert(contains(text, "clocks:\n  - {name: BUF1.Y}\n  - {name: clk}\n"));
  assert(contains(text, "init: 1}"));
  logic::yaml::Workspace ws;
  std::string err;
  assert(logic::yaml::load_text(text, ws, err));
  assert(netlists_equal(nl, ws.netlist));
  assert(ws.init[0] == kL); // combinational slot: no init
  assert(ws.init[1] == kH);
  assert(ws.init[2] == kL);
  std::string again;
  logic::yaml::save_text(ws.netlist, ws.init, again);
  assert(again == text); // byte-identical
}

void test_tie_round_trip() {
  // Ties persist byte-identically: NAND1.1 = NAND1.2 survives save/load.
  logic::Netlist nl;
  assert(logic::add_gate(nl, logic::GateKind::kNand, 2).ok());
  assert(logic::tie_text(nl, "NAND1.1", "NAND1.2").ok());
  assert(logic::block_input_count(nl) == 1);
  std::string text;
  logic::yaml::save_text(nl, zero_inits(), text);
  assert(contains(text, "version: 1\n"));
  assert(contains(text, "{a: NAND1.1, b: NAND1.2}"));
  logic::yaml::Workspace back;
  std::string err;
  assert(logic::yaml::load_text(text, back, err));
  assert(netlists_equal(nl, back.netlist));
  std::string again;
  logic::yaml::save_text(back.netlist, back.init, again);
  assert(again == text); // byte-identical
  // Tied NAND(a,a) == NOT(a) over the surviving 1-input interface.
  for (int b = 0; b < 2; ++b) {
    std::vector<logic::LogicValue> in{static_cast<logic::LogicValue>(b)};
    std::vector<logic::LogicValue> out{kL};
    assert(logic::solve(back.netlist, std::span<const logic::LogicValue>(in.data(), in.size()),
                        std::span<logic::LogicValue>(out.data(), out.size()))
                .ok());
    const logic::LogicValue want = (b != 0) ? kL : kH;
    assert(out[0] == want);
  }
}

void test_t_round_trip() {
  // T rows persist canonically (kind: t, inputs: 2) with `T1.Q` wire
  // endpoints and `ignore:` indices; reload is netlist-equal and
  // re-saves byte-identically (003-jk-t-flops T010).
  logic::Netlist nl;
  assert(logic::add_gate(nl, logic::GateKind::kT, 2).ok()); // T1 @0
  assert(logic::add_gate(nl, logic::GateKind::kT, 2).ok()); // T2 @1
  auto p = logic::parse_pin(nl, "T1.CLK");
  assert(p.ok());
  assert(logic::ignore_pin(nl, p.value).ok());
  assert(logic::wire(nl, 0, 1, 0).ok()); // T1.Q -> T2.T
  std::string text;
  logic::yaml::save_text(nl, zero_inits(), text);
  assert(contains(text, "{id: T1, kind: t, inputs: 2, ignore: [2]"));
  assert(contains(text, "{id: T2, kind: t, inputs: 2, ignore: []"));
  assert(contains(text, "{from: T1.Q, to: T2.T}"));

  logic::yaml::Workspace back;
  std::string err;
  assert(logic::yaml::load_text(text, back, err));
  assert(netlists_equal(nl, back.netlist));
  std::string again;
  logic::yaml::save_text(back.netlist, back.init, again);
  assert(again == text); // byte-identical
}

void test_jk_round_trip() {
  // JK rows persist canonically (kind: jk, inputs: 3) with `JK1.Q` wire
  // endpoints and `ignore:` indices; reload is netlist-equal and
  // re-saves byte-identically (003-jk-t-flops T014).
  logic::Netlist nl;
  assert(logic::add_gate(nl, logic::GateKind::kJk, 3).ok()); // JK1 @0
  assert(logic::add_gate(nl, logic::GateKind::kJk, 3).ok()); // JK2 @1
  auto p = logic::parse_pin(nl, "JK1.CLK");
  assert(p.ok());
  assert(logic::ignore_pin(nl, p.value).ok());
  p = logic::parse_pin(nl, "JK2.CLK");
  assert(p.ok());
  assert(logic::ignore_pin(nl, p.value).ok());
  assert(logic::wire(nl, 0, 1, 0).ok()); // JK1.Q -> JK2.J
  assert(logic::wire(nl, 0, 1, 1).ok()); // JK1.Q -> JK2.K
  std::string text;
  logic::yaml::save_text(nl, zero_inits(), text);
  assert(contains(text, "{id: JK1, kind: jk, inputs: 3, ignore: [3]"));
  assert(contains(text, "{id: JK2, kind: jk, inputs: 3, ignore: [3]"));
  assert(contains(text, "{from: JK1.Q, to: JK2.J}"));
  assert(contains(text, "{from: JK1.Q, to: JK2.K}"));

  logic::yaml::Workspace back;
  std::string err;
  assert(logic::yaml::load_text(text, back, err));
  assert(netlists_equal(nl, back.netlist));
  std::string again;
  logic::yaml::save_text(back.netlist, back.init, again);
  assert(again == text); // byte-identical
}

void test_dlatch_round_trip() {
  // DLATCH rows persist canonically (kind: dlatch, inputs: 2) with
  // `DLATCHn.D|EN` wire endpoints and `ignore:` indices; reload is
  // netlist-equal and re-saves byte-identically (004-sr-d-latches T009).
  logic::Netlist nl;
  assert(logic::add_gate(nl, logic::GateKind::kDlatch, 2).ok()); // DLATCH1 @0
  assert(logic::add_gate(nl, logic::GateKind::kBuf, 1).ok());    // BUF1 @1
  auto p = logic::parse_pin(nl, "DLATCH1.EN");
  assert(p.ok());
  assert(logic::ignore_pin(nl, p.value).ok());
  assert(logic::wire(nl, 1, 0, 0).ok()); // BUF1.Y -> DLATCH1.D
  std::string text;
  logic::yaml::save_text(nl, zero_inits(), text);
  assert(contains(text, "{id: DLATCH1, kind: dlatch, inputs: 2, ignore: [2]"));
  assert(contains(text, "{from: BUF1.Y, to: DLATCH1.D}"));

  logic::yaml::Workspace back;
  std::string err;
  assert(logic::yaml::load_text(text, back, err));
  assert(netlists_equal(nl, back.netlist));
  std::string again;
  logic::yaml::save_text(back.netlist, back.init, again);
  assert(again == text); // byte-identical
}

void test_sr_round_trip() {
  // SR rows persist canonically (kind: srlatch, inputs: 2) with
  // `SRLATCHn.S|R` wire endpoints and `ignore:` indices; reload is
  // netlist-equal and re-saves byte-identically (004-sr-d-latches T014).
  // A legacy file without SR rows loads unchanged (structural check in
  // test_canonical_bytes covers the shape; SR-free nets step identically
  // per the engine tests).
  logic::Netlist nl;
  assert(logic::add_gate(nl, logic::GateKind::kSrlatch, 2).ok()); // SRLATCH1 @0
  assert(logic::add_gate(nl, logic::GateKind::kSrlatch, 2).ok()); // SRLATCH2 @1
  auto p = logic::parse_pin(nl, "SRLATCH1.R");
  assert(p.ok());
  assert(logic::ignore_pin(nl, p.value).ok());
  assert(logic::wire(nl, 0, 1, 0).ok()); // SRLATCH1.Q -> SRLATCH2.S
  std::string text;
  logic::yaml::save_text(nl, zero_inits(), text);
  assert(contains(text, "{id: SRLATCH1, kind: srlatch, inputs: 2, ignore: [2]"));
  assert(contains(text, "{id: SRLATCH2, kind: srlatch, inputs: 2, ignore: []"));
  assert(contains(text, "{from: SRLATCH1.Q, to: SRLATCH2.S}"));

  logic::yaml::Workspace back;
  std::string err;
  assert(logic::yaml::load_text(text, back, err));
  assert(netlists_equal(nl, back.netlist));
  std::string again;
  logic::yaml::save_text(back.netlist, back.init, again);
  assert(again == text); // byte-identical
}

void test_sequential_feedback_round_trip() {
  // Flop Q+NQ fanout with combinational feedback (a.yml shape): the
  // feedback edge lands on a flop input (Q-boundary), so file order
  // never affects acceptance — the loader takes it and the trip is
  // netlist-equal and byte-identical.
  logic::Netlist nl;
  assert(logic::add_gate(nl, logic::GateKind::kT, 2).ok());  // T1 @0
  assert(logic::add_gate(nl, logic::GateKind::kOr, 2).ok()); // OR1 @1
  auto q = logic::parse_pin(nl, "T1.Q");
  assert(q.ok());
  auto nq = logic::parse_pin(nl, "T1.NQ");
  assert(nq.ok());
  auto i0 = logic::parse_pin(nl, "OR1.1");
  assert(i0.ok());
  auto i1 = logic::parse_pin(nl, "OR1.2");
  assert(i1.ok());
  assert(logic::wire_pins(nl, q.value, i0.value).ok());  // T1.Q -> OR1.1
  assert(logic::wire_pins(nl, nq.value, i1.value).ok()); // T1.NQ -> OR1.2
  auto t = logic::parse_pin(nl, "T1.T");
  assert(t.ok());
  auto y = logic::parse_pin(nl, "OR1.Y");
  assert(y.ok());
  assert(logic::wire_pins(nl, y.value, t.value).ok()); // OR1.Y -> T1.T
  std::string text;
  logic::yaml::save_text(nl, zero_inits(), text);
  assert(contains(text, "{from: T1.Q, to: OR1.1}"));
  assert(contains(text, "{from: T1.NQ, to: OR1.2}"));
  assert(contains(text, "{from: OR1.Y, to: T1.T}"));

  logic::yaml::Workspace back;
  std::string err;
  assert(logic::yaml::load_text(text, back, err));
  assert(netlists_equal(nl, back.netlist));
  // NQ polarity survives the trip (not covered by netlists_equal).
  assert(back.netlist.driver_inv[logic::tie_flat(1, 0)] == 0);
  assert(back.netlist.driver_inv[logic::tie_flat(1, 1)] == 1);
  assert(back.netlist.gates[0].nq_fanout == 1);
  std::string again;
  logic::yaml::save_text(back.netlist, back.init, again);
  assert(again == text); // byte-identical
}

void test_clocks_mismatch_refusals() {
  // `clocks:` must equal the derived set (006 T016): BUF1.Y drives
  // DFF1.CLK while DFF2.CLK dangles into `clk`. Any drift — unknown,
  // duplicate, or missing entries — is a loud, atomic refusal. A stored
  // name that resolves to a non-clock pin (e.g. a D-input driver) is
  // likewise unknown, since membership is never stored.
  const std::string head =
      "version: 1\n"
      "gates:\n"
      "  - {id: BUF1, kind: buf, inputs: 1, ignore: [], ignore_output: false, delay_ps: 0}\n"
      "  - {id: DFF1, kind: dff, inputs: 2, ignore: [], ignore_output: false, delay_ps: 0}\n"
      "  - {id: DFF2, kind: dff, inputs: 2, ignore: [], ignore_output: false, delay_ps: 0}\n"
      "wires:\n"
      "  - {from: BUF1.Y, to: DFF1.CLK}\n"
      "ties:\n";
  const std::string tail = "meta:\n  tool: wengine-logic\n";
  const auto load = [&](const std::string &clocks, std::string &err) {
    logic::yaml::Workspace ws;
    const bool ok = logic::yaml::load_text(head + clocks + tail, ws, err);
    assert(workspaces_equal(ws, logic::yaml::Workspace{})); // atomic
    return ok;
  };
  {
    // Unknown name (line 11: `clocks:` is line 9, entries on 10-11).
    std::string err;
    assert(!load("clocks:\n  - {name: BUF1.Y}\n  - {name: NOPE}\n", err));
    assert(contains(err, "line 11:"));
    assert(contains(err, "unknown clock domain 'NOPE'"));
  }
  {
    // Duplicate name (reported at the first occurrence, line 10).
    std::string err;
    assert(!load("clocks:\n  - {name: clk}\n  - {name: clk}\n", err));
    assert(contains(err, "line 10:"));
    assert(contains(err, "duplicate clock domain 'clk'"));
  }
  {
    // Missing entry for the derived implicit domain.
    std::string err;
    assert(!load("clocks:\n  - {name: BUF1.Y}\n", err));
    assert(contains(err, "derives clock domain 'clk' with no 'clocks:' entry"));
  }
  {
    // A live pin that drives no clock is not a domain either.
    std::string err;
    assert(!load("clocks:\n  - {name: BUF1.Y}\n  - {name: clk}\n  - {name: DFF1.Q}\n", err));
    assert(contains(err, "unknown clock domain 'DFF1.Q'"));
  }
  {
    // Combinational net: `clocks:` must be empty.
    const std::string comb =
        "version: 1\ngates:\n"
        "  - {id: AND1, kind: and, inputs: 2, ignore: [], ignore_output: false, delay_ps: 0}\n"
        "wires:\nties:\nclocks:\n  - {name: clk}\nmeta:\n  tool: wengine-logic\n";
    logic::yaml::Workspace ws;
    std::string err2;
    assert(!logic::yaml::load_text(comb, ws, err2));
    assert(contains(err2, "unknown clock domain 'clk'"));
    assert(workspaces_equal(ws, logic::yaml::Workspace{})); // atomic
  }
}

void test_malformed_line_numbers() {
  const std::string head =
      "version: 1\n"
      "gates:\n"
      "  - {id: AND1, kind: and, inputs: 2, ignore: [], ignore_output: false, delay_ps: 0}\n"
      "  - {id: NOT1, kind: not, inputs: 1, ignore: [], ignore_output: false, delay_ps: 0}\n"
      "wires:\n";
  const std::string mid = "ties:\nclocks:\n";
  const std::string tail = "meta:\n  tool: wengine-logic\n";
  {
    // Multi-driver: second wire targets the occupied input (file line 7).
    const std::string text =
        head + "  - {from: AND1.Y, to: NOT1.1}\n  - {from: AND1.Y, to: NOT1.1}\n" + mid + tail;
    logic::yaml::Workspace ws;
    std::string err;
    assert(!logic::yaml::load_text(text, ws, err));
    assert(contains(err, "line 7:"));
    assert(contains(err, "input already wired 'NOT1.1'"));
    assert(ws.netlist.live_count == 0); // atomic
  }
  {
    // Unknown gate id prefix (file line 6).
    const std::string text = head + "  - {from: AND9.Y, to: NOT1.1}\n" + mid + tail;
    logic::yaml::Workspace ws;
    std::string err;
    assert(!logic::yaml::load_text(text, ws, err));
    assert(contains(err, "line 6:"));
    assert(contains(err, "AND9.Y"));
  }
  {
    // Bad pin: input index out of range for NOT1 (file line 6).
    const std::string text = head + "  - {from: AND1.Y, to: NOT1.2}\n" + mid + tail;
    logic::yaml::Workspace ws;
    std::string err;
    assert(!logic::yaml::load_text(text, ws, err));
    assert(contains(err, "line 6:"));
  }
  {
    // Unknown gate kind (file line 3).
    const std::string text =
        "version: 1\ngates:\n"
        "  - {id: FOO1, kind: mega, inputs: 2, ignore: [], ignore_output: false, delay_ps: 0}\n"
        "wires:\n"
        "ties:\n"
        "clocks:\n" +
        tail;
    logic::yaml::Workspace ws;
    std::string err;
    assert(!logic::yaml::load_text(text, ws, err));
    assert(contains(err, "line 3:"));
    assert(contains(err, "bad gate kind"));
  }
  {
    // Unknown gate key (file line 3).
    const std::string text =
        "version: 1\ngates:\n"
        "  - {id: AND1, kind: and, inputs: 2, ignore: [], ignore_output: false, delay_ps: 0, bogus: 1}\n"
        "wires:\n"
        "ties:\n"
        "clocks:\n" +
        tail;
    logic::yaml::Workspace ws;
    std::string err;
    assert(!logic::yaml::load_text(text, ws, err));
    assert(contains(err, "line 3:"));
    assert(contains(err, "unknown gate key 'bogus'"));
  }
  {
    // Unknown top-level section (file line 6).
    const std::string text = head + "frobnicate:\n" + mid + tail;
    logic::yaml::Workspace ws;
    std::string err;
    assert(!logic::yaml::load_text(text, ws, err));
    assert(contains(err, "line 6:"));
    assert(contains(err, "unknown workspace key 'frobnicate'"));
  }
  {
    // Cycle-closing wire (BUF1.Y -> BUF2.1, BUF2.Y -> BUF1.1; line 7).
    const std::string text =
        "version: 1\n"
        "gates:\n"
        "  - {id: BUF1, kind: buf, inputs: 1, ignore: [], ignore_output: false, delay_ps: 0}\n"
        "  - {id: BUF2, kind: buf, inputs: 1, ignore: [], ignore_output: false, delay_ps: 0}\n"
        "wires:\n"
        "  - {from: BUF1.Y, to: BUF2.1}\n"
        "  - {from: BUF2.Y, to: BUF1.1}\n"
        "ties:\n"
        "clocks:\n" +
        tail;
    logic::yaml::Workspace ws;
    std::string err;
    assert(!logic::yaml::load_text(text, ws, err));
    assert(contains(err, "line 7:"));
    assert(contains(err, "combinational cycle"));
  }
}

void test_examples() {
  // Shipped examples stay loadable and keep their documented goldens.
  // Same fixture-directory recipe as test_lua (absolute source dir at
  // configure time, relative fallback for ad-hoc builds). A missing
  // file fails loudly with its path rather than silently skipping.
#ifdef WENGINE_LOGIC_EXAMPLE_DIR
  constexpr std::string_view kDir = WENGINE_LOGIC_EXAMPLE_DIR;
#else
  constexpr std::string_view kDir = "../examples/logic";
#endif
  const auto load = [&](const char *name) {
    const std::string path = std::string(kDir) + "/" + name + ".yaml";
    std::ifstream in(path);
    if (!in) {
      std::cerr << "test_logic_yaml: missing fixture: " << path << "\n";
    }
    assert(static_cast<bool>(in));
    std::ostringstream buf;
    buf << in.rdbuf();
    std::string text = buf.str();
    if (text.empty()) {
      std::cerr << "test_logic_yaml: empty fixture: " << path << "\n";
    }
    assert(!text.empty());
    return text;
  };
  const auto check_solve = [](logic::Netlist &nl, std::vector<logic::LogicValue> in,
                              std::vector<logic::LogicValue> want) {
    assert(logic::block_input_count(nl) == in.size());
    assert(logic::block_output_count(nl) == want.size());
    std::vector<logic::LogicValue> out(want.size(), kL);
    assert(logic::solve(nl, std::span<const logic::LogicValue>(in.data(), in.size()),
                        std::span<logic::LogicValue>(out.data(), out.size()))
               .ok());
    assert(out == want);
  };
  const auto check_stable = [](logic::Netlist &nl) {
    // Comment headers mean the file bytes differ from the first save;
    // stability (save -> load -> save identical) is what must hold.
    std::string a;
    logic::yaml::save_text(nl, zero_inits(), a);
    logic::yaml::Workspace ws;
    std::string err;
    assert(logic::yaml::load_text(a, ws, err));
    std::string b;
    logic::yaml::save_text(ws.netlist, ws.init, b);
    assert(a == b);
  };
  // nand_pair.yaml: NOT(AND(a, b)).
  {
    logic::yaml::Workspace ws;
    logic::Netlist &nl = ws.netlist;
    std::string err;
    assert(logic::yaml::load_text(load("nand_pair"), ws, err));
    assert(nl.live_count == 2);
    check_solve(nl, {kH, kH}, {kL});
    check_solve(nl, {kL, kL}, {kH});
    check_stable(nl);
  }
  // fanout.yaml: AND1.Y drives NOT1.1 and BUF1.1 (slot-ordered outputs).
  {
    logic::yaml::Workspace ws;
    logic::Netlist &nl = ws.netlist;
    std::string err;
    assert(logic::yaml::load_text(load("fanout"), ws, err));
    assert(nl.live_count == 3);
    check_solve(nl, {kH, kH}, {kL, kH});
    check_solve(nl, {kL, kL}, {kH, kL});
    check_stable(nl);
  }
  // parity3.yaml: XOR cascade, odd parity over three block inputs.
  {
    logic::yaml::Workspace ws;
    logic::Netlist &nl = ws.netlist;
    std::string err;
    assert(logic::yaml::load_text(load("parity3"), ws, err));
    assert(nl.live_count == 2);
    check_solve(nl, {kH, kH, kH}, {kH});
    check_solve(nl, {kH, kH, kL}, {kL});
    check_solve(nl, {kL, kL, kL}, {kL});
    check_stable(nl);
  }
  // tieoff.yaml: AND1.2 ignored (tied 0) plus stored delays.
  {
    logic::yaml::Workspace ws;
    logic::Netlist &nl = ws.netlist;
    std::string err;
    assert(logic::yaml::load_text(load("tieoff"), ws, err));
    assert(nl.live_count == 2);
    assert(nl.gates[0].ignored_mask == (1u << 1));
    assert(nl.gates[0].propagation_delay_ps == 1500);
    assert(nl.gates[1].propagation_delay_ps == 250);
    check_solve(nl, {kH}, {kH}); // AND(1, tied-0) = 0, NOT(0) = 1
    check_stable(nl);
  }
  // two_clock_counter.yaml: mixed inits + two named domains persist.
  {
    logic::yaml::Workspace ws;
    std::string err;
    assert(logic::yaml::load_text(load("two_clock_counter"), ws, err));
    logic::Netlist &nl = ws.netlist;
    assert(nl.live_count == 5);
    assert(ws.init[1] == kL); // DFF1 @1 powers on 0
    assert(ws.init[2] == kH); // DFF2 @2 powers on 1
    std::string text;
    logic::yaml::save_text(nl, ws.init, text);
    assert(contains(text, "init: 0}"));
    assert(contains(text, "init: 1}"));
    assert(contains(text, "clocks:\n  - {name: BUF1.Y}\n  - {name: clk}\n"));
    check_stable(nl);
  }
  // gated_async_counter.yaml: full-control-set flop, stored init applies.
  {
    logic::yaml::Workspace ws;
    std::string err;
    assert(logic::yaml::load_text(load("gated_async_counter"), ws, err));
    logic::Netlist &nl = ws.netlist;
    assert(nl.live_count == 2);
    assert(nl.gates[0].input_count == 5);
    assert(ws.init[0] == kL);
    std::string text;
    logic::yaml::save_text(nl, ws.init, text);
    assert(contains(text, "inputs: 5"));
    assert(contains(text, "clocks:\n  - {name: clk}\n"));
    check_stable(nl);
  }
  // jk_t_latch_chain.yaml: all sequential kinds persist with zero inits.
  {
    logic::yaml::Workspace ws;
    std::string err;
    assert(logic::yaml::load_text(load("jk_t_latch_chain"), ws, err));
    logic::Netlist &nl = ws.netlist;
    assert(nl.live_count == 3);
    assert(nl.gates[0].kind == logic::GateKind::kJk);
    assert(nl.gates[1].kind == logic::GateKind::kT);
    assert(nl.gates[2].kind == logic::GateKind::kDlatch);
    for (std::size_t s = 0; s < 3; ++s) {
      assert(ws.init[s] == kL);
    }
    check_stable(nl);
  }
  // tied_demorgan.yaml: De Morgan mirror over tied-input inverters (two
  // 2-member tie groups + one transitive 3-member group, 6 gates,
  // 6 block inputs, 4 outputs). Exhaustive over all 64 rows against the
  // hand-derived goldens: NAND1.Y = ~(a&b), OR1.Y = ~c|~d (== NAND(c,d)
  // on every row), XOR1.Y = 0, AND1.Y = E&e.
  {
    logic::yaml::Workspace ws;
    logic::Netlist &nl = ws.netlist;
    std::string err;
    assert(logic::yaml::load_text(load("tied_demorgan"), ws, err));
    assert(nl.live_count == 6);
    assert(logic::block_input_count(nl) == 6);
    assert(logic::block_output_count(nl) == 4);
    assert(logic::tie_group_count(nl) == 3);
    for (std::uint32_t row = 0; row < 64; ++row) {
      std::vector<logic::LogicValue> in(6, kL);
      for (std::size_t j = 0; j < 6; ++j) {
        in[j] = (((row >> j) & 1u) != 0u) ? kH : kL;
      }
      const bool a = (in[0] == kH);
      const bool b = (in[1] == kH);
      const bool c = (in[2] == kH);
      const bool d = (in[3] == kH);
      const bool e_class = (in[4] == kH);
      const bool e_plain = (in[5] == kH);
      const logic::LogicValue nand_ab = !(a && b) ? kH : kL;
      const logic::LogicValue or_mirror = (!c || !d) ? kH : kL;
      const logic::LogicValue xor_zero = kL; // XOR(E, E) == 0
      const logic::LogicValue and_out = (e_class && e_plain) ? kH : kL;
      check_solve(nl, in, {nand_ab, or_mirror, xor_zero, and_out});
    }
    check_stable(nl);
  }
}

} // namespace

int main() {
  test_canonical_bytes_v1();
  test_init_round_trip();
  test_missing_init_defaults_low();
  test_legacy_refusals();
  test_bad_init_refusals();
  test_empty_round_trip();
  test_round_trip_ignore_fanin_delay();
  test_yaml_superset_tolerance();
  test_foreign_version_refusal();
  test_two_domain_clocks_round_trip();
  test_clocks_mismatch_refusals();
  test_tie_round_trip();
  test_t_round_trip();
  test_jk_round_trip();
  test_dlatch_round_trip();
  test_sr_round_trip();
  test_sequential_feedback_round_trip();
  test_malformed_line_numbers();
  test_examples();
  std::cout << "test_logic_yaml: OK\n";
  return 0;
}
