#pragma once

// Logic-gate netlist vocabulary (scaffold — LOGIC-2 fills in eval).
//
// Design (docs/logic.md §4): combinational block of gates with variable
// fan-in (2..8, NOT/BUF exactly 1). Identity is per-kind monotonic
// counters never reused (AND1, NOT5, ...), slots recycle — the same
// convention as signal serials. Pins spell `<NAME>.Y` (output) or
// `<NAME>.<1..k>` (input). Single-driver inputs, unlimited fan-out,
// cycles rejected (combinational v1 only).
//
// Header-only, dependency-free (STATUS-free vocabulary; fallible ops use
// core::Status/Result at the eval/io layer, never here). No iostream.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace wengine::logic {

// Append-only: persisted as bytes in YAML v1 (docs/logic.md §9).
// Values 0..7 are combinational; sequential kinds (DFF/LATCH/...) append
// at >= 16 in V2 (docs/logic.md §12) — never renumber, never reuse.
enum class GateKind : std::uint8_t {
  kAnd = 0,
  kOr = 1,
  kNand = 2,
  kNor = 3,
  kXor = 4,
  kXnor = 5,
  kNot = 6,
  kBuf = 7,
};

// Net value. V1 produces/consumes only kLow/kHigh; kHighZ/kUnknown exist
// now so tri-state buses (V2, docs/logic.md §12) need no type change —
// eval rejects them loudly until then.
enum class LogicValue : std::uint8_t {
  kLow = 0,
  kHigh = 1,
  kHighZ = 2,   // undriven / tri-state off (V2)
  kUnknown = 3, // conflict marker (V2 multi-driver resolution)
};

// Single funnel every input resolves through. V1: exactly one driver
// (callers enforce; multi-value spans are a loud error). V2 replaces
// this body with wired-OR / tri-state resolution — call sites unchanged.
[[nodiscard]] constexpr bool resolve_net(const LogicValue *drivers, std::size_t n, LogicValue &out) noexcept {
  if (n != 1) {
    return false;
  }
  if (drivers[0] != LogicValue::kLow && drivers[0] != LogicValue::kHigh) {
    return false; // Z/X rejected until V2 tri-state (docs/logic.md §12).
  }
  out = drivers[0];
  return true;
}

inline constexpr std::size_t kMaxInputs = 8;
inline constexpr std::size_t kMaxGates = 1024; // slot cap, loud on overflow

[[nodiscard]] constexpr std::string_view gate_kind_name(GateKind k) noexcept {
  switch (k) {
  case GateKind::kAnd:
    return "AND";
  case GateKind::kOr:
    return "OR";
  case GateKind::kNand:
    return "NAND";
  case GateKind::kNor:
    return "NOR";
  case GateKind::kXor:
    return "XOR";
  case GateKind::kXnor:
    return "XNOR";
  case GateKind::kNot:
    return "NOT";
  case GateKind::kBuf:
    return "BUF";
  }
  return "?";
}

// One gate slot. `live` false => free-list entry. `counter` is the
// per-kind monotonic id (display: AND<counter>); `input_count` the
// configured fan-in; `ignored_inputs` bitmask (bit i => input i+1
// excluded from the derived block interface); `drivers` holds the
// source slot index per input, kNoDriver when unwired.
inline constexpr std::uint16_t kNoDriver = 0xFFFF;

struct Gate {
  bool live = false;
  GateKind kind = GateKind::kAnd;
  std::uint32_t counter = 0; // per-kind, never reused
  std::uint8_t input_count = 2;
  std::uint8_t ignored_mask = 0; // bit i => input (i+1) ignored
  std::uint16_t output_fanout = 0; // derived, for list/iface
  std::uint64_t propagation_delay_ps = 0; // V2 timing hook (§12): stored
                                          // now, honored later; v1 eval is
                                          // zero-delay regardless of value
  std::array<std::uint16_t, kMaxInputs> drivers{};
};

// Fixed-capacity SoA-ish pool: slots recycle, counters don't.
// Mutation marks `dirty` (topology must be recompiled before eval).
struct Netlist {
  std::array<Gate, kMaxGates> gates{};
  std::array<std::uint32_t, 8> next_counter{}; // per GateKind ordinal
  std::uint16_t live_count = 0;
  bool dirty = true;
};

} // namespace wengine::logic
