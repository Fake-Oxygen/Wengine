// LOGIC-6: versioned YAML workspace store (yaml-cpp backed).
//
// Links `wengine_logic_yaml` (the single TU allowed to know yaml-cpp
// exists) — never `wengine_core` alone, so the engine-header seam stays
// provable: `test_logic` still links core only (see CMakeLists), while
// this target pins the store contract: canonical bytes, byte-identical
// round-trip (ignore marks + fan-in + delay), foreign-version refusal,
// and loud `line N:` diagnostics for malformed workspaces.

#include "wengine/logic/eval.hpp"
#include "wengine/logic/netlist.hpp"
#include "wengine/logic/types.hpp"
#include "wengine/logic/yaml_store.hpp"

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

void test_canonical_bytes() {
  const logic::Netlist nl = make_pair();
  std::string text;
  logic::yaml::save_text(nl, text);
  const std::string want =
      "version: 2\n"
      "gates:\n"
      "  - {id: AND1, kind: and, inputs: 2, ignore: [], ignore_output: false, delay_ps: 0}\n"
      "  - {id: NOT1, kind: not, inputs: 1, ignore: [], ignore_output: false, delay_ps: 0}\n"
      "wires:\n"
      "  - {from: AND1.Y, to: NOT1.1}\n"
      "ties:\n"
      "meta:\n"
      "  tool: wengine-logic\n";
  assert(text == want);
}

void test_empty_round_trip() {
  logic::Netlist nl;
  std::string text;
  logic::yaml::save_text(nl, text);
  assert(text == "version: 2\ngates:\nwires:\nties:\nmeta:\n  tool: wengine-logic\n");
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
      "version: 2  # trailing comment\n"
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
  const std::string text = "version: 3\ngates:\nwires:\nties:\nmeta:\n  tool: x\n";
  logic::Netlist nl = make_pair(); // sentinel: must be untouched
  const logic::Netlist before = nl;
  std::string err;
  assert(!logic::yaml::load_text(text, nl, err));
  assert(contains(err, "unsupported workspace version 3 (want 2)"));
  assert(contains(err, "line 1:"));
  assert(netlists_equal(nl, before)); // atomic: untouched on failure
}

void test_v1_fallback_loads_untied() {
  // v1 files (no `ties:` section) load as untied workspaces and re-save
  // to canonical v2 bytes.
  const std::string v1 =
      "version: 1\n"
      "gates:\n"
      "  - {id: AND1, kind: and, inputs: 2, ignore: [], ignore_output: false, delay_ps: 0}\n"
      "  - {id: NOT1, kind: not, inputs: 1, ignore: [], ignore_output: false, delay_ps: 0}\n"
      "wires:\n"
      "  - {from: AND1.Y, to: NOT1.1}\n"
      "meta:\n"
      "  tool: wengine-logic\n";
  logic::Netlist nl;
  std::string err;
  assert(logic::yaml::load_text(v1, nl, err));
  assert(nl.live_count == 2);
  assert(logic::block_input_count(nl) == 2);
  assert(logic::tie_group_count(nl) == 0);
  const logic::Netlist want = make_pair();
  assert(netlists_equal(nl, want));
  std::string canonical;
  logic::yaml::save_text(want, canonical);
  std::string again;
  logic::yaml::save_text(nl, again);
  assert(again == canonical);
  assert(contains(again, "version: 2\n"));
}

void test_tie_round_trip() {
  // Ties persist byte-identically: NAND1.1 = NAND1.2 survives save/load.
  logic::Netlist nl;
  assert(logic::add_gate(nl, logic::GateKind::kNand, 2).ok());
  assert(logic::tie_text(nl, "NAND1.1", "NAND1.2").ok());
  assert(logic::block_input_count(nl) == 1);
  std::string text;
  logic::yaml::save_text(nl, text);
  assert(contains(text, "version: 2\n"));
  assert(contains(text, "{a: NAND1.1, b: NAND1.2}"));
  logic::Netlist back;
  std::string err;
  assert(logic::yaml::load_text(text, back, err));
  assert(netlists_equal(nl, back));
  std::string again;
  logic::yaml::save_text(back, again);
  assert(again == text); // byte-identical
  // Tied NAND(a,a) == NOT(a) over the surviving 1-input interface.
  for (int b = 0; b < 2; ++b) {
    std::vector<logic::LogicValue> in{static_cast<logic::LogicValue>(b)};
    std::vector<logic::LogicValue> out{kL};
    assert(logic::solve(back, std::span<const logic::LogicValue>(in.data(), in.size()),
                        std::span<logic::LogicValue>(out.data(), out.size()))
                .ok());
    const logic::LogicValue want = (b != 0) ? kL : kH;
    assert(out[0] == want);
  }
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
    logic::yaml::save_text(nl, a);
    logic::Netlist back;
    std::string err;
    assert(logic::yaml::load_text(a, back, err));
    std::string b;
    logic::yaml::save_text(back, b);
    assert(a == b);
  };
  // nand_pair.yaml: NOT(AND(a, b)).
  {
    logic::Netlist nl;
    std::string err;
    assert(logic::yaml::load_text(load("nand_pair"), nl, err));
    assert(nl.live_count == 2);
    check_solve(nl, {kH, kH}, {kL});
    check_solve(nl, {kL, kL}, {kH});
    check_stable(nl);
  }
  // fanout.yaml: AND1.Y drives NOT1.1 and BUF1.1 (slot-ordered outputs).
  {
    logic::Netlist nl;
    std::string err;
    assert(logic::yaml::load_text(load("fanout"), nl, err));
    assert(nl.live_count == 3);
    check_solve(nl, {kH, kH}, {kL, kH});
    check_solve(nl, {kL, kL}, {kH, kL});
    check_stable(nl);
  }
  // parity3.yaml: XOR cascade, odd parity over three block inputs.
  {
    logic::Netlist nl;
    std::string err;
    assert(logic::yaml::load_text(load("parity3"), nl, err));
    assert(nl.live_count == 2);
    check_solve(nl, {kH, kH, kH}, {kH});
    check_solve(nl, {kH, kH, kL}, {kL});
    check_solve(nl, {kL, kL, kL}, {kL});
    check_stable(nl);
  }
  // tieoff.yaml: AND1.2 ignored (tied 0) plus stored delays.
  {
    logic::Netlist nl;
    std::string err;
    assert(logic::yaml::load_text(load("tieoff"), nl, err));
    assert(nl.live_count == 2);
    assert(nl.gates[0].ignored_mask == (1u << 1));
    assert(nl.gates[0].propagation_delay_ps == 1500);
    assert(nl.gates[1].propagation_delay_ps == 250);
    check_solve(nl, {kH}, {kH}); // AND(1, tied-0) = 0, NOT(0) = 1
    check_stable(nl);
  }
  // tied_demorgan.yaml: De Morgan mirror over tied-input inverters (two
  // 2-member tie groups + one transitive 3-member group, 6 gates,
  // 6 block inputs, 4 outputs). Exhaustive over all 64 rows against the
  // hand-derived goldens: NAND1.Y = ~(a&b), OR1.Y = ~c|~d (== NAND(c,d)
  // on every row), XOR1.Y = 0, AND1.Y = E&e.
  {
    logic::Netlist nl;
    std::string err;
    assert(logic::yaml::load_text(load("tied_demorgan"), nl, err));
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
  test_canonical_bytes();
  test_empty_round_trip();
  test_round_trip_ignore_fanin_delay();
  test_yaml_superset_tolerance();
  test_foreign_version_refusal();
  test_v1_fallback_loads_untied();
  test_tie_round_trip();
  test_malformed_line_numbers();
  test_examples();
  std::cout << "test_logic_yaml: OK\n";
  return 0;
}
