#pragma once

// Logic netlist mutation + pins + derived block interface (LOGIC-2).
//
// Header-only, allocation-free, exception-free. Fallible ops use
// core::Status/Result (literals only, no heap). Determinism: free-slot
// search and interface derivation both scan in slot order; display ids
// are per-kind monotonic counters never reused (slots recycle).
//
// Ignored-pin rule (v1): `ignore` excludes a dangling pin from the
// derived block interface. Ignored dangling *inputs* tie off to kLow
// during eval (documented); wired inputs ignore the mark (the wire
// wins). `use` clears the mark. Both are idempotent.

#include "wengine/core/status.hpp"
#include "wengine/logic/types.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace wengine::logic {

struct PinRef {
  std::uint16_t slot = 0;
  bool is_output = false;
  std::uint8_t input = 0; // 0-based, valid iff !is_output
};

struct BlockPin {
  std::uint16_t slot = 0;
  std::uint8_t input = 0; // valid iff !is_output
  bool is_output = false;
};

[[nodiscard]] constexpr std::uint8_t kind_ordinal(GateKind k) noexcept {
  return static_cast<std::uint8_t>(k);
}

[[nodiscard]] constexpr bool valid_fanin(GateKind k, std::uint8_t n) noexcept {
  if (k == GateKind::kNot || k == GateKind::kBuf) {
    return n == 1;
  }
  return n >= 2 && n <= kMaxInputs;
}

[[nodiscard]] constexpr std::uint8_t default_fanin(GateKind k) noexcept {
  if (k == GateKind::kNot || k == GateKind::kBuf) {
    return 1;
  }
  return 2;
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

// Case-insensitive ("and", "AND", "And" all match). Exact full match.
[[nodiscard]] inline core::Result<GateKind> parse_gate_kind(std::string_view text) noexcept {
  if (text.empty()) {
    return core::Result<GateKind>::failure("bad gate kind: empty");
  }
  // Upper-compare against each known name.
  constexpr std::string_view kNames[8] = {"AND", "OR", "NAND", "NOR", "XOR", "XNOR", "NOT", "BUF"};
  for (std::uint8_t i = 0; i < 8; ++i) {
    if (text.size() != kNames[i].size()) {
      continue;
    }
    bool same = true;
    for (std::size_t j = 0; j < text.size(); ++j) {
      if (detail::to_upper(text[j]) != kNames[i][j]) {
        same = false;
        break;
      }
    }
    if (same) {
      return core::Result<GateKind>::success(static_cast<GateKind>(i));
    }
  }
  return core::Result<GateKind>::failure("bad gate kind: want AND|OR|NAND|NOR|XOR|XNOR|NOT|BUF");
}

// Find a live gate by display name (e.g. "AND1", case-insensitive kind).
// Linear slot-order scan: deterministic, first (only) match wins.
[[nodiscard]] inline core::Result<std::uint16_t> resolve_gate(const Netlist &nl, std::string_view name) noexcept {
  if (name.empty()) {
    return core::Result<std::uint16_t>::failure("bad gate name: empty");
  }
  for (std::uint8_t o = 0; o < 8; ++o) {
    const std::string_view kind_name = gate_kind_name(static_cast<GateKind>(o));
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

// Add a gate with explicit fan-in. Lowest free slot wins (deterministic);
// per-kind counter assigns then increments, never reused.
[[nodiscard]] inline core::Result<std::uint16_t> add_gate(Netlist &nl, GateKind kind, std::uint8_t inputs) noexcept {
  if (!valid_fanin(kind, inputs)) {
    return core::Result<std::uint16_t>::failure("bad fan-in for kind");
  }
  const std::uint8_t ord = kind_ordinal(kind);
  if (ord >= 8) {
    return core::Result<std::uint16_t>::failure("bad gate kind");
  }
  if (nl.next_counter[ord] == 0) {
    nl.next_counter[ord] = 1; // tolerate zero-init (scaffold default)
  }
  std::uint16_t slot = kNoDriver;
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    if (!nl.gates[s].live) {
      slot = s;
      break;
    }
  }
  if (slot == kNoDriver) {
    return core::Result<std::uint16_t>::failure("netlist full");
  }
  Gate &g = nl.gates[slot];
  g.live = true;
  g.kind = kind;
  g.counter = nl.next_counter[ord]++;
  if (nl.next_counter[ord] == 0) { // u32 wrap: skip 0 (0 never assigned)
    nl.next_counter[ord] = 1;
  }
  g.input_count = inputs;
  g.ignored_mask = 0;
  g.ignore_output = false;
  g.output_fanout = 0;
  g.propagation_delay_ps = 0;
  for (std::size_t i = 0; i < kMaxInputs; ++i) {
    g.drivers[i] = kNoDriver;
  }
  if (nl.live_count < kMaxGates) {
    ++nl.live_count;
  }
  nl.dirty = true;
  nl.topo_size = 0;
  return core::Result<std::uint16_t>::success(slot);
}

[[nodiscard]] inline core::Result<std::uint16_t> add_gate(Netlist &nl, GateKind kind) noexcept {
  return add_gate(nl, kind, default_fanin(kind));
}

// Remove a gate: detaches its inputs (releasing driver fanout) and clears
// every input it drives elsewhere. Counters never reused; slot recycles.
[[nodiscard]] inline core::Status remove_gate(Netlist &nl, std::uint16_t slot) noexcept {
  if (slot >= kMaxGates) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  Gate &g = nl.gates[slot];
  if (!g.live) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  for (std::uint8_t i = 0; i < g.input_count; ++i) {
    const std::uint16_t drv = g.drivers[i];
    if (drv != kNoDriver && drv < kMaxGates && nl.gates[drv].live) {
      if (nl.gates[drv].output_fanout > 0) {
        --nl.gates[drv].output_fanout;
      }
    }
    g.drivers[i] = kNoDriver;
  }
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    Gate &o = nl.gates[s];
    if (!o.live) {
      continue;
    }
    for (std::uint8_t i = 0; i < o.input_count; ++i) {
      if (o.drivers[i] == slot) {
        o.drivers[i] = kNoDriver;
      }
    }
  }
  g.live = false;
  g.ignored_mask = 0;
  g.ignore_output = false;
  g.output_fanout = 0;
  g.input_count = default_fanin(g.kind);
  for (std::size_t i = 0; i < kMaxInputs; ++i) {
    g.drivers[i] = kNoDriver;
  }
  if (nl.live_count > 0) {
    --nl.live_count;
  }
  nl.dirty = true;
  nl.topo_size = 0;
  return core::Status::success();
}

[[nodiscard]] inline core::Status remove_gate_by_name(Netlist &nl, std::string_view name) noexcept {
  auto gate = resolve_gate(nl, name);
  if (!gate.ok()) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  return remove_gate(nl, gate.value);
}

namespace detail {
// Reachability over the driver graph: can `from` reach `to` following
// output->input edges? Iterative DFS on a fixed stack (no alloc). Used
// to reject cycle-closing wires before mutation.
[[nodiscard]] inline bool reaches(const Netlist &nl, std::uint16_t from, std::uint16_t to) noexcept {
  if (from == to) {
    return true;
  }
  bool seen[kMaxGates] = {};
  std::uint16_t stack[kMaxGates];
  std::size_t top = 0;
  stack[top++] = from;
  seen[from] = true;
  while (top > 0) {
    const std::uint16_t cur = stack[--top];
    for (std::uint16_t s = 0; s < kMaxGates; ++s) {
      const Gate &g = nl.gates[s];
      if (!g.live) {
        continue;
      }
      bool uses_cur = false;
      for (std::uint8_t i = 0; i < g.input_count; ++i) {
        if (g.drivers[i] == cur) {
          uses_cur = true;
          break;
        }
      }
      if (!uses_cur || seen[s]) {
        continue;
      }
      if (s == to) {
        return true;
      }
      seen[s] = true;
      stack[top++] = s;
    }
  }
  return false;
}
} // namespace detail

// Single-driver wire: exactly one wire per destination input. Occupied
// inputs and cycle-closing wires are loud errors; the netlist is
// untouched on failure. Fan-out is unlimited.
[[nodiscard]] inline core::Status wire(Netlist &nl, std::uint16_t from_slot, std::uint16_t to_slot, std::uint8_t to_input) noexcept {
  if (from_slot >= kMaxGates || to_slot >= kMaxGates) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  Gate &from = nl.gates[from_slot];
  Gate &to = nl.gates[to_slot];
  if (!from.live || !to.live) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  if (to_input >= to.input_count) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  if (to.drivers[to_input] != kNoDriver) {
    return core::Status::failure(core::StatusCode::kInvalidArgument); // occupied
  }
  if (detail::reaches(nl, to_slot, from_slot)) {
    return core::Status::failure(core::StatusCode::kInvalidArgument); // cycle
  }
  to.drivers[to_input] = from_slot;
  ++from.output_fanout;
  nl.dirty = true;
  nl.topo_size = 0;
  return core::Status::success();
}

[[nodiscard]] inline core::Status unwire(Netlist &nl, std::uint16_t to_slot, std::uint8_t to_input) noexcept {
  if (to_slot >= kMaxGates) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  Gate &to = nl.gates[to_slot];
  if (!to.live) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  if (to_input >= to.input_count) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  const std::uint16_t drv = to.drivers[to_input];
  if (drv == kNoDriver) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  to.drivers[to_input] = kNoDriver;
  if (drv < kMaxGates && nl.gates[drv].live && nl.gates[drv].output_fanout > 0) {
    --nl.gates[drv].output_fanout;
  }
  nl.dirty = true;
  nl.topo_size = 0;
  return core::Status::success();
}

[[nodiscard]] inline core::Status wire_pins(Netlist &nl, PinRef from, PinRef to) noexcept {
  if (!from.is_output || to.is_output) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  return wire(nl, from.slot, to.slot, to.input);
}

[[nodiscard]] inline core::Status unwire_pin(Netlist &nl, PinRef pin) noexcept {
  if (pin.is_output) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  return unwire(nl, pin.slot, pin.input);
}

[[nodiscard]] inline core::Status wire_text(Netlist &nl, std::string_view from_text, std::string_view to_text) noexcept {
  auto from = parse_pin(nl, from_text);
  if (!from.ok()) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  auto to = parse_pin(nl, to_text);
  if (!to.ok()) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  return wire_pins(nl, from.value, to.value);
}

// ignore/use are idempotent: marking twice (or clearing an unmarked pin)
// still succeeds.
[[nodiscard]] inline core::Status ignore_pin(Netlist &nl, PinRef pin) noexcept {
  if (pin.slot >= kMaxGates || !nl.gates[pin.slot].live) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  Gate &g = nl.gates[pin.slot];
  if (pin.is_output) {
    g.ignore_output = true;
    return core::Status::success();
  }
  if (pin.input >= g.input_count) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  g.ignored_mask = static_cast<std::uint8_t>(g.ignored_mask | (1u << pin.input));
  return core::Status::success();
}

[[nodiscard]] inline core::Status use_pin(Netlist &nl, PinRef pin) noexcept {
  if (pin.slot >= kMaxGates || !nl.gates[pin.slot].live) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  Gate &g = nl.gates[pin.slot];
  if (pin.is_output) {
    g.ignore_output = false;
    return core::Status::success();
  }
  if (pin.input >= g.input_count) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  g.ignored_mask =
      static_cast<std::uint8_t>(g.ignored_mask & ~(1u << pin.input));
  return core::Status::success();
}

inline void reset_netlist(Netlist &nl) noexcept {
  for (std::size_t s = 0; s < kMaxGates; ++s) {
    Gate &g = nl.gates[s];
    g.live = false;
    g.kind = GateKind::kAnd;
    g.counter = 0;
    g.input_count = 2;
    g.ignored_mask = 0;
    g.ignore_output = false;
    g.output_fanout = 0;
    g.propagation_delay_ps = 0;
    for (std::size_t i = 0; i < kMaxInputs; ++i) {
      g.drivers[i] = kNoDriver;
    }
  }
  for (std::size_t i = 0; i < 8; ++i) {
    nl.next_counter[i] = 1;
  }
  nl.live_count = 0;
  nl.dirty = true;
  nl.topo_size = 0;
}

// Derived block interface (never stored): dangling inputs (unwired,
// non-ignored) and dangling outputs (fanout == 0, non-ignored) in slot
// order (inputs secondarily by input index). Fixed caller buffers, loud
// kOutOfRange when too small (*_n receives the required count).
[[nodiscard]] inline std::size_t block_input_count(const Netlist &nl) noexcept {
  std::size_t n = 0;
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    const Gate &g = nl.gates[s];
    if (!g.live) {
      continue;
    }
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      if (g.drivers[i] != kNoDriver) {
        continue;
      }
      if ((g.ignored_mask & (1u << i)) != 0) {
        continue;
      }
      ++n;
    }
  }
  return n;
}

[[nodiscard]] inline std::size_t block_output_count(const Netlist &nl) noexcept {
  std::size_t n = 0;
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    const Gate &g = nl.gates[s];
    if (!g.live || g.output_fanout != 0 || g.ignore_output) {
      continue;
    }
    ++n;
  }
  return n;
}

[[nodiscard]] inline core::Status block_interface(const Netlist &nl, BlockPin *in_buf, std::size_t in_cap,
                                                  std::size_t *in_n, BlockPin *out_buf,
                                                  std::size_t out_cap, std::size_t *out_n) noexcept {
  std::size_t need_in = 0;
  std::size_t need_out = 0;
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    const Gate &g = nl.gates[s];
    if (!g.live) {
      continue;
    }
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      if (g.drivers[i] != kNoDriver) {
        continue;
      }
      if ((g.ignored_mask & (1u << i)) != 0) {
        continue;
      }
      if (in_buf != nullptr && need_in < in_cap) {
        in_buf[need_in] = BlockPin{.slot = s, .input = i, .is_output = false};
      }
      ++need_in;
    }
  }
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    const Gate &g = nl.gates[s];
    if (!g.live || g.output_fanout != 0 || g.ignore_output) {
      continue;
    }
    if (out_buf != nullptr && need_out < out_cap) {
      out_buf[need_out] = BlockPin{.slot = s, .input = 0, .is_output = true};
    }
    ++need_out;
  }
  if (in_n != nullptr) {
    *in_n = need_in;
  }
  if (out_n != nullptr) {
    *out_n = need_out;
  }
  if (need_in > in_cap || need_out > out_cap) {
    return core::Status::failure(core::StatusCode::kOutOfRange);
  }
  return core::Status::success();
}

} // namespace wengine::logic
