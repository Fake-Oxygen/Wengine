#pragma once

// Pin + kind vocabulary Module (C2 deepening): one table per gate kind.
//
// Each row owns everything parsing needs for its kind — display name,
// default fan-in, exact allowed fan-in set (bitmask, so DFF's {2,5} stays
// exact instead of degrading to a range), and the named input pins with
// their per-pin minimum fan-in (DFF RST/SET/EN exist only on wider flops).
// `valid_fanin`, `default_fanin`, `parse_gate_kind`, `resolve_gate`, and
// `parse_pin` are all views over this table: adding a kind is one row, not
// five edits across three functions.
//
// Split (deliberate): `types.hpp::gate_kind_name` stays the display-only
// name (types.hpp is dependency-free; parse returns Result and cannot live
// there), and `kind_ordinal` stays beside the table for the counter slots.
// Keep the three name spellings in sync when adding a kind.
//
// Header-only, allocation-free, exception-free, noexcept throughout.
// Depends only on types.hpp + core/status, so netlist/yaml/CLI can all
// include it with no cycle.

#include "wengine/core/status.hpp"
#include "wengine/logic/types.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace wengine::logic {

[[nodiscard]] constexpr std::uint8_t kind_ordinal(GateKind k) noexcept {
  return static_cast<std::uint8_t>(k);
}

namespace detail {
[[nodiscard]] constexpr char to_upper(char c) noexcept {
  if (c >= 'a' && c <= 'z') {
    return static_cast<char>(c - ('a' - 'A'));
  }
  return c;
}

[[nodiscard]] constexpr bool starts_with_ci(std::string_view text, std::string_view prefix) noexcept {
  if (text.size() < prefix.size()) {
    return false;
  }
  for (std::size_t i = 0; i < prefix.size(); ++i) {
    if (to_upper(text[i]) != prefix[i]) {
      return false;
    }
  }
  return true;
}

// Strict unsigned decimal, no signs/spaces. Returns false on empty,
// non-digits, or overflow past limit.
[[nodiscard]] constexpr bool parse_decimal(std::string_view text, std::uint32_t limit, std::uint32_t &out) noexcept {
  if (text.empty()) {
    return false;
  }
  std::uint32_t v = 0;
  for (char c : text) {
    if (c < '0' || c > '9') {
      return false;
    }
    const auto d = static_cast<std::uint32_t>(c - '0');
    if (v > (limit - d) / 10) {
      return false;
    }
    v = v * 10 + d;
  }
  out = v;
  return true;
}
} // namespace detail

// Exact case-insensitive pin-name match ("clk" == "CLK", "q" == "Q").
// Empty name never matches; longer/shorter suffixes fall through to the
// numeric parser so `DFF1.1` keeps working alongside `DFF1.D`.
[[nodiscard]] constexpr bool match_pin_name(std::string_view suffix, std::string_view name) noexcept {
  if (name.empty() || suffix.size() != name.size()) {
    return false;
  }
  for (std::size_t i = 0; i < suffix.size(); ++i) {
    if (detail::to_upper(suffix[i]) != name[i]) {
      return false;
    }
  }
  return true;
}

// One named input pin: `name` resolves to 0-based `input` iff the gate was
// created with at least `min_inputs` inputs (DFF RST/SET/EN exist only on
// wider flops; bare 2-input flops reject them like out-of-range numerics).
struct KindPin {
  std::string_view name;
  std::uint8_t input = 0;
  std::uint8_t min_inputs = 0;
};

// One gate kind: parse name, default fan-in, exact allowed fan-in set
// (bit (n-1) set iff fan-in n valid, n = 1..8), and named input pins.
struct KindInfo {
  GateKind kind;
  std::string_view name;
  std::uint8_t default_inputs = 2;
  std::uint16_t allowed_mask = 0;
  std::uint8_t pin_count = 0;
  KindPin pins[5]{};
};

// Append-only: keep every known kind here (same order as the historical
// kKinds/kAllKinds) so the persisted byte values (DFF=16, DLATCH=17,
// JK=18, T=19, SR=20) never shift. Fan-in masks: combinational 2..8 =
// 0xFE; NOT/BUF exactly 1 = 0x01; DFF {2,5} = 0x12; DLATCH/T/SR exactly
// 2 = 0x02; JK exactly 3 = 0x04.
inline constexpr KindInfo kKinds[] = {
    {GateKind::kAnd, "AND", 2, 0xFEu, 0, {}},
    {GateKind::kOr, "OR", 2, 0xFEu, 0, {}},
    {GateKind::kNand, "NAND", 2, 0xFEu, 0, {}},
    {GateKind::kNor, "NOR", 2, 0xFEu, 0, {}},
    {GateKind::kXor, "XOR", 2, 0xFEu, 0, {}},
    {GateKind::kXnor, "XNOR", 2, 0xFEu, 0, {}},
    {GateKind::kNot, "NOT", 1, 0x01u, 0, {}},
    {GateKind::kBuf, "BUF", 1, 0x01u, 0, {}},
    {GateKind::kDff, "DFF", 2, 0x12u, 5,
     {{"D", 0, 2}, {"CLK", 1, 2}, {"RST", 2, 3}, {"SET", 3, 4}, {"EN", 4, 5}}},
    {GateKind::kDlatch, "DLATCH", 2, 0x02u, 2, {{"D", 0, 2}, {"EN", 1, 2}}},
    {GateKind::kJk, "JK", 3, 0x04u, 3, {{"J", 0, 3}, {"K", 1, 3}, {"CLK", 2, 3}}},
    {GateKind::kT, "T", 2, 0x02u, 2, {{"T", 0, 2}, {"CLK", 1, 2}}},
    {GateKind::kSrlatch, "SRLATCH", 2, 0x02u, 2, {{"S", 0, 2}, {"R", 1, 2}}},
};

[[nodiscard]] constexpr bool kind_table_valid() noexcept {
  std::size_t n = 0;
  for (const KindInfo &e : kKinds) {
    (void)e;
    ++n;
  }
  if (n != 13) {
    return false;
  }
  for (const KindInfo &e : kKinds) {
    if (e.name.empty() || e.allowed_mask == 0) {
      return false;
    }
    if (e.default_inputs == 0 || e.default_inputs > kMaxInputs) {
      return false;
    }
    if ((e.allowed_mask & static_cast<std::uint16_t>(1u << (e.default_inputs - 1u))) == 0) {
      return false; // default fan-in must itself be allowed
    }
    if (e.pin_count > 5) {
      return false;
    }
    for (std::uint8_t p = 0; p < e.pin_count; ++p) {
      if (e.pins[p].name.empty() || e.pins[p].input >= kMaxInputs) {
        return false;
      }
      if (e.pins[p].min_inputs <= e.pins[p].input) {
        return false; // min fan-in always exceeds the 0-based index
      }
    }
  }
  return true;
}

static_assert(kind_table_valid(), "logic kind table: default/mask/pin rows inconsistent");

[[nodiscard]] constexpr bool valid_fanin(GateKind k, std::uint8_t n) noexcept {
  if (n == 0 || n > kMaxInputs) {
    return false;
  }
  for (const KindInfo &e : kKinds) {
    if (e.kind == k) {
      return (e.allowed_mask & static_cast<std::uint16_t>(1u << (n - 1u))) != 0;
    }
  }
  return false;
}

[[nodiscard]] constexpr std::uint8_t default_fanin(GateKind k) noexcept {
  for (const KindInfo &e : kKinds) {
    if (e.kind == k) {
      return e.default_inputs;
    }
  }
  return 2;
}

// Case-insensitive ("and", "AND", "And" all match). Exact full match.
// Sequential kinds (DFF/DLATCH/JK/T/SRLATCH) parse here; creation validates
// fan-in via valid_fanin and state attaches via init_seq_state. Keep the
// want-list in sync with kKinds above.
[[nodiscard]] inline core::Result<GateKind> parse_gate_kind(std::string_view text) noexcept {
  if (text.empty()) {
    return core::Result<GateKind>::failure("bad gate kind: empty");
  }
  for (const KindInfo &e : kKinds) {
    if (text.size() != e.name.size()) {
      continue;
    }
    bool same = true;
    for (std::size_t j = 0; j < text.size(); ++j) {
      if (detail::to_upper(text[j]) != e.name[j]) {
        same = false;
        break;
      }
    }
    if (same) {
      return core::Result<GateKind>::success(e.kind);
    }
  }
  return core::Result<GateKind>::failure("bad gate kind: want AND|OR|NAND|NOR|XOR|XNOR|NOT|BUF|DFF|DLATCH|JK|T|SRLATCH");
}

// Find a live gate by display name (e.g. "AND1", "DFF1", case-insensitive
// kind). Linear slot-order scan: deterministic, first (only) match wins.
// Covers all sequential kinds (DFF/DLATCH/JK/T/SRLATCH); no Netlist shape change
// (ordinals compare by value).
[[nodiscard]] inline core::Result<std::uint16_t> resolve_gate(const Netlist &nl, std::string_view name) noexcept {
  if (name.empty()) {
    return core::Result<std::uint16_t>::failure("bad gate name: empty");
  }
  for (const KindInfo &e : kKinds) {
    const std::uint8_t o = kind_ordinal(e.kind);
    const std::string_view kind_name = e.name;
    if (!detail::starts_with_ci(name, kind_name)) {
      continue;
    }
    const std::string_view num = name.substr(kind_name.size());
    std::uint32_t counter = 0;
    if (!detail::parse_decimal(num, 0xFFFFFFFFu, counter) || counter == 0) {
      continue; // not this kind (or bad number): keep trying other kinds
    }
    for (std::uint16_t s = 0; s < kMaxGates; ++s) {
      const Gate &g = nl.gates[s];
      if (g.live && kind_ordinal(g.kind) == o && g.counter == counter) {
        return core::Result<std::uint16_t>::success(s);
      }
    }
    return core::Result<std::uint16_t>::failure("unknown gate");
  }
  return core::Result<std::uint16_t>::failure("bad gate name");
}

// Parse `<NAME>.Y` (output) or `<NAME>.<1..k>` (input, 1-based in text).
// Sequential gates additionally accept named pins (FR-010/FR-011,
// case-insensitive) from the kind table (DFF `D`/`CLK` plus `RST`/`SET`/`EN`
// on full-control flops only; DLATCH `D`/`EN`; JK `J`/`K`/`CLK`; T `T`/
// `CLK`; SRLATCH `S`/`R`; every flop also `Q`/`NQ`). Numeric aliases
// keep working for all kinds; names on combinational gates are rejected;
// kind-foreign names (e.g. `D` on a JK flop, `J` on a DFF) fall through
// to the numeric parser and fail there.
// Input range validates against the gate's configured fan-in.
[[nodiscard]] inline core::Result<PinRef> parse_pin(const Netlist &nl, std::string_view text) noexcept {
  std::size_t dot = text.find('.');
  if (dot == std::string_view::npos) {
    return core::Result<PinRef>::failure("bad pin: want NAME.Y|NAME.i");
  }
  if (text.find('.', dot + 1) != std::string_view::npos) {
    return core::Result<PinRef>::failure("bad pin: want NAME.Y|NAME.i");
  }
  const std::string_view gate_text = text.substr(0, dot);
  const std::string_view suffix = text.substr(dot + 1);
  if (gate_text.empty() || suffix.empty()) {
    return core::Result<PinRef>::failure("bad pin: want NAME.Y|NAME.i");
  }
  auto gate = resolve_gate(nl, gate_text);
  if (!gate.ok()) {
    if (gate.error == std::string_view("unknown gate")) {
      return core::Result<PinRef>::failure("unknown gate");
    }
    return core::Result<PinRef>::failure("bad pin: bad gate name");
  }
  const std::uint16_t slot = gate.value;
  const Gate &g = nl.gates[slot];
  // Named sequential pins (FR-010): checked before the generic Y/numeric
  // forms; numeric aliases fall through below and keep working.
  if (is_sequential_kind(g.kind)) {
    for (const KindInfo &e : kKinds) {
      if (e.kind != g.kind) {
        continue;
      }
      for (std::uint8_t p = 0; p < e.pin_count; ++p) {
        if (g.input_count >= e.pins[p].min_inputs && match_pin_name(suffix, e.pins[p].name)) {
          PinRef ref{};
          ref.slot = slot;
          ref.is_output = false;
          ref.input = e.pins[p].input;
          return core::Result<PinRef>::success(ref);
        }
      }
      break;
    }
    if (match_pin_name(suffix, "Q")) {
      PinRef ref{};
      ref.slot = slot;
      ref.is_output = true;
      return core::Result<PinRef>::success(ref);
    }
    if (match_pin_name(suffix, "NQ")) {
      PinRef ref{};
      ref.slot = slot;
      ref.is_output = true;
      ref.negated = true;
      return core::Result<PinRef>::success(ref);
    }
  }
  if (suffix.size() == 1 && (suffix[0] == 'Y' || suffix[0] == 'y')) {
    PinRef ref{};
    ref.slot = slot;
    ref.is_output = true;
    return core::Result<PinRef>::success(ref);
  }
  std::uint32_t idx = 0;
  if (!detail::parse_decimal(suffix, 0xFFFFu, idx) || idx == 0) {
    return core::Result<PinRef>::failure("bad pin: want NAME.Y|NAME.i");
  }
  if (idx > g.input_count) {
    return core::Result<PinRef>::failure("input index out of range");
  }
  PinRef ref{};
  ref.slot = slot;
  ref.is_output = false;
  ref.input = static_cast<std::uint8_t>(idx - 1);
  return core::Result<PinRef>::success(ref);
}

} // namespace wengine::logic
