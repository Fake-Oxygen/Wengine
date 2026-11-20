// Row checkers + SectionSpec table for the versioned workspace store.
//
// One checker per sequence section plus the single table driving the
// generic collect loop (presence, sequence-shape guards, row loops) and
// the root-key allowlist diagnostic. Adding a section is one table row +
// one checker + one replay function (see yaml_store_replay.cpp).

#include "yaml_store_detail.hpp"

#include "wengine/logic/types.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wengine::logic::yaml::detail {

[[nodiscard]] bool check_gate_map(const YAML::Node &g, GateRow &row, std::string &err) {
  constexpr const char *kWant[6] = {"id", "kind", "inputs", "ignore", "ignore_output", "delay_ps"};
  for (auto it = g.begin(); it != g.end(); ++it) {
    const std::string key = it->first.IsScalar() ? it->first.Scalar() : std::string{};
    bool known = (key == "ignore_nq") || (key == "init"); // optional 7th/8th keys (sequential gates)
    for (const char *w : kWant) {
      if (key == w) {
        known = true;
        break;
      }
    }
    if (!known) {
      err = at_line(it->first, "unknown gate key '" + key + "' (want id|kind|inputs|ignore|ignore_output|delay_ps|ignore_nq|init)");
      return false;
    }
  }
  for (const char *w : kWant) {
    const YAML::Node v = g[w];
    if (!v || !v.IsDefined()) {
      err = at_line(g, std::string("gate entry needs 6 fields (id, kind, inputs, ignore, ignore_output, delay_ps), missing '") + w + "'");
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
    err = at_line(kind, "bad gate kind (want AND|OR|NAND|NOR|XOR|XNOR|NOT|BUF|TBUF|DFF|DLATCH|JK|T|SRLATCH)");
    return false;
  }
  const auto parsed_kind = parse_gate_kind(kind.Scalar());
  if (!parsed_kind.ok()) {
    err = at_line(kind, "bad gate kind (want AND|OR|NAND|NOR|XOR|XNOR|NOT|BUF|TBUF|DFF|DLATCH|JK|T|SRLATCH)");
    return false;
  }
  row.kind = parsed_kind.value;
  // NQ defaults to ignored on sequential gates (explicit `ignore_nq: false`
  // re-exposes, same as the `use` verb); combinational gates never read it.
  row.ignore_nq = is_sequential_kind(row.kind);
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
  // Optional 7th key: missing means false (all pre-NQ files).
  const YAML::Node ignore_nq = g["ignore_nq"];
  if (ignore_nq && ignore_nq.IsDefined()) {
    if (!ignore_nq.IsScalar()) {
      err = at_line(ignore_nq, "ignore_nq must be true|false");
      return false;
    }
    if (ignore_nq.Scalar() == "true") {
      row.ignore_nq = true;
    } else if (ignore_nq.Scalar() == "false") {
      row.ignore_nq = false;
    } else {
      err = at_line(ignore_nq, "ignore_nq must be true|false");
      return false;
    }
  }
  // Optional 8th key: per-sequential-row power-on value. Missing means
  // Low (hand-written files stay small); combinational rows must not
  // carry it.
  const YAML::Node init = g["init"];
  if (init && init.IsDefined()) {
    if (!is_sequential_kind(row.kind)) {
      err = at_line(init, "init needs a sequential gate (got '" + row.id + "')");
      return false;
    }
    if (!init.IsScalar()) {
      err = at_line(init, "bad init value (want 0|1)");
      return false;
    }
    if (init.Scalar() == "1") {
      row.has_init = true;
      row.init = LogicValue::kHigh;
    } else if (init.Scalar() == "0") {
      row.has_init = true;
      row.init = LogicValue::kLow;
    } else {
      err = at_line(init, "bad init value (want 0|1)");
      return false;
    }
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

[[nodiscard]] bool check_clocks_map(const YAML::Node &c, ClockRow &row, std::string &err) {
  for (auto it = c.begin(); it != c.end(); ++it) {
    const std::string key = it->first.IsScalar() ? it->first.Scalar() : std::string{};
    if (key != "name") {
      err = at_line(it->first, "clock fields must read 'name'");
      return false;
    }
  }
  const YAML::Node name = c["name"];
  if (!name || !name.IsDefined()) {
    err = at_line(c, "clock entry needs 1 field (name)");
    return false;
  }
  if (!name.IsScalar() || name.Scalar().empty()) {
    err = at_line(c, "empty clock name");
    return false;
  }
  row.name = name.Scalar();
  row.line = line_of(c);
  return true;
}

// 011: one `{net: PIN}` row per marked input. The mark is the opt-in;
// the attachment count derives from the repeated `wires:` rows.
[[nodiscard]] bool check_shared_map(const YAML::Node &n, SharedRow &row, std::string &err) {
  for (auto it = n.begin(); it != n.end(); ++it) {
    const std::string key = it->first.IsScalar() ? it->first.Scalar() : std::string{};
    if (key != "net") {
      err = at_line(it->first, "unknown shared key '" + key + "' (want 'net: ...')");
      return false;
    }
  }
  const YAML::Node net = n["net"];
  if (!net || !net.IsDefined()) {
    err = at_line(n, "shared entry needs 1 field (net)");
    return false;
  }
  if (!net.IsScalar() || net.Scalar().empty()) {
    err = at_line(n, "empty shared net");
    return false;
  }
  row.net = net.Scalar();
  row.line = line_of(n);
  return true;
}

// 012: one `{net: PIN, pull: up|down}` row per pulled input — the pull
// is the property; absence of a row (or of the section) is `none`.
[[nodiscard]] bool check_pull_map(const YAML::Node &n, PullRow &row, std::string &err) {
  for (auto it = n.begin(); it != n.end(); ++it) {
    const std::string key = it->first.IsScalar() ? it->first.Scalar() : std::string{};
    if (key != "net" && key != "pull") {
      err = at_line(it->first, "unknown pulls key '" + key + "' (want 'net: ..., pull: up|down')");
      return false;
    }
  }
  const YAML::Node net = n["net"];
  const YAML::Node pull = n["pull"];
  if (!net || !net.IsDefined() || !pull || !pull.IsDefined()) {
    err = at_line(n, "pulls entry needs 2 fields (net, pull)");
    return false;
  }
  if (!net.IsScalar() || net.Scalar().empty()) {
    err = at_line(n, "empty pull net");
    return false;
  }
  if (!pull.IsScalar() || (pull.Scalar() != "up" && pull.Scalar() != "down")) {
    err = at_line(n, "bad pull level (want up|down)");
    return false;
  }
  row.net = net.Scalar();
  row.level = (pull.Scalar() == "up") ? 1u : 2u;
  row.line = line_of(n);
  return true;
}

// --- Generic row-collection loop ---
//
// Every sequence section had the same block: reserve, iterate, reject
// non-map rows with the section shape string, run the checker, append.
// One template now serves all six sections; the table below supplies
// the per-section destination vector (via the thin collect_* wrappers)
// and the shape string.

template <typename Row, bool (*Check)(const YAML::Node &, Row &, std::string &)>
bool collect_typed(const YAML::Node &sec, std::vector<Row> &rows, const char *row_err, std::string &err) {
  rows.reserve(sec.size());
  for (std::size_t i = 0; i < sec.size(); ++i) {
    const YAML::Node n = sec[i];
    if (!n.IsMap()) {
      err = at_line(n, row_err);
      return false;
    }
    Row row{};
    if (!Check(n, row, err)) {
      return false;
    }
    rows.push_back(std::move(row));
  }
  return true;
}

namespace {

bool collect_gates(const YAML::Node &sec, Collected &out, const char *row_err, std::string &err) {
  return collect_typed<GateRow, check_gate_map>(sec, out.gates, row_err, err);
}

bool collect_wires(const YAML::Node &sec, Collected &out, const char *row_err, std::string &err) {
  return collect_typed<WireRow, check_wire_map>(sec, out.wires, row_err, err);
}

bool collect_shared(const YAML::Node &sec, Collected &out, const char *row_err, std::string &err) {
  return collect_typed<SharedRow, check_shared_map>(sec, out.shared, row_err, err);
}

bool collect_pulls(const YAML::Node &sec, Collected &out, const char *row_err, std::string &err) {
  return collect_typed<PullRow, check_pull_map>(sec, out.pulls, row_err, err);
}

bool collect_ties(const YAML::Node &sec, Collected &out, const char *row_err, std::string &err) {
  return collect_typed<TieRow, check_tie_map>(sec, out.ties, row_err, err);
}

bool collect_clocks(const YAML::Node &sec, Collected &out, const char *row_err, std::string &err) {
  return collect_typed<ClockRow, check_clocks_map>(sec, out.clocks, row_err, err);
}

} // namespace

// Canonical save order: version, gates, wires, shared, pulls, ties,
// clocks, meta. The root allowlist diagnostic derives from this order.
const SectionSpec kSections[6] = {
    {"gates", false, "want 'gates:' section", "'gates:' must read a sequence",
     "gate entry must read '{id: ..., kind: ..., inputs: ..., ignore: [...], ignore_output: ..., [ignore_nq: ...,] delay_ps: ...[, init: ...]}'",
     collect_gates, replay_gates},
    {"wires", false, "want 'wires:' section", "'wires:' must read a sequence",
     "wire entry must read '{from: ..., to: ...}'", collect_wires, replay_wires},
    {"shared", true, nullptr, "'shared:' must read a sequence", "shared entry must read '{net: ...}'", collect_shared,
     replay_shared},
    {"pulls", true, nullptr, "'pulls:' must read a sequence", "pulls entry must read '{net: ..., pull: up|down}'",
     collect_pulls, replay_pulls},
    {"ties", false, "legacy workspace version 1 is retired (missing 'ties:' section, want 1)",
     "'ties:' must read a sequence", "tie entry must read '{a: ..., b: ...}'", collect_ties, replay_ties},
    {"clocks", false, "legacy workspace version 1 is retired (missing 'clocks:' section, want 1)",
     "'clocks:' must read a sequence", "clock entry must read '{name: ...}'", collect_clocks, nullptr},
};

[[nodiscard]] const SectionSpec *section_by_name(std::string_view name) noexcept {
  for (const SectionSpec &s : kSections) {
    if (name == s.name) {
      return &s;
    }
  }
  return nullptr;
}

[[nodiscard]] std::string root_want_list() {
  std::string want = "version";
  for (const SectionSpec &s : kSections) {
    want += '|';
    want += s.name;
  }
  want += "|meta";
  return want;
}

} // namespace wengine::logic::yaml::detail
