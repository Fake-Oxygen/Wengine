// LOGIC-6: versioned YAML workspace store (yaml-cpp backed).
//
// Links `wengine_logic_yaml` (the single TU allowed to know yaml-cpp
// exists) — never `wengine_core` alone, so the engine-header seam stays
// provable: `test_logic` still links core only (see CMakeLists), while
// this target pins the store contract: canonical bytes, byte-identical
// round-trip (ignore marks + fan-in + delay), foreign-version refusal,
// and loud `line N:` diagnostics for malformed workspaces.

#include "wengine/logic/netlist.hpp"
#include "wengine/logic/types.hpp"
#include "wengine/logic/yaml_store.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>

using namespace wengine;

namespace {

bool contains(const std::string &hay, const std::string &needle) {
  return hay.find(needle) != std::string::npos;
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

void test_canonical_bytes() {
  const logic::Netlist nl = make_pair();
  std::string text;
  logic::yaml::save_text(nl, text);
  const std::string want =
      "version: 1\n"
      "gates:\n"
      "  - {id: AND1, kind: and, inputs: 2, ignore: [], ignore_output: false, delay_ps: 0}\n"
      "  - {id: NOT1, kind: not, inputs: 1, ignore: [], ignore_output: false, delay_ps: 0}\n"
      "wires:\n"
      "  - {from: AND1.Y, to: NOT1.1}\n"
      "meta:\n"
      "  tool: wengine-logic\n";
  assert(text == want);
}

void test_empty_round_trip() {
  logic::Netlist nl;
  std::string text;
  logic::yaml::save_text(nl, text);
  assert(text == "version: 1\ngates:\nwires:\nmeta:\n  tool: wengine-logic\n");
  logic::Netlist back;
  std::string err;
  assert(logic::yaml::load_text(text, back, err));
  assert(back.live_count == 0);
  std::string again;
  logic::yaml::save_text(back, again);
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
  logic::yaml::save_text(nl, text);
  assert(contains(text, "{id: AND1, kind: and, inputs: 8, ignore: [2, 8], ignore_output: false, delay_ps: 1500}"));
  assert(contains(text, "{id: NOT1, kind: not, inputs: 1, ignore: [], ignore_output: true, delay_ps: 250}"));
  assert(contains(text, "{from: AND1.Y, to: OR1.1}"));
  assert(contains(text, "{from: AND1.Y, to: NOT1.1}"));

  logic::Netlist back;
  std::string err;
  assert(logic::yaml::load_text(text, back, err));
  assert(netlists_equal(nl, back));
  std::string again;
  logic::yaml::save_text(back, again);
  assert(again == text); // byte-identical
}

void test_yaml_superset_tolerance() {
  // User-edited YAML: comments, quoting, block-style maps and lists.
  // Must load to the same netlist and re-save to canonical bytes.
  const logic::Netlist want = make_pair();
  std::string canonical;
  logic::yaml::save_text(want, canonical);
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
      "meta:\n"
      "  tool: wengine-logic\n";
  logic::Netlist back;
  std::string err;
  assert(logic::yaml::load_text(edited, back, err));
  assert(netlists_equal(want, back));
  std::string again;
  logic::yaml::save_text(back, again);
  assert(again == canonical);
}

void test_foreign_version_refusal() {
  const std::string text = "version: 2\ngates:\nwires:\nmeta:\n  tool: x\n";
  logic::Netlist nl = make_pair(); // sentinel: must be untouched
  const logic::Netlist before = nl;
  std::string err;
  assert(!logic::yaml::load_text(text, nl, err));
  assert(contains(err, "unsupported workspace version 2 (want 1)"));
  assert(contains(err, "line 1:"));
  assert(netlists_equal(nl, before)); // atomic: untouched on failure
}

void test_malformed_line_numbers() {
  const std::string head =
      "version: 1\n"
      "gates:\n"
      "  - {id: AND1, kind: and, inputs: 2, ignore: [], ignore_output: false, delay_ps: 0}\n"
      "  - {id: NOT1, kind: not, inputs: 1, ignore: [], ignore_output: false, delay_ps: 0}\n"
      "wires:\n";
  const std::string tail = "meta:\n  tool: wengine-logic\n";
  {
    // Multi-driver: second wire targets the occupied input (file line 7).
    const std::string text =
        head + "  - {from: AND1.Y, to: NOT1.1}\n  - {from: AND1.Y, to: NOT1.1}\n" + tail;
    logic::Netlist nl;
    std::string err;
    assert(!logic::yaml::load_text(text, nl, err));
    assert(contains(err, "line 7:"));
    assert(contains(err, "input already wired 'NOT1.1'"));
    assert(nl.live_count == 0); // atomic
  }
  {
    // Unknown gate id prefix (file line 6).
    const std::string text = head + "  - {from: AND9.Y, to: NOT1.1}\n" + tail;
    logic::Netlist nl;
    std::string err;
    assert(!logic::yaml::load_text(text, nl, err));
    assert(contains(err, "line 6:"));
    assert(contains(err, "AND9.Y"));
  }
  {
    // Bad pin: input index out of range for NOT1 (file line 6).
    const std::string text = head + "  - {from: AND1.Y, to: NOT1.2}\n" + tail;
    logic::Netlist nl;
    std::string err;
    assert(!logic::yaml::load_text(text, nl, err));
    assert(contains(err, "line 6:"));
  }
  {
    // Unknown gate kind (file line 3).
    const std::string text =
        "version: 1\ngates:\n"
        "  - {id: FOO1, kind: mega, inputs: 2, ignore: [], ignore_output: false, delay_ps: 0}\n"
        "wires:\n" +
        tail;
    logic::Netlist nl;
    std::string err;
    assert(!logic::yaml::load_text(text, nl, err));
    assert(contains(err, "line 3:"));
    assert(contains(err, "bad gate kind"));
  }
  {
    // Unknown gate key (file line 3).
    const std::string text =
        "version: 1\ngates:\n"
        "  - {id: AND1, kind: and, inputs: 2, ignore: [], ignore_output: false, delay_ps: 0, bogus: 1}\n"
        "wires:\n" +
        tail;
    logic::Netlist nl;
    std::string err;
    assert(!logic::yaml::load_text(text, nl, err));
    assert(contains(err, "line 3:"));
    assert(contains(err, "unknown gate key 'bogus'"));
  }
  {
    // Unknown top-level section (file line 6).
    const std::string text = head + "frobnicate:\n" + tail;
    logic::Netlist nl;
    std::string err;
    assert(!logic::yaml::load_text(text, nl, err));
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
        "  - {from: BUF2.Y, to: BUF1.1}\n" +
        tail;
    logic::Netlist nl;
    std::string err;
    assert(!logic::yaml::load_text(text, nl, err));
    assert(contains(err, "line 7:"));
    assert(contains(err, "combinational cycle"));
  }
}

} // namespace

int main() {
  test_canonical_bytes();
  test_empty_round_trip();
  test_round_trip_ignore_fanin_delay();
  test_yaml_superset_tolerance();
  test_foreign_version_refusal();
  test_malformed_line_numbers();
  std::cout << "test_logic_yaml: OK\n";
  return 0;
}
