#include "common.hpp"

#include "wengine/logic/clock_domains.hpp"
#include "wengine/logic/eval.hpp"
#include "wengine/logic/seq_step.hpp"
#include "wengine/logic/types.hpp"
#include "wengine/logic/yaml_store.hpp"

#include <array>
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

namespace {

[[nodiscard]] char bit_of(wengine::logic::LogicValue v) noexcept {
  return (v == wengine::logic::LogicValue::kHigh) ? '1' : '0';
}

} // namespace

[[nodiscard]] std::string pin_text(const Netlist &nl, std::uint16_t slot, bool is_output, std::uint8_t input, bool negated) {
  std::string s = gate_name(nl, slot);
  s += '.';
  const auto &g = nl.gates[slot];
  if (is_output && wengine::logic::is_sequential_kind(g.kind)) {
    s += negated ? "NQ" : "Q";
  } else if (is_output) {
    s += 'Y';
  } else if (wengine::logic::is_sequential_kind(g.kind)) {
    // Canonical named inputs: DFF D/CLK (+RST/SET/EN on full-control
    // flops), DLATCH D/EN, JK J/K/CLK, T T/CLK, SR S/R (004-sr-d-latches).
    if (g.kind == GateKind::kJk) {
      s += (input == 0) ? "J" : (input == 1) ? "K" : "CLK";
    } else if (g.kind == GateKind::kT) {
      s += (input == 0) ? "T" : "CLK";
    } else if (g.kind == GateKind::kSrlatch) {
      s += (input == 0) ? "S" : "R";
    } else if (input == 0) {
      s += 'D';
    } else if (g.kind == GateKind::kDff && input >= 2) {
      // Full control set (5-input flops): canonical named forms.
      s += (input == 2) ? "RST" : (input == 3) ? "SET" : "EN";
    } else {
      s += (g.kind == GateKind::kDff) ? "CLK" : "EN";
    }
  } else {
    s += std::to_string(static_cast<unsigned>(input) + 1u);
  }
  return s;
}

} // namespace support

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
      if (wengine::logic::is_sequential_kind(g.kind)) {
        std::string nq = pin_text(nl, s, true, 0, true);
        if (nq.size() >= partial.size() && nq.substr(0, partial.size()) == partial) {
          out.push_back(nq);
        }
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

std::vector<ClockDomain> clock_domains(const Netlist &nl) {
  // Single engine-owned derivation (006): the file pins these same names,
  // so session stepping must use exactly the loader's vocabulary.
  std::vector<ClockDomain> domains;
  for (const wengine::logic::ClockDomain &d : wengine::logic::derive_clock_domains(nl)) {
    ClockDomain out;
    out.name = d.name;
    out.members = d.members;
    domains.push_back(std::move(out));
  }
  return domains;
}

core::Status compose_block_inputs(const Netlist &nl, std::vector<wengine::logic::LogicValue> &out) {
  out.clear();
  const auto &pokes = poke_map();
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
      const std::size_t flat = static_cast<std::size_t>(s) * kMaxInputs + static_cast<std::size_t>(i);
      if (wengine::logic::tie_leader_flat(nl, flat) != flat) {
        continue; // members share the leader's value (same order as solve)
      }
      const auto it = pokes.find(flat);
      if (it == pokes.end() && g.kind == wengine::logic::GateKind::kDff && i == 4 && nl.tie_rep[flat] == 0) {
        // FR-008 (see R8): untied, unpoked enable reads asserted. Tied EN
        // follows its tie net via the generic rule below; ignored EN is
        // skipped above and reads asserted via resolve_control.
        out.push_back(wengine::logic::LogicValue::kHigh);
        continue;
      }
      out.push_back(it == pokes.end() ? wengine::logic::LogicValue::kLow : it->second);
    }
  }
  return core::Status::success();
}

std::size_t reap_stale_pokes(const Netlist &nl) {
  auto &pokes = poke_map();
  std::size_t dropped = 0;
  for (auto it = pokes.begin(); it != pokes.end();) {
    const std::size_t flat = it->first;
    const auto slot = static_cast<std::uint16_t>(flat / kMaxInputs);
    const auto input = static_cast<std::uint8_t>(flat % kMaxInputs);
    bool live = slot < kMaxGates && nl.gates[slot].live && input < nl.gates[slot].input_count &&
                nl.gates[slot].drivers[input] == kNoDriver &&
                (nl.gates[slot].ignored_mask & static_cast<std::uint8_t>(1u << input)) == 0;
    if (live) {
      ++it;
    } else {
      it = pokes.erase(it);
      ++dropped;
    }
  }
  return dropped;
}

core::Status apply_async_and_report(Netlist &nl, std::ostream &out) {
  wengine::logic::SeqState &st = seq_state();
  wengine::logic::init_seq_state(st, nl);
  std::array<wengine::logic::LogicValue, kMaxGates> old_q{};
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    if (nl.gates[s].live && wengine::logic::is_sequential_kind(nl.gates[s].kind)) {
      old_q[s] = st.q[s];
    }
  }
  std::vector<wengine::logic::LogicValue> inputs;
  if (!compose_block_inputs(nl, inputs).ok()) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  // Snapshot the pool: the async apply commits before the level apply
  // runs, so a level refusal (S=R=1) must roll back async forces too —
  // callers additionally revert their own topology/map mutation.
  const wengine::logic::SeqState committed = st;
  if (!wengine::logic::apply_async_controls(nl, st, std::span<const wengine::logic::LogicValue>(inputs.data(), inputs.size())).ok()) {
    st = committed;
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  // Level latches settle at once (transparent D follows D while EN is
  // active, frozen latches hold); same report shape as async forces below.
  if (!wengine::logic::apply_level_latches(nl, st, std::span<const wengine::logic::LogicValue>(inputs.data(), inputs.size())).ok()) {
    st = committed;
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    if (!nl.gates[s].live || !wengine::logic::is_sequential_kind(nl.gates[s].kind)) {
      continue;
    }
    if (st.q[s] == old_q[s]) {
      continue;
    }
    const wengine::logic::LogicValue before = old_q[s];
    const wengine::logic::LogicValue now = st.q[s];
    out << pin_text(nl, s, true, 0) << ": " << bit_of(before) << "->" << bit_of(now) << " (NQ "
        << bit_of(before == wengine::logic::LogicValue::kHigh ? wengine::logic::LogicValue::kLow
                                                              : wengine::logic::LogicValue::kHigh)
        << "->"
        << bit_of(now == wengine::logic::LogicValue::kHigh ? wengine::logic::LogicValue::kLow
                                                           : wengine::logic::LogicValue::kHigh)
        << ")\n";
  }
  return core::Status::success();
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
    os << pin_text(nl, p.slot, true, 0, p.negated);
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

core::Status write_truth_pretty(Netlist &nl, std::uint64_t rows, std::uint64_t total_rows,
                                const std::vector<BlockPin> &in_pins, const std::vector<BlockPin> &out_pins,
                                std::ostream &os) {
  const std::size_t nin = in_pins.size();
  const std::size_t nout = out_pins.size();
  const std::size_t ncols = nin + nout;
  if (ncols == 0) {
    os << "(empty block: no interface pins, 1 row)\n";
    if (!os) {
      return core::Status::failure(core::StatusCode::kInternal);
    }
    return core::Status::success();
  }
  std::vector<std::string> headers;
  headers.reserve(ncols);
  for (const BlockPin &p : in_pins) {
    headers.push_back(pin_text(nl, p.slot, false, p.input));
  }
  for (const BlockPin &p : out_pins) {
    headers.push_back(pin_text(nl, p.slot, true, 0, p.negated));
  }
  std::vector<std::size_t> widths;
  widths.reserve(ncols);
  for (const std::string &h : headers) {
    widths.push_back(h.size());
  }
  for (std::size_t i = 0; i < ncols; ++i) {
    if (i != 0) {
      os << " | ";
    }
    os << headers[i];
  }
  os << '\n';
  for (std::size_t i = 0; i < ncols; ++i) {
    if (i != 0) {
      os << "-+-";
    }
    os << std::string(widths[i], '-');
  }
  os << '\n';
  if (!os) {
    return core::Status::failure(core::StatusCode::kInternal);
  }
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
      for (std::size_t c = 0; c < ncols; ++c) {
        if (c != 0) {
          os << " | ";
        }
        const char bit = (c < nin) ? ((((row >> c) & 1ULL) != 0ULL) ? '1' : '0')
                                   : ((((words[c - nin] >> b) & 1ULL) != 0ULL) ? '1' : '0');
        const std::size_t width = widths[c];
        const std::size_t left = (width - 1) / 2;
        const std::size_t right = width - 1 - left;
        os << std::string(left, ' ') << bit << std::string(right, ' ');
      }
      os << '\n';
      if (!os) {
        return core::Status::failure(core::StatusCode::kInternal);
      }
    }
  }
  if (rows < total_rows) {
    os << '(' << rows << " of " << total_rows << " rows shown; use --limit N or --csv <path> for more)\n";
    if (!os) {
      return core::Status::failure(core::StatusCode::kInternal);
    }
  }
  return core::Status::success();
}

void save_yaml(std::ostream &os, const Netlist &nl) {
  // Canonical bytes live in the versioned store (src/logic/yaml_store.cpp,
  // the single TU allowed to know yaml-cpp exists); this CLI wrapper
  // resyncs the pool and streams live Q as the file's per-element power-on
  // values (006: save captures live state), plus the derived clock
  // domains. Byte-identical to the v1 layout by construction.
  wengine::logic::SeqState &st = seq_state();
  wengine::logic::init_seq_state(st, nl);
  std::array<wengine::logic::LogicValue, wengine::logic::kMaxGates> inits{};
  inits.fill(wengine::logic::LogicValue::kLow);
  for (std::uint16_t s = 0; s < wengine::logic::kMaxGates; ++s) {
    if (st.valid[s] != 0) {
      inits[s] = st.q[s];
    }
  }
  std::string text;
  wengine::logic::yaml::save_text(nl, inits, text);
  os << text;
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

bool load_text(const std::string &text, Netlist &out,
               std::array<wengine::logic::LogicValue, wengine::logic::kMaxGates> &inits,
               std::string &err) {
  // Strict v1 loader lives in the versioned store
  // (src/logic/yaml_store.cpp, the single TU allowed to know yaml-cpp
  // exists); this CLI wrapper forwards the bundle. Atomic: `out` and
  // `inits` are untouched on failure, so `load` keeps the workspace on
  // error.
  wengine::logic::yaml::Workspace ws;
  if (!wengine::logic::yaml::load_text(text, ws, err)) {
    return false;
  }
  out = ws.netlist;
  inits = ws.init;
  return true;
}

} // namespace yaml

[[nodiscard]] bool has_bmp_suffix(std::string_view path) noexcept {
  if (path.size() < 4) {
    return false;
  }
  const std::string_view suf = path.substr(path.size() - 4);
  return suf == ".bmp" || suf == ".BMP";
}

} // namespace support

} // namespace wengine::cli::logic
