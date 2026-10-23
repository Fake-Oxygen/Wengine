// Versioned logic-workspace store (LOGIC-6).
//
// THE single TU allowed to know yaml-cpp exists (same seam as
// src/signal/lua_solver.cpp). No other engine, CLI, or test TU includes a
// yaml header; the public surface is the yaml-free
// include/wengine/logic/yaml_store.hpp.
//
// Save emits one canonical byte layout (manual string build, identical to
// the LOGIC-5 hand writer it replaces — the round-trip test pins the
// bytes). Load parses real YAML via yaml-cpp (comments, quoting, block vs
// flow style, multiline all accepted) then validates the strict v1 schema:
// exact version gate, no unknown keys anywhere, single-driver wires, known
// gates/pins, no cycles. Every diagnostic carries a 1-based `line N:`
// prefix from the offending node's mark.

#include "wengine/logic/yaml_store.hpp"

#include "wengine/logic/eval.hpp"
#include "wengine/logic/netlist.hpp"
#include "wengine/logic/types.hpp"

#include <yaml-cpp/yaml.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wengine::logic::yaml {

namespace {

[[nodiscard]] std::string lower_kind(GateKind k) {
  std::string s(gate_kind_name(k));
  for (char &c : s) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - ('A' - 'a'));
    }
  }
  return s;
}

[[nodiscard]] std::string gate_name(const Netlist &nl, std::uint16_t slot) {
  std::string s(gate_kind_name(nl.gates[slot].kind));
  s += std::to_string(nl.gates[slot].counter);
  return s;
}

[[nodiscard]] std::string pin_text(const Netlist &nl, std::uint16_t slot, bool is_output, std::uint8_t input) {
  std::string s = gate_name(nl, slot);
  s += '.';
  if (is_output) {
    s += 'Y';
  } else {
    s += std::to_string(static_cast<unsigned>(input) + 1u);
  }
  return s;
}

// Strict unsigned decimal: no signs/spaces, digits only, saturates on
// overflow. Mirrors the former hand parser (quoted "2" and plain 2 both
// surface as Scalar "2", so one path covers both spellings).
[[nodiscard]] bool parse_u64(std::string_view text, std::uint64_t &out) {
  if (text.empty()) {
    return false;
  }
  std::uint64_t v = 0;
  for (char c : text) {
    if (c < '0' || c > '9') {
      return false;
    }
    const auto d = static_cast<std::uint64_t>(c - '0');
    if (v > (0xFFFFFFFFFFFFFFFFULL - d) / 10ULL) {
      return false;
    }
    v = v * 10ULL + d;
  }
  out = v;
  return true;
}

[[nodiscard]] std::size_t line_of(const YAML::Node &n) {
  const int line = n.Mark().line;
  if (line < 0) {
    return 1;
  }
  return static_cast<std::size_t>(line) + 1u;
}

[[nodiscard]] std::string at_line(const YAML::Node &n, const std::string &msg) {
  return "line " + std::to_string(line_of(n)) + ": " + msg;
}

struct GateRow {
  std::string id;
  GateKind kind = GateKind::kAnd;
  std::uint8_t inputs = 0;
  std::uint8_t ignored_mask = 0;
  bool ignore_output = false;
  std::uint64_t delay_ps = 0;
  std::uint32_t counter = 0;
};

struct WireRow {
  std::string from;
  std::string to;
  std::size_t line = 1;
};

struct TieRow {
  std::string a;
  std::string b;
  std::size_t line = 1;
};

[[nodiscard]] bool check_gate_map(const YAML::Node &g, GateRow &row, std::string &err) {
  constexpr const char *kWant[6] = {"id", "kind", "inputs", "ignore", "ignore_output", "delay_ps"};
  for (auto it = g.begin(); it != g.end(); ++it) {
    const std::string key = it->first.IsScalar() ? it->first.Scalar() : std::string{};
    bool known = false;
    for (const char *w : kWant) {
      if (key == w) {
        known = true;
        break;
      }
    }
    if (!known) {
      err = at_line(it->first, "unknown gate key '" + key + "' (want id|kind|inputs|ignore|ignore_output|delay_ps)");
      return false;
    }
  }
  for (const char *w : kWant) {
    const YAML::Node v = g[w];
    if (!v || !v.IsDefined()) {
      err = at_line(g, std::string("gate entry needs 6 fields (id, kind, inputs, ignore, ignore_output, delay_ps), missing '") +
                           w + "'");
      return false;
    }
  }
  const YAML::Node id = g["id"];
  const YAML::Node kind = g["kind"];
  const YAML::Node inputs = g["inputs"];
  const YAML::Node ignore = g["ignore"];
  const YAML::Node ignore_output = g["ignore_output"];
  const YAML::Node delay = g["delay_ps"];
  if (!id.IsScalar() || id.Scalar().empty()) {
    err = at_line(id.IsDefined() ? id : g, "empty gate id");
    return false;
  }
  row.id = id.Scalar();
  if (!kind.IsScalar()) {
    err = at_line(kind, "bad gate kind (want AND|OR|NAND|NOR|XOR|XNOR|NOT|BUF|DFF|DLATCH)");
    return false;
  }
  const auto parsed_kind = parse_gate_kind(kind.Scalar());
  if (!parsed_kind.ok()) {
    err = at_line(kind, "bad gate kind (want AND|OR|NAND|NOR|XOR|XNOR|NOT|BUF|DFF|DLATCH)");
    return false;
  }
  row.kind = parsed_kind.value;
  if (kind_ordinal(row.kind) >= 8) {
    // SEQ1 vocabulary only: sequential gates parse, but v2 workspaces hold
    // combinational gates only (schema v3 + SEQ2 state pool arrive later).
    err = at_line(kind, "sequential gates need schema v3 (DFF/DLATCH not yet supported)");
    return false;
  }
  if (!inputs.IsScalar()) {
    err = at_line(inputs, "bad input count (want 1..8)");
    return false;
  }
  std::uint64_t n = 0;
  if (!parse_u64(inputs.Scalar(), n) || n == 0 || n > kMaxInputs) {
    err = at_line(inputs, "bad input count (want 1..8)");
    return false;
  }
  row.inputs = static_cast<std::uint8_t>(n);
  if (!valid_fanin(row.kind, row.inputs)) {
    err = at_line(inputs, "bad fan-in for kind");
    return false;
  }
  // Id must read `<KIND><counter>` with the same kind (case-insensitive).
  const std::string kname(gate_kind_name(row.kind));
  if (row.id.size() <= kname.size()) {
    err = at_line(id, "gate id must read KIND+counter (e.g. AND1)");
    return false;
  }
  for (std::size_t i = 0; i < kname.size(); ++i) {
    char a = row.id[i];
    if (a >= 'a' && a <= 'z') {
      a = static_cast<char>(a - ('a' - 'A'));
    }
    if (a != kname[i]) {
      err = at_line(id, "gate id/kind mismatch");
      return false;
    }
  }
  std::uint64_t counter = 0;
  if (!parse_u64(std::string_view(row.id.data() + kname.size(), row.id.size() - kname.size()), counter) ||
      counter == 0 || counter > 0xFFFFFFFFULL) {
    err = at_line(id, "gate id needs a counter >= 1 (e.g. AND1)");
    return false;
  }
  row.counter = static_cast<std::uint32_t>(counter);
  // ignore: sequence of 1-based inputs, ascending, unique, in range.
  // Accepts flow `[2, 3]` and block lists alike (the multiline win over
  // the old canonical-subset parser).
  if (!ignore.IsSequence()) {
    err = at_line(ignore, "ignore must read [i, ...]");
    return false;
  }
  row.ignored_mask = 0;
  unsigned prev = 0;
  for (std::size_t i = 0; i < ignore.size(); ++i) {
    const YAML::Node item = ignore[i];
    if (!item.IsScalar()) {
      err = at_line(item.IsDefined() ? item : ignore, "ignore index out of range");
      return false;
    }
    std::uint64_t idx = 0;
    if (!parse_u64(item.Scalar(), idx) || idx == 0 || idx > row.inputs) {
      err = at_line(item, "ignore index out of range");
      return false;
    }
    if (idx <= prev) {
      err = at_line(item, "ignore indices must ascend");
      return false;
    }
    prev = static_cast<unsigned>(idx);
    row.ignored_mask = static_cast<std::uint8_t>(row.ignored_mask | (1u << (idx - 1u)));
  }
  if (!ignore_output.IsScalar()) {
    err = at_line(ignore_output, "ignore_output must be true|false");
    return false;
  }
  if (ignore_output.Scalar() == "true") {
    row.ignore_output = true;
  } else if (ignore_output.Scalar() == "false") {
    row.ignore_output = false;
  } else {
    err = at_line(ignore_output, "ignore_output must be true|false");
    return false;
  }
  if (!delay.IsScalar() || !parse_u64(delay.Scalar(), row.delay_ps)) {
    err = at_line(delay.IsDefined() ? delay : g, "bad delay_ps");
    return false;
  }
  return true;
}

[[nodiscard]] bool check_wire_map(const YAML::Node &w, WireRow &row, std::string &err) {
  for (auto it = w.begin(); it != w.end(); ++it) {
    const std::string key = it->first.IsScalar() ? it->first.Scalar() : std::string{};
    if (key != "from" && key != "to") {
      err = at_line(it->first, "wire fields must read 'from' then 'to'");
      return false;
    }
  }
  const YAML::Node from = w["from"];
  const YAML::Node to = w["to"];
  if (!from || !from.IsDefined() || !to || !to.IsDefined()) {
    err = at_line(w, "wire entry needs 2 fields (from, to)");
    return false;
  }
  if (!from.IsScalar() || from.Scalar().empty() || !to.IsScalar() || to.Scalar().empty()) {
    err = at_line(w, "empty wire pin");
    return false;
  }
  row.from = from.Scalar();
  row.to = to.Scalar();
  row.line = line_of(w);
  return true;
}

[[nodiscard]] bool check_tie_map(const YAML::Node &t, TieRow &row, std::string &err) {
  for (auto it = t.begin(); it != t.end(); ++it) {
    const std::string key = it->first.IsScalar() ? it->first.Scalar() : std::string{};
    if (key != "a" && key != "b") {
      err = at_line(it->first, "tie fields must read 'a' then 'b'");
      return false;
    }
  }
  const YAML::Node a = t["a"];
  const YAML::Node b = t["b"];
  if (!a || !a.IsDefined() || !b || !b.IsDefined()) {
    err = at_line(t, "tie entry needs 2 fields (a, b)");
    return false;
  }
  if (!a.IsScalar() || a.Scalar().empty() || !b.IsScalar() || b.Scalar().empty()) {
    err = at_line(t, "empty tie pin");
    return false;
  }
  row.a = a.Scalar();
  row.b = b.Scalar();
  row.line = line_of(t);
  return true;
}

[[nodiscard]] bool load_impl(const std::string &text, Netlist &out, std::string &err) {
  if (text.find_first_not_of(" \t\r\n#") == std::string::npos) {
    // Cheap empty/whitespace fast path; comment-only files fall through
    // to the parser and hit the same diagnostic below.
    bool only_ws = true;
    for (char c : text) {
      if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
        only_ws = false;
        break;
      }
    }
    if (only_ws) {
      err = "empty workspace (want 'version: 2' first)";
      return false;
    }
  }
  YAML::Node root;
  try {
    root = YAML::Load(text);
  } catch (const YAML::Exception &e) {
    const int line = e.mark.line;
    if (line < 0) {
      err = std::string("bad YAML: ") + e.msg;
    } else {
      err = "line " + std::to_string(static_cast<std::size_t>(line) + 1u) + ": " + e.msg;
    }
    return false;
  }
  if (!root || !root.IsDefined() || root.IsNull()) {
    err = "empty workspace (want 'version: 2' first)";
    return false;
  }
  if (!root.IsMap()) {
    err = at_line(root, "workspace root must read 'version: 2' first");
    return false;
  }
  for (auto it = root.begin(); it != root.end(); ++it) {
    const std::string key = it->first.IsScalar() ? it->first.Scalar() : std::string{};
    if (key != "version" && key != "gates" && key != "wires" && key != "ties" && key != "meta") {
      err = at_line(it->first, "unknown workspace key '" + key + "' (want version|gates|wires|ties|meta)");
      return false;
    }
  }
  const YAML::Node version = root["version"];
  const YAML::Node gates = root["gates"];
  const YAML::Node wires = root["wires"];
  const YAML::Node ties = root["ties"];
  const YAML::Node meta = root["meta"];
  if (!version || !version.IsDefined()) {
    err = "first line must read 'version: 2'";
    return false;
  }
  if (!gates || !gates.IsDefined()) {
    err = "want 'gates:' section";
    return false;
  }
  if (!wires || !wires.IsDefined()) {
    err = "want 'wires:' section";
    return false;
  }
  if (!meta || !meta.IsDefined()) {
    err = "want 'meta:' section";
    return false;
  }
  if (!version.IsScalar()) {
    err = at_line(version, "bad workspace version");
    return false;
  }
  {
    std::uint64_t v = 0;
    if (!parse_u64(version.Scalar(), v)) {
      err = at_line(version, "bad workspace version");
      return false;
    }
    if (v != 1 && v != static_cast<std::uint64_t>(kWorkspaceVersion)) {
      err = at_line(version, "unsupported workspace version " + std::to_string(v) + " (want 2)");
      return false;
    }
    if (v == 1 && ties && ties.IsDefined()) {
      err = at_line(ties, "version 1 workspaces must not carry 'ties:' (want version 2)");
      return false;
    }
    if (v == static_cast<std::uint64_t>(kWorkspaceVersion) && (!ties || !ties.IsDefined())) {
      err = "want 'ties:' section";
      return false;
    }
  }
  if (!gates.IsSequence() && !gates.IsNull()) {
    err = at_line(gates, "'gates:' must read a sequence");
    return false;
  }
  if (!wires.IsSequence() && !wires.IsNull()) {
    err = at_line(wires, "'wires:' must read a sequence");
    return false;
  }
  if (ties && ties.IsDefined() && !ties.IsSequence() && !ties.IsNull()) {
    err = at_line(ties, "'ties:' must read a sequence");
    return false;
  }
  if (!meta.IsMap()) {
    err = at_line(meta, "'meta:' must read a map");
    return false;
  }
  for (auto it = meta.begin(); it != meta.end(); ++it) {
    const std::string key = it->first.IsScalar() ? it->first.Scalar() : std::string{};
    if (key != "tool") {
      err = at_line(it->first, "unknown meta key (want 'tool: ...')");
      return false;
    }
  }
  const YAML::Node tool = meta["tool"];
  if (!tool || !tool.IsDefined() || !tool.IsScalar()) {
    err = "meta: missing 'tool: ...'";
    return false;
  }

  std::vector<GateRow> gate_rows;
  std::vector<WireRow> wire_rows;
  std::vector<TieRow> tie_rows;
  if (gates.IsSequence()) {
    gate_rows.reserve(gates.size());
    for (std::size_t i = 0; i < gates.size(); ++i) {
      const YAML::Node g = gates[i];
      if (!g.IsMap()) {
        err = at_line(g, "gate entry must read '{id: ..., kind: ..., inputs: ..., ignore: [...], ignore_output: ..., delay_ps: ...}'");
        return false;
      }
      GateRow row;
      if (!check_gate_map(g, row, err)) {
        return false;
      }
      gate_rows.push_back(std::move(row));
    }
  }
  if (wires.IsSequence()) {
    wire_rows.reserve(wires.size());
    for (std::size_t i = 0; i < wires.size(); ++i) {
      const YAML::Node w = wires[i];
      if (!w.IsMap()) {
        err = at_line(w, "wire entry must read '{from: ..., to: ...}'");
        return false;
      }
      WireRow row;
      if (!check_wire_map(w, row, err)) {
        return false;
      }
      wire_rows.push_back(std::move(row));
    }
  }
  if (ties && ties.IsDefined() && ties.IsSequence()) {
    tie_rows.reserve(ties.size());
    for (std::size_t i = 0; i < ties.size(); ++i) {
      const YAML::Node t = ties[i];
      if (!t.IsMap()) {
        err = at_line(t, "tie entry must read '{a: ..., b: ...}'");
        return false;
      }
      TieRow row;
      if (!check_tie_map(t, row, err)) {
        return false;
      }
      tie_rows.push_back(std::move(row));
    }
  }
  if (gate_rows.size() > kMaxGates) {
    err = at_line(gates, "too many gates (max 1024)");
    return false;
  }

  Netlist tmp{};
  for (std::size_t i = 0; i < gate_rows.size(); ++i) {
    const GateRow &g = gate_rows[i];
    for (std::size_t j = 0; j < i; ++j) {
      if (gate_rows[j].kind == g.kind && gate_rows[j].counter == g.counter) {
        err = "line " + std::to_string(wire_rows.empty() ? line_of(gates[i]) : line_of(gates[i])) + ": duplicate gate id '" +
              g.id + "'";
        return false;
      }
    }
    Gate &slot = tmp.gates[i];
    slot.live = true;
    slot.kind = g.kind;
    slot.counter = g.counter;
    slot.input_count = g.inputs;
    slot.ignored_mask = g.ignored_mask;
    slot.ignore_output = g.ignore_output;
    slot.output_fanout = 0;
    slot.propagation_delay_ps = g.delay_ps;
    for (std::size_t k = 0; k < kMaxInputs; ++k) {
      slot.drivers[k] = kNoDriver;
    }
    const std::uint8_t ord = kind_ordinal(g.kind);
    if (tmp.next_counter[ord] <= g.counter) {
      tmp.next_counter[ord] = g.counter + 1u;
      if (tmp.next_counter[ord] == 0) {
        tmp.next_counter[ord] = 1;
      }
    }
  }
  tmp.live_count = static_cast<std::uint16_t>(gate_rows.size());
  tmp.dirty = true;
  tmp.topo_size = 0;

  for (const WireRow &w : wire_rows) {
    const std::string at = "line " + std::to_string(w.line) + ": ";
    const auto from = parse_pin(tmp, w.from);
    if (!from.ok()) {
      if (from.error == std::string_view("unknown gate")) {
        err = at + "unknown gate in wire source '" + w.from + "'";
      } else {
        err = at + "bad wire source '" + w.from + "'";
      }
      return false;
    }
    const auto to = parse_pin(tmp, w.to);
    if (!to.ok()) {
      if (to.error == std::string_view("unknown gate")) {
        err = at + "unknown gate in wire destination '" + w.to + "'";
      } else {
        err = at + "bad wire destination '" + w.to + "'";
      }
      return false;
    }
    if (!from.value.is_output || to.value.is_output) {
      err = at + "wire needs FROM.Y TO.i (got '" + w.from + "' -> '" + w.to + "')";
      return false;
    }
    Gate &dst = tmp.gates[to.value.slot];
    if (dst.drivers[to.value.input] != kNoDriver) {
      err = at + "input already wired '" + w.to + "'";
      return false;
    }
    if (detail::reaches(tmp, to.value.slot, from.value.slot)) {
      err = at + "wire closes a combinational cycle '" + w.from + "' -> '" + w.to + "'";
      return false;
    }
    dst.drivers[to.value.input] = from.value.slot;
    ++tmp.gates[from.value.slot].output_fanout;
  }
  for (const TieRow &t : tie_rows) {
    const std::string at = "line " + std::to_string(t.line) + ": ";
    const auto a = parse_pin(tmp, t.a);
    if (!a.ok()) {
      if (a.error == std::string_view("unknown gate")) {
        err = at + "unknown gate in tie pin '" + t.a + "'";
      } else {
        err = at + "bad tie pin '" + t.a + "'";
      }
      return false;
    }
    const auto b = parse_pin(tmp, t.b);
    if (!b.ok()) {
      if (b.error == std::string_view("unknown gate")) {
        err = at + "unknown gate in tie pin '" + t.b + "'";
      } else {
        err = at + "bad tie pin '" + t.b + "'";
      }
      return false;
    }
    if (a.value.is_output || b.value.is_output) {
      err = at + "tie needs two input pins (got '" + t.a + "' + '" + t.b + "')";
      return false;
    }
    if (a.value.slot == b.value.slot && a.value.input == b.value.input) {
      err = at + "tie needs two distinct pins (got '" + t.a + "' twice)";
      return false;
    }
    const Gate &ga = tmp.gates[a.value.slot];
    const Gate &gb = tmp.gates[b.value.slot];
    if (ga.drivers[a.value.input] != kNoDriver || gb.drivers[b.value.input] != kNoDriver) {
      err = at + "tie needs dangling pins (unwire first)";
      return false;
    }
    if (((ga.ignored_mask & static_cast<std::uint8_t>(1u << a.value.input)) != 0) ||
        ((gb.ignored_mask & static_cast<std::uint8_t>(1u << b.value.input)) != 0)) {
      err = at + "tie needs non-ignored pins (use first)";
      return false;
    }
    if (!tie_pins(tmp, a.value, b.value).ok()) {
      err = at + "bad tie '" + t.a + "' + '" + t.b + "'";
      return false;
    }
  }
  tmp.dirty = true;
  tmp.topo_size = 0;
  if (!ensure_topology(tmp).ok()) {
    err = "workspace topology error";
    return false;
  }
  out = tmp;
  return true;
}

} // namespace

void save_text(const Netlist &nl, std::string &out) {
  out += "version: 2\n";
  out += "gates:\n";
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    const Gate &g = nl.gates[s];
    if (!g.live) {
      continue;
    }
    out += "  - {id: ";
    out += gate_name(nl, s);
    out += ", kind: ";
    out += lower_kind(g.kind);
    out += ", inputs: ";
    out += std::to_string(static_cast<unsigned>(g.input_count));
    out += ", ignore: [";
    bool first = true;
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      if ((g.ignored_mask & static_cast<std::uint8_t>(1u << i)) == 0) {
        continue;
      }
      if (!first) {
        out += ", ";
      }
      first = false;
      out += std::to_string(static_cast<unsigned>(i) + 1u);
    }
    out += "], ignore_output: ";
    out += g.ignore_output ? "true" : "false";
    out += ", delay_ps: ";
    out += std::to_string(g.propagation_delay_ps);
    out += "}\n";
  }
  out += "wires:\n";
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    const Gate &g = nl.gates[s];
    if (!g.live) {
      continue;
    }
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      const std::uint16_t drv = g.drivers[i];
      if (drv == kNoDriver) {
        continue;
      }
      out += "  - {from: ";
      out += pin_text(nl, drv, true, 0);
      out += ", to: ";
      out += pin_text(nl, s, false, i);
      out += "}\n";
    }
  }
  out += "ties:\n";
  // Canonical: one entry per non-leader tied member (a = leader, b = member),
  // ordered by b in slot order so save/load round-trips byte-identically.
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    const Gate &g = nl.gates[s];
    if (!g.live) {
      continue;
    }
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      if (g.drivers[i] != kNoDriver) {
        continue;
      }
      if ((g.ignored_mask & static_cast<std::uint8_t>(1u << i)) != 0) {
        continue;
      }
      const std::size_t flat = tie_flat(s, i);
      if (nl.tie_rep[flat] == 0) {
        continue;
      }
      const std::size_t leader = tie_leader_flat(nl, flat);
      if (leader == flat) {
        continue;
      }
      const auto lslot = static_cast<std::uint16_t>(leader / kMaxInputs);
      const auto linput = static_cast<std::uint8_t>(leader % kMaxInputs);
      out += "  - {a: ";
      out += pin_text(nl, lslot, false, linput);
      out += ", b: ";
      out += pin_text(nl, s, false, i);
      out += "}\n";
    }
  }
  out += "meta:\n";
  out += "  tool: ";
  out += kToolName;
  out += "\n";
}

bool load_text(const std::string &text, Netlist &out, std::string &err) {
  try {
    return load_impl(text, out, err);
  } catch (const YAML::Exception &e) {
    const int line = e.mark.line;
    if (line < 0) {
      err = std::string("bad YAML: ") + e.msg;
    } else {
      err = "line " + std::to_string(static_cast<std::size_t>(line) + 1u) + ": " + e.msg;
    }
    return false;
  } catch (const std::exception &e) {
    err = std::string("bad YAML: ") + e.what();
    return false;
  }
}

} // namespace wengine::logic::yaml
