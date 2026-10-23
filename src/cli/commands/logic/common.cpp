#include "common.hpp"

#include "wengine/logic/eval.hpp"
#include "wengine/logic/types.hpp"

#include <fstream>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wengine::cli::logic {

using wengine::logic::BlockPin;
using wengine::logic::GateKind;
using wengine::logic::kMaxGates;
using wengine::logic::kMaxInputs;
using wengine::logic::kNoDriver;
using wengine::logic::Netlist;

namespace support {

void push_matches(std::string_view partial, std::span<const std::string_view> options, std::vector<std::string> &out) {
  for (std::string_view o : options) {
    if (o.substr(0, partial.size()) == partial) {
      out.emplace_back(o);
    }
  }
}

[[nodiscard]] bool starts_with(std::string_view text, std::string_view prefix) noexcept {
  return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

[[nodiscard]] std::string gate_name(const Netlist &nl, std::uint16_t slot) {
  std::string s(wengine::logic::gate_kind_name(nl.gates[slot].kind));
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

} // namespace support

namespace {

[[nodiscard]] std::string lower_kind(GateKind k) {
  std::string s(wengine::logic::gate_kind_name(k));
  for (char &c : s) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - ('A' - 'a'));
    }
  }
  return s;
}

} // namespace

namespace support {

void collect_gate_names(const Netlist &nl, std::string_view partial, std::vector<std::string> &out) {
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    if (!nl.gates[s].live) {
      continue;
    }
    std::string name = gate_name(nl, s);
    if (name.size() >= partial.size() && name.substr(0, partial.size()) == partial) {
      out.push_back(name);
    }
  }
}

void collect_pins(const Netlist &nl, bool want_out, bool want_in, std::string_view partial, std::vector<std::string> &out) {
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    const auto &g = nl.gates[s];
    if (!g.live) {
      continue;
    }
    if (want_out) {
      std::string p = pin_text(nl, s, true, 0);
      if (p.size() >= partial.size() && p.substr(0, partial.size()) == partial) {
        out.push_back(p);
      }
    }
    if (want_in) {
      for (std::uint8_t i = 0; i < g.input_count; ++i) {
        std::string p = pin_text(nl, s, false, i);
        if (p.size() >= partial.size() && p.substr(0, partial.size()) == partial) {
          out.push_back(p);
        }
      }
    }
  }
}

void collect_block_inputs(const Netlist &nl, std::string_view partial, std::vector<std::string> &out) {
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    const auto &g = nl.gates[s];
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
      std::string p = pin_text(nl, s, false, i);
      if (p.size() >= partial.size() && p.substr(0, partial.size()) == partial) {
        out.push_back(p);
      }
    }
  }
}

void interface_pins(const Netlist &nl, std::vector<BlockPin> &in_pins, std::vector<BlockPin> &out_pins) {
  const std::size_t nin = wengine::logic::block_input_count(nl);
  const std::size_t nout = wengine::logic::block_output_count(nl);
  in_pins.assign(nin, BlockPin{});
  out_pins.assign(nout, BlockPin{});
  std::size_t nin_got = nin;
  std::size_t nout_got = nout;
  const core::Status st = wengine::logic::block_interface(nl, in_pins.data(), in_pins.size(), &nin_got, out_pins.data(), out_pins.size(), &nout_got);
  (void)st; // sizes are exact by construction
}

core::Status write_truth_csv(Netlist &nl, std::uint64_t rows, const std::vector<BlockPin> &in_pins,
                             const std::vector<BlockPin> &out_pins, std::ostream &os) {
  // Header: input pins then output pins.
  bool first = true;
  for (const BlockPin &p : in_pins) {
    if (!first) {
      os << ',';
    }
    first = false;
    os << pin_text(nl, p.slot, false, p.input);
  }
  for (const BlockPin &p : out_pins) {
    if (!first) {
      os << ',';
    }
    first = false;
    os << pin_text(nl, p.slot, true, 0);
  }
  os << '\n';
  if (!os) {
    return core::Status::failure(core::StatusCode::kInternal);
  }
  const std::size_t nout = out_pins.size();
  std::vector<std::uint64_t> scratch(kMaxGates, 0);
  std::vector<std::uint64_t> words(nout == 0 ? 1 : nout, 0);
  const std::uint64_t nwords = (rows + 63ULL) / 64ULL;
  for (std::uint64_t w = 0; w < nwords; ++w) {
    std::span<std::uint64_t> out_span = nout == 0 ? std::span<std::uint64_t>{} : std::span<std::uint64_t>{words.data(), nout};
    const core::Status st = wengine::logic::eval_block_word(nl, w, std::span<std::uint64_t>{scratch.data(), scratch.size()}, out_span);
    if (!st.ok()) {
      return st;
    }
    for (std::uint64_t b = 0; b < 64; ++b) {
      const std::uint64_t row = w * 64ULL + b;
      if (row >= rows) {
        break;
      }
      bool col_first = true;
      for (std::size_t j = 0; j < in_pins.size(); ++j) {
        if (!col_first) {
          os << ',';
        }
        col_first = false;
        os << ((((row >> j) & 1ULL) != 0ULL) ? '1' : '0');
      }
      for (std::size_t o = 0; o < nout; ++o) {
        if (!col_first) {
          os << ',';
        }
        col_first = false;
        os << ((((words[o] >> b) & 1ULL) != 0ULL) ? '1' : '0');
      }
      os << '\n';
      if (!os) {
        return core::Status::failure(core::StatusCode::kInternal);
      }
    }
  }
  return core::Status::success();
}

void save_yaml(std::ostream &os, const Netlist &nl) {
  os << "version: 1\n";
  os << "gates:\n";
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    const auto &g = nl.gates[s];
    if (!g.live) {
      continue;
    }
    os << "  - {id: " << gate_name(nl, s) << ", kind: " << lower_kind(g.kind) << ", inputs: "
       << static_cast<unsigned>(g.input_count) << ", ignore: [";
    bool first = true;
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      if ((g.ignored_mask & static_cast<std::uint8_t>(1u << i)) == 0) {
        continue;
      }
      if (!first) {
        os << ", ";
      }
      first = false;
      os << (static_cast<unsigned>(i) + 1u);
    }
    os << "], ignore_output: " << (g.ignore_output ? "true" : "false") << ", delay_ps: " << g.propagation_delay_ps << "}\n";
  }
  os << "wires:\n";
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    const auto &g = nl.gates[s];
    if (!g.live) {
      continue;
    }
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      const std::uint16_t drv = g.drivers[i];
      if (drv == kNoDriver) {
        continue;
      }
      os << "  - {from: " << pin_text(nl, drv, true, 0) << ", to: " << pin_text(nl, s, false, i) << "}\n";
    }
  }
  os << "meta:\n";
  os << "  tool: wengine-logic\n";
}

[[nodiscard]] std::size_t wire_count(const Netlist &nl) noexcept {
  std::size_t n = 0;
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    const auto &g = nl.gates[s];
    if (!g.live) {
      continue;
    }
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      if (g.drivers[i] != kNoDriver) {
        ++n;
      }
    }
  }
  return n;
}

void complete_from_value(const Netlist &nl, std::string_view val, std::vector<std::string> &out) {
  const std::size_t eq = val.find('=');
  if (eq == std::string_view::npos) {
    std::vector<std::string> pins;
    collect_block_inputs(nl, val, pins);
    // Exact pin hit: offer the two valued forms instead of the bare pin.
    bool exact = false;
    for (const std::string &p : pins) {
      if (p == val) {
        exact = true;
        break;
      }
    }
    if (exact) {
      out.emplace_back(std::string(val) + "=0");
      out.emplace_back(std::string(val) + "=1");
      return;
    }
    for (std::string &p : pins) {
      out.push_back(p);
    }
    return;
  }
  const std::string_view pin = val.substr(0, eq);
  const std::string_view bit = val.substr(eq + 1);
  std::vector<std::string> pins;
  collect_block_inputs(nl, pin, pins);
  bool known = false;
  for (const std::string &p : pins) {
    if (p == pin) {
      known = true;
      break;
    }
  }
  if (!known) {
    return;
  }
  if (std::string_view("0").substr(0, bit.size()) == bit) {
    out.emplace_back(std::string(pin) + "=0");
  }
  if (std::string_view("1").substr(0, bit.size()) == bit) {
    out.emplace_back(std::string(pin) + "=1");
  }
}

} // namespace support

namespace support {

namespace yaml {

[[nodiscard]] std::string trim(std::string_view s) {
  std::size_t b = 0;
  while (b < s.size() && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r')) {
    ++b;
  }
  std::size_t e = s.size();
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) {
    --e;
  }
  return std::string(s.substr(b, e - b));
}

// Split a `{...}` inner body on top-level commas (brackets nest one level).
bool split_fields(std::string_view body, std::vector<std::string_view> &fields, std::string &err) {
  fields.clear();
  int depth = 0;
  std::size_t start = 0;
  for (std::size_t i = 0; i < body.size(); ++i) {
    const char c = body[i];
    if (c == '[') {
      ++depth;
    } else if (c == ']') {
      --depth;
      if (depth < 0) {
        err = "unbalanced brackets";
        return false;
      }
    } else if (c == ',' && depth == 0) {
      fields.push_back(body.substr(start, i - start));
      start = i + 1;
    }
  }
  if (depth != 0) {
    err = "unbalanced brackets";
    return false;
  }
  fields.push_back(body.substr(start));
  return true;
}

bool split_kv(std::string_view field, std::string &key, std::string &val, std::string &err) {
  const std::size_t colon = field.find(':');
  if (colon == std::string_view::npos) {
    err = "expected 'key: value'";
    return false;
  }
  key = trim(field.substr(0, colon));
  val = trim(field.substr(colon + 1));
  if (key.empty()) {
    err = "empty key";
    return false;
  }
  return true;
}

bool parse_u64(std::string_view text, std::uint64_t &out, std::string &err) {
  if (text.empty()) {
    err = "expected a number";
    return false;
  }
  std::uint64_t v = 0;
  for (char c : text) {
    if (c < '0' || c > '9') {
      err = "expected a number";
      return false;
    }
    const auto d = static_cast<std::uint64_t>(c - '0');
    if (v > (0xFFFFFFFFFFFFFFFFULL - d) / 10ULL) {
      err = "number out of range";
      return false;
    }
    v = v * 10ULL + d;
  }
  out = v;
  return true;
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
};

bool parse_gate_line(std::string_view line, GateRow &row, std::string &err) {
  // `  - {id: AND1, kind: and, inputs: 2, ignore: [2], ignore_output: false, delay_ps: 0}`
  std::string t = trim(line);
  if (!support::starts_with(t, "- {") || t.back() != '}') {
    err = "expected '  - {id: ..., kind: ..., inputs: ..., ignore: [...], ignore_output: ..., delay_ps: ...}'";
    return false;
  }
  const std::string_view body(t.data() + 3, t.size() - 4);
  std::vector<std::string_view> fields;
  if (!split_fields(body, fields, err)) {
    return false;
  }
  if (fields.size() != 6) {
    err = "gate entry needs 6 fields (id, kind, inputs, ignore, ignore_output, delay_ps)";
    return false;
  }
  const char *want[6] = {"id", "kind", "inputs", "ignore", "ignore_output", "delay_ps"};
  std::string vals[6];
  for (int k = 0; k < 6; ++k) {
    std::string key;
    std::string val;
    if (!split_kv(fields[static_cast<std::size_t>(k)], key, val, err)) {
      return false;
    }
    if (key != want[k]) {
      err = std::string("gate field ") + std::to_string(k + 1) + " must be '" + want[k] + "'";
      return false;
    }
    vals[k] = val;
  }
  if (vals[0].empty()) {
    err = "empty gate id";
    return false;
  }
  row.id = vals[0];
  const auto kind = wengine::logic::parse_gate_kind(vals[1]);
  if (!kind.ok()) {
    err = "bad gate kind (want AND|OR|NAND|NOR|XOR|XNOR|NOT|BUF)";
    return false;
  }
  row.kind = kind.value;
  std::uint64_t n = 0;
  if (!parse_u64(vals[2], n, err) || n == 0 || n > kMaxInputs) {
    err = "bad input count (want 1..8)";
    return false;
  }
  row.inputs = static_cast<std::uint8_t>(n);
  if (!wengine::logic::valid_fanin(row.kind, row.inputs)) {
    err = "bad fan-in for kind";
    return false;
  }
  // Id must read `<KIND><counter>` with the same kind (case-insensitive).
  const std::string kname(wengine::logic::gate_kind_name(row.kind));
  if (row.id.size() <= kname.size()) {
    err = "gate id must read KIND+counter (e.g. AND1)";
    return false;
  }
  for (std::size_t i = 0; i < kname.size(); ++i) {
    char a = row.id[i];
    if (a >= 'a' && a <= 'z') {
      a = static_cast<char>(a - ('a' - 'A'));
    }
    if (a != kname[i]) {
      err = "gate id/kind mismatch";
      return false;
    }
  }
  std::uint64_t counter = 0;
  if (!parse_u64(std::string_view(row.id.data() + kname.size(), row.id.size() - kname.size()), counter, err) || counter == 0 ||
      counter > 0xFFFFFFFFULL) {
    err = "gate id needs a counter >= 1 (e.g. AND1)";
    return false;
  }
  row.counter = static_cast<std::uint32_t>(counter);
  // ignore: [2, 3] — ascending, unique, in range.
  std::string ig = trim(vals[3]);
  if (ig.size() < 2 || ig.front() != '[' || ig.back() != ']') {
    err = "ignore must read [i, ...]";
    return false;
  }
  row.ignored_mask = 0;
  const std::string inner = trim(std::string_view(ig.data() + 1, ig.size() - 2));
  if (!inner.empty()) {
    std::size_t start = 0;
    unsigned prev = 0;
    while (true) {
      const std::size_t comma = inner.find(',', start);
      const std::string tok = trim(std::string_view(inner.data() + start, (comma == std::string::npos ? inner.size() : comma) - start));
      std::uint64_t idx = 0;
      if (!parse_u64(tok, idx, err) || idx == 0 || idx > row.inputs) {
        err = "ignore index out of range";
        return false;
      }
      if (idx <= prev) {
        err = "ignore indices must ascend";
        return false;
      }
      prev = static_cast<unsigned>(idx);
      row.ignored_mask = static_cast<std::uint8_t>(row.ignored_mask | (1u << (idx - 1u)));
      if (comma == std::string::npos) {
        break;
      }
      start = comma + 1;
    }
  }
  if (vals[4] == "true") {
    row.ignore_output = true;
  } else if (vals[4] == "false") {
    row.ignore_output = false;
  } else {
    err = "ignore_output must be true|false";
    return false;
  }
  if (!parse_u64(vals[5], row.delay_ps, err)) {
    err = "bad delay_ps";
    return false;
  }
  return true;
}

bool parse_wire_line(std::string_view line, WireRow &row, std::string &err) {
  std::string t = trim(line);
  if (!support::starts_with(t, "- {") || t.back() != '}') {
    err = "expected '  - {from: ..., to: ...}'";
    return false;
  }
  const std::string_view body(t.data() + 3, t.size() - 4);
  std::vector<std::string_view> fields;
  if (!split_fields(body, fields, err)) {
    return false;
  }
  if (fields.size() != 2) {
    err = "wire entry needs 2 fields (from, to)";
    return false;
  }
  std::string k0;
  std::string v0;
  std::string k1;
  std::string v1;
  if (!split_kv(fields[0], k0, v0, err) || !split_kv(fields[1], k1, v1, err)) {
    return false;
  }
  if (k0 != "from" || k1 != "to") {
    err = "wire fields must read 'from' then 'to'";
    return false;
  }
  if (v0.empty() || v1.empty()) {
    err = "empty wire pin";
    return false;
  }
  row.from = v0;
  row.to = v1;
  return true;
}

} // namespace yaml

} // namespace support

namespace support {

namespace yaml {

bool load_text(const std::string &text, Netlist &out, std::string &err) {
  std::vector<std::string> lines;
  {
    std::size_t pos = 0;
    while (pos <= text.size()) {
      const std::size_t nl = text.find('\n', pos);
      std::string line = (nl == std::string::npos) ? text.substr(pos) : text.substr(pos, nl - pos);
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      lines.push_back(line);
      if (nl == std::string::npos) {
        break;
      }
      pos = nl + 1;
    }
  }
  auto skipped = [](std::string_view l) {
    for (char c : l) {
      if (c == ' ' || c == '\t' || c == '\r') {
        continue;
      }
      return c == '#';
    }
    return true; // blank
  };
  std::size_t cur = 0;
  auto next = [&](std::string &line, std::size_t &number) -> bool {
    while (cur < lines.size()) {
      line = lines[cur];
      number = cur + 1;
      ++cur;
      if (!skipped(line)) {
        return true;
      }
    }
    return false;
  };
  Netlist tmp{};
  std::string line;
  std::size_t number = 0;
  if (!next(line, number)) {
    err = "empty workspace (want 'version: 1' first)";
    return false;
  }
  {
    const std::string t = trim(line);
    if (!support::starts_with(t, "version:")) {
      err = "first line must read 'version: 1'";
      return false;
    }
    std::uint64_t v = 0;
    std::string verr;
    if (!parse_u64(trim(std::string_view(t.data() + 8, t.size() - 8)), v, verr)) {
      err = "bad workspace version";
      return false;
    }
    if (v != 1) {
      err = "unsupported workspace version " + std::to_string(v) + " (want 1)";
      return false;
    }
  }
  if (!next(line, number) || trim(line) != "gates:") {
    err = "want 'gates:' section";
    return false;
  }
  std::vector<GateRow> gates;
  std::vector<WireRow> wires;
  bool in_wires = false;
  bool in_meta = false;
  bool have_tool = false;
  while (next(line, number)) {
    const std::string t = trim(line);
    if (!in_wires && t == "wires:") {
      in_wires = true;
      continue;
    }
    if (in_wires && t == "meta:") {
      in_meta = true;
      break;
    }
    if (!in_wires) {
      GateRow g;
      if (!parse_gate_line(line, g, err)) {
        err = "line " + std::to_string(number) + ": " + err;
        return false;
      }
      gates.push_back(g);
      continue;
    }
    WireRow w;
    if (!parse_wire_line(line, w, err)) {
      err = "line " + std::to_string(number) + ": " + err;
      return false;
    }
    wires.push_back(w);
  }
  if (!in_wires) {
    err = "want 'wires:' section";
    return false;
  }
  if (!in_meta) {
    err = "want 'meta:' section";
    return false;
  }
  while (next(line, number)) {
    const std::string t = trim(line);
    if (!support::starts_with(t, "tool:")) {
      err = "line " + std::to_string(number) + ": unknown meta key (want 'tool: ...')";
      return false;
    }
    have_tool = true;
  }
  if (!have_tool) {
    err = "meta: missing 'tool: ...'";
    return false;
  }
  if (gates.size() > kMaxGates) {
    err = "too many gates (max 1024)";
    return false;
  }
  // Place gates in file order; ids fix the per-kind counters.
  for (std::size_t i = 0; i < gates.size(); ++i) {
    const GateRow &g = gates[i];
    for (std::size_t j = 0; j < i; ++j) {
      if (gates[j].kind == g.kind && gates[j].counter == g.counter) {
        err = "duplicate gate id '" + g.id + "'";
        return false;
      }
    }
    auto &slot = tmp.gates[i];
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
    const std::uint8_t ord = wengine::logic::kind_ordinal(g.kind);
    if (tmp.next_counter[ord] <= g.counter) {
      tmp.next_counter[ord] = g.counter + 1;
      if (tmp.next_counter[ord] == 0) {
        tmp.next_counter[ord] = 1;
      }
    }
  }
  tmp.live_count = static_cast<std::uint16_t>(gates.size());
  tmp.dirty = true;
  tmp.topo_size = 0;
  for (const WireRow &w : wires) {
    const auto from = wengine::logic::parse_pin(tmp, w.from);
    if (!from.ok()) {
      err = "bad wire source '" + w.from + "'";
      return false;
    }
    const auto to = wengine::logic::parse_pin(tmp, w.to);
    if (!to.ok()) {
      err = "bad wire destination '" + w.to + "'";
      return false;
    }
    if (!from.value.is_output || to.value.is_output) {
      err = "wire needs FROM.Y TO.i (got '" + w.from + "' -> '" + w.to + "')";
      return false;
    }
    auto &dst = tmp.gates[to.value.slot];
    if (dst.drivers[to.value.input] != kNoDriver) {
      err = "input already wired '" + w.to + "'";
      return false;
    }
    if (wengine::logic::detail::reaches(tmp, to.value.slot, from.value.slot)) {
      err = "wire closes a combinational cycle '" + w.from + "' -> '" + w.to + "'";
      return false;
    }
    dst.drivers[to.value.input] = from.value.slot;
    ++tmp.gates[from.value.slot].output_fanout;
  }
  tmp.dirty = true;
  tmp.topo_size = 0;
  if (!wengine::logic::ensure_topology(tmp).ok()) {
    err = "workspace topology error";
    return false;
  }
  out = tmp;
  return true;
}

} // namespace yaml

Schematic layout_schematic(Netlist &nl) {
  Schematic sc;
  if (nl.live_count == 0) {
    sc.width = 0;
    sc.height = 0;
    return sc;
  }
  int max_level = 0;
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    if (nl.gates[s].live && nl.topo_level[s] > max_level) {
      max_level = nl.topo_level[s];
    }
  }
  sc.levels = max_level + 1;
  constexpr int kColW = 150;
  constexpr int kBoxW = 110;
  constexpr int kBoxH = 40;
  constexpr int kGapY = 30;
  constexpr int kMargin = 30;
  std::vector<int> per_level(static_cast<std::size_t>(sc.levels), 0);
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    if (!nl.gates[s].live) {
      continue;
    }
    const int lvl = nl.topo_level[s];
    const int idx = per_level[static_cast<std::size_t>(lvl)]++;
    Placed p;
    p.slot = s;
    p.level = lvl;
    p.x = kMargin + lvl * kColW + (kColW - kBoxW) / 2;
    p.y = kMargin + idx * (kBoxH + kGapY);
    sc.gates.push_back(p);
  }
  int max_count = 1;
  for (int c : per_level) {
    if (c > max_count) {
      max_count = c;
    }
  }
  sc.width = kMargin * 2 + sc.levels * kColW;
  sc.height = kMargin * 2 + max_count * (kBoxH + kGapY);
  return sc;
}

void draw_ascii(std::ostream &os, const Netlist &nl, const Schematic &sc) {
  os << "schematic (gates=" << nl.live_count << ", levels=" << sc.levels << "):\n";
  if (nl.live_count == 0) {
    os << "  (empty)\n";
    return;
  }
  for (int lvl = 0; lvl < sc.levels; ++lvl) {
    os << "  L" << lvl << ":";
    for (const Placed &p : sc.gates) {
      if (p.level == lvl) {
        os << ' ' << gate_name(nl, p.slot);
      }
    }
    os << '\n';
  }
  std::size_t nw = wire_count(nl);
  os << "wires (" << nw << "):\n";
  if (nw == 0) {
    os << "  (none)\n";
    return;
  }
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    const auto &g = nl.gates[s];
    if (!g.live) {
      continue;
    }
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      const std::uint16_t drv = g.drivers[i];
      if (drv == kNoDriver) {
        continue;
      }
      os << "  " << pin_text(nl, drv, true, 0) << " -> " << pin_text(nl, s, false, i) << '\n';
    }
  }
}

bool write_bmp(const std::string &path, const Netlist &nl, const Schematic &sc, int &w_out, int &h_out, std::string &err) {
  constexpr int kBoxW = 110;
  constexpr int kBoxH = 40;
  const int w = sc.width;
  const int h = sc.height;
  w_out = w;
  h_out = h;
  std::vector<std::uint8_t> px(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 3, 255);
  auto set_px = [&](int x, int y, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    if (x < 0 || y < 0 || x >= w || y >= h) {
      return;
    }
    std::uint8_t *p = &px[(static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) * 3];
    p[0] = r;
    p[1] = g;
    p[2] = b;
  };
  auto hline = [&](int x0, int x1, int y) {
    if (x0 > x1) {
      const int t = x0;
      x0 = x1;
      x1 = t;
    }
    for (int x = x0; x <= x1; ++x) {
      set_px(x, y, 0, 0, 0);
      set_px(x, y + 1, 0, 0, 0);
    }
  };
  auto vline = [&](int x, int y0, int y1) {
    if (y0 > y1) {
      const int t = y0;
      y0 = y1;
      y1 = t;
    }
    for (int y = y0; y <= y1; ++y) {
      set_px(x, y, 0, 0, 0);
      set_px(x + 1, y, 0, 0, 0);
    }
  };
  // Wires first (under the boxes): Manhattan dogleg through the mid column.
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    const auto &g = nl.gates[s];
    if (!g.live) {
      continue;
    }
    const Placed *dst = nullptr;
    for (const Placed &p : sc.gates) {
      if (p.slot == s) {
        dst = &p;
        break;
      }
    }
    if (dst == nullptr) {
      continue;
    }
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      const std::uint16_t drv = g.drivers[i];
      if (drv == kNoDriver) {
        continue;
      }
      const Placed *src = nullptr;
      for (const Placed &p : sc.gates) {
        if (p.slot == drv) {
          src = &p;
          break;
        }
      }
      if (src == nullptr) {
        continue;
      }
      const int n = g.input_count;
      const int y0 = src->y + kBoxH / 2;
      const int y1 = dst->y + (static_cast<int>(i) + 1) * kBoxH / (n + 1);
      const int x0 = src->x + kBoxW;
      const int x1 = dst->x;
      const int xm = (x0 + x1) / 2;
      hline(x0, xm, y0);
      vline(xm, y0, y1);
      hline(xm, x1, y1);
    }
  }
  // Boxes: gray fill, black border.
  for (const Placed &p : sc.gates) {
    for (int y = p.y; y < p.y + kBoxH; ++y) {
      for (int x = p.x; x < p.x + kBoxW; ++x) {
        set_px(x, y, 200, 200, 200);
      }
    }
    for (int x = p.x; x < p.x + kBoxW; ++x) {
      set_px(x, p.y, 0, 0, 0);
      set_px(x, p.y + 1, 0, 0, 0);
      set_px(x, p.y + kBoxH - 2, 0, 0, 0);
      set_px(x, p.y + kBoxH - 1, 0, 0, 0);
    }
    for (int y = p.y; y < p.y + kBoxH; ++y) {
      set_px(p.x, y, 0, 0, 0);
      set_px(p.x + 1, y, 0, 0, 0);
      set_px(p.x + kBoxW - 2, y, 0, 0, 0);
      set_px(p.x + kBoxW - 1, y, 0, 0, 0);
    }
  }
  const std::size_t stride = (static_cast<std::size_t>(w) * 3 + 3) & ~std::size_t(3);
  const std::size_t img = stride * static_cast<std::size_t>(h);
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) {
    err = "cannot open file '" + path + "'";
    return false;
  }
  std::uint8_t hdr[54] = {};
  hdr[0] = 'B';
  hdr[1] = 'M';
  auto put32 = [&](int off, std::uint32_t v) {
    hdr[off] = static_cast<std::uint8_t>(v & 0xFFu);
    hdr[off + 1] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
    hdr[off + 2] = static_cast<std::uint8_t>((v >> 16) & 0xFFu);
    hdr[off + 3] = static_cast<std::uint8_t>((v >> 24) & 0xFFu);
  };
  auto put16 = [&](int off, std::uint32_t v) {
    hdr[off] = static_cast<std::uint8_t>(v & 0xFFu);
    hdr[off + 1] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
  };
  put32(2, static_cast<std::uint32_t>(54 + img));
  put32(10, 54);
  put32(14, 40);
  put32(18, static_cast<std::uint32_t>(w));
  put32(22, static_cast<std::uint32_t>(h));
  put16(26, 1);
  put16(28, 24);
  put32(34, static_cast<std::uint32_t>(img));
  f.write(reinterpret_cast<const char *>(hdr), sizeof(hdr));
  std::vector<std::uint8_t> row(stride, 0);
  for (int y = h - 1; y >= 0; --y) {
    for (int x = 0; x < w; ++x) {
      const std::uint8_t *p = &px[(static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) * 3];
      row[static_cast<std::size_t>(x) * 3 + 0] = p[2];
      row[static_cast<std::size_t>(x) * 3 + 1] = p[1];
      row[static_cast<std::size_t>(x) * 3 + 2] = p[0];
    }
    f.write(reinterpret_cast<const char *>(row.data()), static_cast<std::streamsize>(stride));
  }
  f.close();
  if (!f) {
    err = "cannot write file '" + path + "'";
    return false;
  }
  return true;
}

[[nodiscard]] bool has_bmp_suffix(std::string_view path) noexcept {
  if (path.size() < 4) {
    return false;
  }
  const std::string_view suf = path.substr(path.size() - 4);
  return suf == ".bmp" || suf == ".BMP";
}

} // namespace support

} // namespace wengine::cli::logic
