#include "common.hpp"

#include "wengine/cli/status.hpp"
#include "wengine/core/status.hpp"
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

using wengine::logic::PinRef;
using wengine::logic::GateKind;
using wengine::logic::kMaxInputs;
using wengine::logic::kNoDriver;
using wengine::logic::Netlist;
using wengine::logic::Slot;
using wengine::logic::slot_count;

namespace support {

bool handle_help(ArgCursor &args, std::ostream &out, std::string_view name, std::string_view description, std::string_view usage) {
  if (!args.take_flag({"--help", "-h"})) {
    return false;
  }
  out << name << " - " << description << "\nUsage:\n" << usage << '\n';
  return true;
}

bool complete_opts(std::string_view partial, std::span<const std::string_view> opts, std::vector<std::string> &out) {
  if (partial.empty() || partial.front() != '-') {
    return false;
  }
  push_matches(partial, opts, out);
  return true;
}

bool take_positionals(ArgCursor &args, std::ostream &out, std::string_view usage, std::vector<std::string_view> &out_pos, std::size_t min, std::size_t max,
                      std::string_view missing_msg, bool include_options) {
  out_pos.clear();
  for (std::size_t i = 0; i < max; ++i) {
    std::optional<std::string_view> p = args.next_positional(include_options);
    if (!p) {
      break;
    }
    out_pos.push_back(*p);
  }
  if (out_pos.size() < min) {
    cli::usage_error(out, missing_msg, usage);
    return false;
  }
  if (std::optional<std::string_view> extra = args.next_positional(include_options)) {
    if (min == max) {
      cli::unexpected_error(out, *extra, usage);
    } else {
      cli::usage_error(out, missing_msg, usage);
    }
    return false;
  }
  if (args.has_remaining()) {
    const std::optional<std::string_view> stray = args.first_remaining();
    cli::unexpected_error(out, stray ? *stray : std::string_view{}, usage);
    return false;
  }
  return true;
}

bool parse_input_pin(const Netlist &nl, std::string_view text, PinRef &pin_out, std::ostream &out, std::string_view usage, std::string_view output_msg) {
  const auto pin = wengine::logic::parse_pin(nl, text);
  if (!pin.ok()) {
    cli::usage_error(out, std::string("bad pin '") + std::string(text) + "' (want NAME.i)", usage);
    return false;
  }
  if (pin.value.is_output) {
    cli::usage_error(out, output_msg, usage);
    return false;
  }
  pin_out = pin.value;
  return true;
}

[[nodiscard]] char bit_of(wengine::logic::LogicValue v) noexcept {
  // (012) Z renders explicitly: combinational block outputs can float
  // via released buffers; sequential Q values stay Low/High, so those
  // lines print byte-identically to the old per-TU copies.
  return (v == wengine::logic::LogicValue::kHigh) ? '1' : ((v == wengine::logic::LogicValue::kLow) ? '0' : 'Z');
}

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

[[nodiscard]] std::string gate_name(const Netlist &nl, Slot slot) {
  std::string s(wengine::logic::gate_kind_name(nl.slots[slot].kind));
  s += std::to_string(nl.slots[slot].counter);
  return s;
}

[[nodiscard]] std::string pin_text(const Netlist &nl, Slot slot, bool is_output, std::uint8_t input, bool negated) {
  std::string s = gate_name(nl, slot);
  s += '.';
  const auto &g = nl.slots[slot];
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
  for (Slot s = 0; s < slot_count(nl); ++s) {
    if (!nl.slots[s].live) {
      continue;
    }
    std::string name = gate_name(nl, s);
    if (name.size() >= partial.size() && name.substr(0, partial.size()) == partial) {
      out.push_back(name);
    }
  }
}

void collect_pins(const Netlist &nl, bool want_out, bool want_in, std::string_view partial, std::vector<std::string> &out) {
  for (Slot s = 0; s < slot_count(nl); ++s) {
    const auto &g = nl.slots[s];
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
  for (Slot s = 0; s < slot_count(nl); ++s) {
    const auto &g = nl.slots[s];
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

void interface_pins(const Netlist &nl, std::vector<PinRef> &in_pins, std::vector<PinRef> &out_pins) {
  const std::size_t nin = wengine::logic::block_input_count(nl);
  const std::size_t nout = wengine::logic::block_output_count(nl);
  in_pins.assign(nin, PinRef{});
  out_pins.assign(nout, PinRef{});
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
  // Block order via the BlockInterface seam (one entry per tie leader);
  // poke lookup by leader flat, FR-008 untied/unpoked EN defaults High.
  out.clear();
  const auto &pokes = poke_map();
  const std::size_t n = wengine::logic::block_input_count(nl);
  for (std::size_t ord = 0; ord < n; ++ord) {
    Slot s = 0;
    std::uint8_t i = 0;
    if (!wengine::logic::block_input_pin(nl, ord, s, i)) {
      return core::Status::failure(core::StatusCode::kInternal);
    }
    const auto &g = nl.slots[s];
    const std::size_t flat = wengine::logic::tie_flat(s, i);
    const auto it = pokes.find(flat);
    if (it == pokes.end() && g.kind == wengine::logic::GateKind::kDff && i == 4 && !wengine::logic::is_tied(nl, s, i)) {
      // FR-008: untied, unpoked enable reads asserted. Tied EN follows its
      // tie net via the generic rule below; ignored EN is skipped by the
      // seam and reads asserted via resolve_control.
      out.push_back(wengine::logic::LogicValue::kHigh);
      continue;
    }
    out.push_back(it == pokes.end() ? wengine::logic::LogicValue::kLow : it->second);
  }
  return core::Status::success();
}

std::size_t reap_stale_pokes(const Netlist &nl) {
  auto &pokes = poke_map();
  std::size_t dropped = 0;
  for (auto it = pokes.begin(); it != pokes.end();) {
    const std::size_t flat = it->first;
    const Slot slot = static_cast<Slot>(flat / kMaxInputs);
    const auto input = static_cast<std::uint8_t>(flat % kMaxInputs);
    bool live = slot < slot_count(nl) && nl.slots[slot].live && input < nl.slots[slot].input_count &&
                nl.slots[slot].drivers[input] == kNoDriver &&
                (nl.slots[slot].ignored_mask & static_cast<std::uint8_t>(1u << input)) == 0;
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
  std::vector<wengine::logic::LogicValue> old_q(slot_count(nl), wengine::logic::LogicValue::kLow);
  for (Slot s = 0; s < slot_count(nl); ++s) {
    if (nl.slots[s].live && wengine::logic::is_sequential_kind(nl.slots[s].kind)) {
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
  for (Slot s = 0; s < slot_count(nl); ++s) {
    if (!nl.slots[s].live || !wengine::logic::is_sequential_kind(nl.slots[s].kind)) {
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

core::Status write_truth_csv(Netlist &nl, std::uint64_t rows, const std::vector<PinRef> &in_pins, const std::vector<PinRef> &out_pins, std::ostream &os, const std::uint64_t *settle_ps) {
  // (011) Word-loop entry guard (see eval_block_word): four-value nets
  // refuse loudly once, never per word.
  if (wengine::logic::shared_word_blocked(nl)) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  // (012) Word-loop entry guard: live buffers drive Z (no 1-bit lane);
  // pull-only netlists stream unchanged.
  if (wengine::logic::has_buffer_gates(nl)) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  // Header: input pins then output pins.
  bool first = true;
  for (const PinRef &p : in_pins) {
    if (!first) {
      os << ',';
    }
    first = false;
    os << pin_text(nl, p.slot, false, p.input);
  }
  for (const PinRef &p : out_pins) {
    if (!first) {
      os << ',';
    }
    first = false;
    os << pin_text(nl, p.slot, true, 0, p.negated);
  }
  const bool want_settle = (settle_ps != nullptr) && (rows != 0) && (!in_pins.empty() || !out_pins.empty());
  if (want_settle) {
    if (!first) {
      os << ',';
    }
    os << "settle_ps";
  }
  os << '\n';
  if (!os) {
    return core::Status::failure(core::StatusCode::kInternal);
  }
  const std::size_t nout = out_pins.size();
  // (017) Slice-blocked word eval: full batches share one traversal,
  // the tail stays on the single-slice path. Output bytes unchanged.
  constexpr std::size_t kBW = wengine::logic::kWordBatchWidth;
  const std::uint64_t nwords = (rows + 63ULL) / 64ULL;
  const std::uint64_t nbatches = nwords / static_cast<std::uint64_t>(kBW);
  std::vector<std::uint64_t> scratch(slot_count(nl) * kBW, 0);
  std::vector<std::uint64_t> words(nout == 0 ? kBW : nout * kBW, 0);
  const std::span<std::uint64_t> scratch_span{scratch.data(), scratch.size()};
  for (std::uint64_t b = 0; b < nbatches; ++b) {
    const std::uint64_t base = b * static_cast<std::uint64_t>(kBW);
    std::span<std::uint64_t> out_span = nout == 0 ? std::span<std::uint64_t>{} : std::span<std::uint64_t>{words.data(), nout * kBW};
    const core::Status st = wengine::logic::eval_block_words(nl, base, scratch_span, out_span);
    if (!st.ok()) {
      return st;
    }
    for (std::size_t w = 0; w < kBW; ++w) {
      for (std::uint64_t bl = 0; bl < 64; ++bl) {
        const std::uint64_t row = (base + static_cast<std::uint64_t>(w)) * 64ULL + bl;
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
          os << ((((words[o * kBW + w] >> bl) & 1ULL) != 0ULL) ? '1' : '0');
        }
        if (want_settle) {
          os << ',' << settle_ps[row];
        }
        os << '\n';
        if (!os) {
          return core::Status::failure(core::StatusCode::kInternal);
        }
      }
    }
  }
  for (std::uint64_t w = nbatches * static_cast<std::uint64_t>(kBW); w < nwords; ++w) {
    std::span<std::uint64_t> out_span = nout == 0 ? std::span<std::uint64_t>{} : std::span<std::uint64_t>{words.data(), nout};
    const core::Status st = wengine::logic::eval_block_word(nl, w, scratch_span, out_span);
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
      if (want_settle) {
        os << ',' << settle_ps[row];
      }
      os << '\n';
      if (!os) {
        return core::Status::failure(core::StatusCode::kInternal);
      }
    }
  }
  return core::Status::success();
}

core::Status write_truth_pretty(Netlist &nl, std::uint64_t rows, std::uint64_t total_rows, const std::vector<PinRef> &in_pins, const std::vector<PinRef> &out_pins,
                                std::ostream &os, const std::uint64_t *settle_ps) {
  // (011) Word-loop entry guard (see eval_block_word): four-value nets
  // refuse loudly once, never per word.
  if (wengine::logic::shared_word_blocked(nl)) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  // (012) Word-loop entry guard: live buffers drive Z (no 1-bit lane);
  // pull-only netlists stream unchanged.
  if (wengine::logic::has_buffer_gates(nl)) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
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
  for (const PinRef &p : in_pins) {
    headers.push_back(pin_text(nl, p.slot, false, p.input));
  }
  for (const PinRef &p : out_pins) {
    headers.push_back(pin_text(nl, p.slot, true, 0, p.negated));
  }
  // (014) Optional settle column: right-aligned whole-picosecond
  // drain times, width fit to the widest value (header included).
  const bool want_settle = (settle_ps != nullptr);
  std::size_t settle_width = 6; // "settle"
  if (want_settle) {
    for (std::uint64_t r = 0; r < rows; ++r) {
      std::size_t digits = 1;
      for (std::uint64_t v = settle_ps[r]; v >= 10; v /= 10) {
        ++digits;
      }
      if (digits > settle_width) {
        settle_width = digits;
      }
    }
    headers.emplace_back("settle");
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
  if (want_settle) {
    os << " | " << headers[ncols];
  }
  os << '\n';
  for (std::size_t i = 0; i < ncols; ++i) {
    if (i != 0) {
      os << "-+-";
    }
    os << std::string(widths[i], '-');
  }
  if (want_settle) {
    os << "-+-" << std::string(settle_width, '-');
  }
  os << '\n';
  if (!os) {
    return core::Status::failure(core::StatusCode::kInternal);
  }
  // (017) Slice-blocked word eval: full batches share one traversal,
  // the tail stays on the single-slice path. Output bytes unchanged.
  constexpr std::size_t kBW = wengine::logic::kWordBatchWidth;
  const std::uint64_t nwords = (rows + 63ULL) / 64ULL;
  const std::uint64_t nbatches = nwords / static_cast<std::uint64_t>(kBW);
  std::vector<std::uint64_t> scratch(slot_count(nl) * kBW, 0);
  std::vector<std::uint64_t> words(nout == 0 ? kBW : nout * kBW, 0);
  const std::span<std::uint64_t> scratch_span{scratch.data(), scratch.size()};
  for (std::uint64_t b = 0; b < nbatches; ++b) {
    const std::uint64_t base = b * static_cast<std::uint64_t>(kBW);
    std::span<std::uint64_t> out_span = nout == 0 ? std::span<std::uint64_t>{} : std::span<std::uint64_t>{words.data(), nout * kBW};
    const core::Status st = wengine::logic::eval_block_words(nl, base, scratch_span, out_span);
    if (!st.ok()) {
      return st;
    }
    for (std::size_t w = 0; w < kBW; ++w) {
      for (std::uint64_t bl = 0; bl < 64; ++bl) {
        const std::uint64_t row = (base + static_cast<std::uint64_t>(w)) * 64ULL + bl;
        if (row >= rows) {
          break;
        }
        for (std::size_t c = 0; c < ncols; ++c) {
          if (c != 0) {
            os << " | ";
          }
          const char bit = (c < nin) ? ((((row >> c) & 1ULL) != 0ULL) ? '1' : '0')
                                     : ((((words[(c - nin) * kBW + w] >> bl) & 1ULL) != 0ULL) ? '1' : '0');
          const std::size_t width = widths[c];
          const std::size_t left = (width - 1) / 2;
          const std::size_t right = width - 1 - left;
          os << std::string(left, ' ') << bit << std::string(right, ' ');
        }
        if (want_settle) {
          std::uint64_t v = settle_ps[row];
          std::size_t digits = 1;
          for (std::uint64_t t = v; t >= 10; t /= 10) {
            ++digits;
          }
          os << " | " << std::string(settle_width - digits, ' ') << v;
        }
        os << '\n';
        if (!os) {
          return core::Status::failure(core::StatusCode::kInternal);
        }
      }
    }
  }
  for (std::uint64_t w = nbatches * static_cast<std::uint64_t>(kBW); w < nwords; ++w) {
    std::span<std::uint64_t> out_span = nout == 0 ? std::span<std::uint64_t>{} : std::span<std::uint64_t>{words.data(), nout};
    const core::Status st = wengine::logic::eval_block_word(nl, w, scratch_span, out_span);
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
      if (want_settle) {
        std::uint64_t v = settle_ps[row];
        std::size_t digits = 1;
        for (std::uint64_t t = v; t >= 10; t /= 10) {
          ++digits;
        }
        os << " | " << std::string(settle_width - digits, ' ') << v;
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
  std::vector<wengine::logic::LogicValue> inits(slot_count(nl), wengine::logic::LogicValue::kLow);
  for (Slot s = 0; s < slot_count(nl); ++s) {
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
  for (Slot s = 0; s < slot_count(nl); ++s) {
    const auto &g = nl.slots[s];
    if (!g.live) {
      continue;
    }
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      if (g.drivers[i] != kNoDriver) {
        ++n;
      }
      // Shared extras are wires too (011); zero on unmarked netlists.
      n += wengine::logic::shared_extra_count(nl, s, i);
    }
  }
  return n;
}

void report_contention(const Netlist &nl, const wengine::logic::SolveDiag &diag, std::ostream &out) {
  bool any_mark = false;
  for (Slot s = 0; s < slot_count(nl) && !any_mark; ++s) {
    if (!nl.slots[s].live) {
      continue;
    }
    for (std::uint8_t i = 0; i < nl.slots[s].input_count; ++i) {
      if (wengine::logic::is_shared_input(nl, s, i)) {
        any_mark = true;
        break;
      }
    }
  }
  if (!any_mark) {
    return; // mark-free output stays byte-identical
  }
  std::uint64_t total = 0;
  out << "contention:";
  for (Slot s = 0; s < slot_count(nl); ++s) {
    if (!nl.slots[s].live) {
      continue;
    }
    for (std::uint8_t i = 0; i < nl.slots[s].input_count; ++i) {
      if (!wengine::logic::is_shared_input(nl, s, i)) {
        continue;
      }
      // A diag smaller than the pool (e.g. the no-op step path, where no
      // resolution ran) reads as zero — counts only exist for spans the
      // funnel actually folded.
      const std::size_t flat = static_cast<std::size_t>(s) * kMaxInputs + i;
      const std::uint32_t n = (flat < diag.per_net.size()) ? diag.per_net[flat] : 0;
      total += n;
      out << ' ' << pin_text(nl, s, false, i) << '=' << n;
    }
  }
  out << " (total " << total << ")\n";
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

bool load_text(const std::string &text, Netlist &out, std::vector<wengine::logic::LogicValue> &inits, std::string &err) {
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

void build_block_seeds(const std::vector<PinRef> &in_pins, std::span<const wengine::logic::LogicValue> values,
                       std::vector<wengine::logic::TimingSeed> &seeds) {
  seeds.clear();
  const std::size_t n = in_pins.size() < values.size() ? in_pins.size() : values.size();
  for (std::size_t j = 0; j < n; ++j) {
    if (values[j] == wengine::logic::LogicValue::kHigh) {
      seeds.push_back(wengine::logic::TimingSeed{.slot = in_pins[j].slot,
                                                 .input = in_pins[j].input,
                                                 .is_flop = false,
                                                 .fire_ps = 0,
                                                 .new_value = wengine::logic::LogicValue::kHigh});
    }
  }
}

[[nodiscard]] std::string timing_window_refused(std::string_view verb) {
  return std::string(verb) + ": timing window refused (event budget " + std::to_string(wengine::logic::kMaxTimingEvents) +
         " exceeded or picosecond time overflow)";
}

[[nodiscard]] char timing_value_char(wengine::logic::LogicValue v) noexcept {
  if (v == wengine::logic::LogicValue::kHigh) {
    return '1';
  }
  if (v == wengine::logic::LogicValue::kLow) {
    return '0';
  }
  if (v == wengine::logic::LogicValue::kHighZ) {
    return 'Z';
  }
  return 'X';
}

void print_timing_trace(const Netlist &nl, const wengine::logic::TimingTrace &trace, std::ostream &out, std::string_view edge) {
  out << "timing";
  if (!edge.empty()) {
    out << " (" << edge << ')';
  }
  out << ": unit " << trace.unit_ps << "ps, " << trace.n_annotated << " annotated\n";
  for (std::size_t i = 0; i < trace.n_events; ++i) {
    const wengine::logic::TimingEvent &ev = trace.events[i];
    std::string pin;
    if (ev.is_input) {
      pin = pin_text(nl, ev.slot, false, ev.input);
    } else {
      pin = pin_text(nl, ev.slot, true, 0, ev.nq);
    }
    out << "  t=" << ev.t_ps << "ps " << pin << ": " << timing_value_char(ev.old_value) << "->" << timing_value_char(ev.new_value) << '\n';
  }
  out << "settled in " << trace.drain_ps << "ps\n";
}

} // namespace support

} // namespace wengine::cli::logic
