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
#include "wengine/logic/block_interface.hpp"
#include "wengine/logic/tie_set.hpp"
#include "wengine/logic/topo.hpp"
#include "wengine/logic/types.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace wengine::logic {

// Pin vocabulary lives in types.hpp (so tie_set.hpp can take pins without
// including this header); tie math + block-interface derivation live in
// tie_set.hpp / block_interface.hpp. This header owns mutation + parsing.

[[nodiscard]] constexpr std::uint8_t kind_ordinal(GateKind k) noexcept {
  return static_cast<std::uint8_t>(k);
}

[[nodiscard]] constexpr bool valid_fanin(GateKind k, std::uint8_t n) noexcept {
  if (k == GateKind::kNot || k == GateKind::kBuf) {
    return n == 1;
  }
  if (k == GateKind::kDff) {
    // Bare D+CLK (2) or full control set D+CLK+RST+SET+EN (5, opt-in via
    // `add DFF --inputs 5`); bare flops stay the default (zero-migration).
    return n == 2 || n == 5;
  }
  if (k == GateKind::kDlatch) {
    return n == 2; // D+EN (DFF) / D+EN (DLATCH)
  }
  if (k == GateKind::kJk) {
    return n == 3; // J+K+CLK (003-jk-t-flops)
  }
  if (k == GateKind::kT) {
    return n == 2; // T+CLK (003-jk-t-flops)
  }
  if (k == GateKind::kSrlatch) {
    return n == 2; // S+R (004-sr-d-latches)
  }
  return n >= 2 && n <= kMaxInputs;
}

[[nodiscard]] constexpr std::uint8_t default_fanin(GateKind k) noexcept {
  if (k == GateKind::kNot || k == GateKind::kBuf) {
    return 1;
  }
  // Combinational 2..8 default 2; DFF/DLATCH/T/SR default 2 (D + CLK/EN,
  // T + CLK, S + R); JK defaults to 3 (J + K + CLK). The DFF full control
  // set is opt-in (`add DFF --inputs 5`).
  if (k == GateKind::kJk) {
    return 3;
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

// Case-insensitive ("and", "AND", "And" all match). Exact full match.
// Sequential kinds (DFF/DLATCH/JK/T/SRLATCH) parse here; creation validates
// fan-in via valid_fanin and state attaches via init_seq_state.
[[nodiscard]] inline core::Result<GateKind> parse_gate_kind(std::string_view text) noexcept {
  if (text.empty()) {
    return core::Result<GateKind>::failure("bad gate kind: empty");
  }
  // Append-only table: keep every known (name, kind) pair here so the
  // persisted byte values (DFF=16, DLATCH=17, JK=18, T=19, SR=20) never shift.
  struct KindEntry {
    std::string_view name;
    GateKind kind;
  };
  constexpr KindEntry kKinds[] = {
      {"AND", GateKind::kAnd},
      {"OR", GateKind::kOr},
      {"NAND", GateKind::kNand},
      {"NOR", GateKind::kNor},
      {"XOR", GateKind::kXor},
      {"XNOR", GateKind::kXnor},
      {"NOT", GateKind::kNot},
      {"BUF", GateKind::kBuf},
      {"DFF", GateKind::kDff},
      {"DLATCH", GateKind::kDlatch},
      {"JK", GateKind::kJk},
      {"T", GateKind::kT},
      {"SRLATCH", GateKind::kSrlatch},
  };
  for (const KindEntry &e : kKinds) {
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
  constexpr GateKind kAllKinds[] = {
      GateKind::kAnd, GateKind::kOr, GateKind::kNand, GateKind::kNor,
      GateKind::kXor, GateKind::kXnor, GateKind::kNot, GateKind::kBuf,
      GateKind::kDff, GateKind::kDlatch, GateKind::kJk, GateKind::kT,
      GateKind::kSrlatch,
  };
  for (GateKind kind : kAllKinds) {
    const std::uint8_t o = kind_ordinal(kind);
    const std::string_view kind_name = gate_kind_name(kind);
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
// case-insensitive): DFF `D`/`CLK`/`Q`/`NQ` on every flop, plus `RST`/
// `SET`/`EN` on full-control (5-input) flops only; DLATCH `D`/`EN`/`Q`/
// `NQ`; JK `J`/`K`/`CLK`/`Q`/`NQ`; T `T`/`CLK`/`Q`/`NQ`; SRLATCH `S`/
// `R`/`Q`/`NQ`. Numeric aliases
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
    if (g.kind == GateKind::kJk) {
      if (match_pin_name(suffix, "J")) {
        PinRef ref{};
        ref.slot = slot;
        ref.is_output = false;
        ref.input = 0;
        return core::Result<PinRef>::success(ref);
      }
      if (match_pin_name(suffix, "K")) {
        PinRef ref{};
        ref.slot = slot;
        ref.is_output = false;
        ref.input = 1;
        return core::Result<PinRef>::success(ref);
      }
      if (match_pin_name(suffix, "CLK")) {
        PinRef ref{};
        ref.slot = slot;
        ref.is_output = false;
        ref.input = 2;
        return core::Result<PinRef>::success(ref);
      }
    } else if (g.kind == GateKind::kT) {
      if (match_pin_name(suffix, "T")) {
        PinRef ref{};
        ref.slot = slot;
        ref.is_output = false;
        ref.input = 0;
        return core::Result<PinRef>::success(ref);
      }
      if (match_pin_name(suffix, "CLK")) {
        PinRef ref{};
        ref.slot = slot;
        ref.is_output = false;
        ref.input = 1;
        return core::Result<PinRef>::success(ref);
      }
    } else if (g.kind == GateKind::kSrlatch) {
      if (match_pin_name(suffix, "S")) {
        PinRef ref{};
        ref.slot = slot;
        ref.is_output = false;
        ref.input = 0;
        return core::Result<PinRef>::success(ref);
      }
      if (match_pin_name(suffix, "R")) {
        PinRef ref{};
        ref.slot = slot;
        ref.is_output = false;
        ref.input = 1;
        return core::Result<PinRef>::success(ref);
      }
    } else {
      if (match_pin_name(suffix, "D")) {
        PinRef ref{};
        ref.slot = slot;
        ref.is_output = false;
        ref.input = 0;
        return core::Result<PinRef>::success(ref);
      }
      const std::string_view clk_name = (g.kind == GateKind::kDff) ? "CLK" : "EN";
      if (match_pin_name(suffix, clk_name)) {
        PinRef ref{};
        ref.slot = slot;
        ref.is_output = false;
        ref.input = 1;
        return core::Result<PinRef>::success(ref);
      }
    // DFF full control set (5-input flops via `add DFF --inputs 5`):
    // async reset/set + clock enable. Each name is valid only when the
    // flop was created with that input — bare 2-input flops reject them
    // exactly like out-of-range numerics (`.3`) do below.
    if (g.kind == GateKind::kDff) {
      if (g.input_count > 2 && match_pin_name(suffix, "RST")) {
        PinRef ref{};
        ref.slot = slot;
        ref.is_output = false;
        ref.input = 2;
        return core::Result<PinRef>::success(ref);
      }
      if (g.input_count > 3 && match_pin_name(suffix, "SET")) {
        PinRef ref{};
        ref.slot = slot;
        ref.is_output = false;
        ref.input = 3;
        return core::Result<PinRef>::success(ref);
      }
      if (g.input_count > 4 && match_pin_name(suffix, "EN")) {
        PinRef ref{};
        ref.slot = slot;
        ref.is_output = false;
        ref.input = 4;
        return core::Result<PinRef>::success(ref);
      }
      }
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

// Add a gate with explicit fan-in. Lowest free slot wins (deterministic);
// per-kind counter assigns then increments, never reused.
[[nodiscard]] inline core::Result<std::uint16_t> add_gate(Netlist &nl, GateKind kind, std::uint8_t inputs) noexcept {
  if (!valid_fanin(kind, inputs)) {
    return core::Result<std::uint16_t>::failure("bad fan-in for kind");
  }
  const std::uint8_t ord = kind_ordinal(kind);
  if (ord >= 8 && !is_sequential_kind(kind)) {
    // No such kind exists today (ordinals are 0..7 + 16..20); the guard
    // stays so future appended kinds fail loudly until wired up.
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
  g.ignore_nq_output = is_sequential_kind(kind); // NQ hidden unless `use`d
  g.output_fanout = 0;
  g.nq_fanout = 0;
  g.propagation_delay_ps = 0;
  for (std::size_t i = 0; i < kMaxInputs; ++i) {
    g.drivers[i] = kNoDriver;
    nl.driver_inv[tie_flat(slot, static_cast<std::uint8_t>(i))] = 0;
    // tie_rep entries are already 0 by the remove/reset invariant
    // (detach_slot_ties clears on remove, reset_all_ties on reset).
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
// Tied pins of the removed gate leave their classes (remaining members
// re-elect the lowest surviving pin as leader).
[[nodiscard]] inline core::Status remove_gate(Netlist &nl, std::uint16_t slot) noexcept {
  if (slot >= kMaxGates) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  Gate &g = nl.gates[slot];
  if (!g.live) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  // Detach ties first (while input_count is still valid): each tied pin
  // of this gate leaves its class via the TieSet seam; survivors re-elect.
  detach_slot_ties(nl, slot);
  for (std::uint8_t i = 0; i < g.input_count; ++i) {
    const std::uint16_t drv = g.drivers[i];
    if (drv != kNoDriver && drv < kMaxGates && nl.gates[drv].live) {
      const std::size_t flat = tie_flat(slot, i);
      if (nl.driver_inv[flat] != 0) {
        nl.driver_inv[flat] = 0;
        if (nl.gates[drv].nq_fanout > 0) {
          --nl.gates[drv].nq_fanout;
        }
      } else if (nl.gates[drv].output_fanout > 0) {
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
        nl.driver_inv[tie_flat(s, i)] = 0;
      }
    }
  }
  g.live = false;
  g.ignored_mask = 0;
  g.ignore_output = false;
  g.ignore_nq_output = true; // fresh-state default (NQ hidden unless `use`d)
  g.output_fanout = 0;
  g.nq_fanout = 0;
  g.input_count = default_fanin(g.kind);
  for (std::size_t i = 0; i < kMaxInputs; ++i) {
    g.drivers[i] = kNoDriver;
    nl.driver_inv[tie_flat(slot, static_cast<std::uint8_t>(i))] = 0;
    // tie entries already cleared by detach_slot_ties above.
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

// Single-driver wire: exactly one wire per destination input. Occupied
// inputs, tied destinations (untie first), and cycle-closing wires are
// loud errors; the netlist is untouched on failure. Fan-out is unlimited.
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
  if (is_tied(nl, to_slot, to_input)) {
    return core::Status::failure(core::StatusCode::kInvalidArgument); // tied: untie first
  }
  // Q-boundary: an edge into a sequential-gate input can never close a
  // combinational cycle (flop Q outputs break loops — see
  // validate_sequential, which excludes exactly these edges), so the
  // incremental check is vacuous there and acceptance never depends on
  // wiring order. The whole-graph validator stays authoritative for
  // combinational destinations (a live-but-uninvolved flop shields
  // nothing).
  if (!is_sequential_kind(to.kind) && topo::reaches(nl, to_slot, from_slot, EdgeFilter::kCombOnly)) {
    return core::Status::failure(core::StatusCode::kInvalidArgument); // cycle
  }
  to.drivers[to_input] = from_slot;
  ++from.output_fanout;
  nl.driver_inv[tie_flat(to_slot, to_input)] = 0; // slot-wire is never inverted
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
  if (drv < kMaxGates && nl.gates[drv].live) {
    // Polarity-aware release: NQ edges decrement nq_fanout, Q edges
    // decrement output_fanout.
    const std::size_t flat = tie_flat(to_slot, to_input);
    if (nl.driver_inv[flat] != 0) {
      nl.driver_inv[flat] = 0;
      if (nl.gates[drv].nq_fanout > 0) {
        --nl.gates[drv].nq_fanout;
      }
    } else if (nl.gates[drv].output_fanout > 0) {
      --nl.gates[drv].output_fanout;
    }
  }
  nl.dirty = true;
  nl.topo_size = 0;
  return core::Status::success();
}

[[nodiscard]] inline core::Status wire_pins(Netlist &nl, PinRef from, PinRef to) noexcept {
  if (!from.is_output || to.is_output) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  if (from.slot >= kMaxGates || !nl.gates[from.slot].live) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  // NQ sources exist on sequential gates only (parse_pin gates this, but
  // slot-built refs bypass text — stay loud, never silent).
  if (from.negated && !is_sequential_kind(nl.gates[from.slot].kind)) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  const core::Status st = wire(nl, from.slot, to.slot, to.input);
  if (!st.ok() || !from.negated) {
    return st;
  }
  // Move the edge from the Q fanout to the NQ fanout and mark the sink
  // inverted (wire() always establishes the non-inverted state).
  Gate &src = nl.gates[from.slot];
  if (src.output_fanout > 0) {
    --src.output_fanout;
  }
  ++src.nq_fanout;
  nl.driver_inv[tie_flat(to.slot, to.input)] = 1;
  return core::Status::success();
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
// still succeeds. Ignoring a tied input fails loudly (untie first);
// tied pins are never ignored by construction.
[[nodiscard]] inline core::Status ignore_pin(Netlist &nl, PinRef pin) noexcept {
  if (pin.slot >= kMaxGates || !nl.gates[pin.slot].live) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  Gate &g = nl.gates[pin.slot];
  if (pin.is_output) {
    if (pin.negated) {
      g.ignore_nq_output = true;
    } else {
      g.ignore_output = true;
    }
    return core::Status::success();
  }
  if (pin.input >= g.input_count) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  if (is_tied(nl, pin.slot, pin.input)) {
    return core::Status::failure(core::StatusCode::kInvalidArgument); // tied: untie first
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
    if (pin.negated) {
      g.ignore_nq_output = false;
    } else {
      g.ignore_output = false;
    }
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
    g.ignore_nq_output = true; // fresh-state default (NQ hidden unless `use`d)
    g.output_fanout = 0;
    g.nq_fanout = 0;
    g.propagation_delay_ps = 0;
    for (std::size_t i = 0; i < kMaxInputs; ++i) {
      g.drivers[i] = kNoDriver;
    }
    for (std::size_t i = 0; i < kMaxInputs; ++i) {
      nl.driver_inv[static_cast<std::size_t>(s) * kMaxInputs + i] = 0;
    }
  }
  reset_all_ties(nl);
  for (std::size_t i = 0; i < nl.next_counter.size(); ++i) {
    nl.next_counter[i] = 1;
  }
  nl.live_count = 0;
  nl.dirty = true;
  nl.topo_size = 0;
}

// Text adapters over the TieSet seam: parse pins, then delegate.
[[nodiscard]] inline core::Status tie_text(Netlist &nl, std::string_view a_text, std::string_view b_text) noexcept {
  auto a = parse_pin(nl, a_text);
  if (!a.ok()) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  auto b = parse_pin(nl, b_text);
  if (!b.ok()) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  return tie_pins(nl, a.value, b.value);
}

[[nodiscard]] inline core::Status untie_text(Netlist &nl, std::string_view text) noexcept {
  auto p = parse_pin(nl, text);
  if (!p.ok()) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  return untie_pin(nl, p.value);
}

} // namespace wengine::logic
