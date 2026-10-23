#pragma once

// Parallel clocked-state pool beside the Netlist (LOGIC-V2-SEQ2).
//
// Sequential gates (DFF/DLATCH, SEQ1 vocabulary) carry state across steps,
// but `Netlist` never reshapes (docs/logic.md §12): clocked Q values live
// here, indexed by Netlist slot — the same pattern as `world::Signals`
// beside `Particles`. The future stepper (SEQ4) owns both objects and
// re-syncs via `init_seq_state` (same ownership shape as the CLI/GUI
// controllers re-attaching non-owning sinks after every rebuild).
//
// Header-only, allocation-free, exception-free, noexcept throughout.
// Fixed capacity (`kMaxGates` slots, like `Netlist` itself); no syscalls,
// no iostream.
//
// IDENTITY (mirrors SIG-5): every attached flop owns a stable `serial`
// (u64, monotonic per pool, never reused — even across slot recycle), so
// later control-plane/CSV/Frame addressing (SEQ5+) stays unambiguous
// across despawn/respawn. Slots recycle, serials don't. `find_seq_serial`
// resolves a serial to its live slot (linear scan; flop counts are small).
// An explicit `reset_seq_state` starts a new pool lifetime (serials restart
// from 0), same as `Signals::clear`.
//
// LIFETIME: `init_seq_state` is a full resync and is idempotent —
// still-valid slots keep BOTH serial and Q (state persists across steps);
// fresh attaches power on at `kLow`; detaches retire the serial and leave
// Q stale (mirrors `Signals::detach` keeping `value[]` stale).

#include "wengine/logic/netlist.hpp"
#include "wengine/logic/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace wengine::logic {

// (is_sequential_kind lives in netlist.hpp beside valid_fanin — vocabulary
// with vocabulary — and is used here and by wire()'s cycle check.)

struct SeqState {
  std::array<LogicValue, kMaxGates> q{};       // latched Q per slot (kLow at fresh attach)
  std::array<std::uint8_t, kMaxGates> valid{}; // 1 = slot holds a live flop's state
  std::array<std::uint64_t, kMaxGates> serial{}; // stable flop id, never reused
  std::uint64_t next_serial{0};
  std::uint16_t live_count{0};
};

[[nodiscard]] inline bool seq_has(const SeqState &st, std::uint16_t slot) noexcept {
  return slot < kMaxGates && st.valid[slot] != 0;
}

// Resolve a flop serial to its live slot. Absent (unknown or retired) is
// nullopt — never a stale slot.
[[nodiscard]] inline std::optional<std::uint16_t> find_seq_serial(const SeqState &st, std::uint64_t serial) noexcept {
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    if (st.valid[s] != 0 && st.serial[s] == serial) {
      return s;
    }
  }
  return std::nullopt;
}

// Full resync from a Netlist: live sequential slots attach (fresh serial,
// power-on kLow), dead/combinational slots detach (serial retired, Q left
// stale). Still-valid slots are untouched — Q persists across steps, so the
// stepper may sync every step. Idempotent. No Netlist mutation.
inline void init_seq_state(SeqState &st, const Netlist &nl) noexcept {
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    const Gate &g = nl.gates[s];
    const bool want = g.live && is_sequential_kind(g.kind);
    const bool have = st.valid[s] != 0;
    if (want && !have) {
      st.valid[s] = 1;
      st.q[s] = LogicValue::kLow;
      st.serial[s] = st.next_serial++;
      ++st.live_count;
    } else if (!want && have) {
      st.valid[s] = 0;
      --st.live_count;
    }
  }
}

// Clear the whole pool (new lifetime: serials restart from 0).
inline void reset_seq_state(SeqState &st) noexcept { st = SeqState{}; }

// True when any live sequential gate exists (the wire verb takes the
// sequential validation path only then; otherwise the old comb path).
[[nodiscard]] inline bool has_sequential_gates(const Netlist &nl) noexcept {
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    const Gate &g = nl.gates[s];
    if (g.live && is_sequential_kind(g.kind)) {
      return true;
    }
  }
  return false;
}

// Whole-graph cycle validation over COMBINATIONAL edges only
// (LOGIC-V2-SEQ3): Kahn's algorithm in slot order (same determinism as
// ensure_topology), except driver edges into sequential-gate inputs
// (DFF.D/CLK, DLATCH.D/EN) are excluded from the cycle edge set — flop Q
// outputs break loops, so feedback through flops (counters, shift
// registers) validates while pure combinational cycles still fail loudly.
// Pure: no mutation, fixed stack arrays, noexcept. ensure_topology still
// sees ALL edges, so solve() on sequential loops keeps failing loudly
// until SEQ4 stepping arrives; loaders/steppers call this instead.
[[nodiscard]] inline core::Status validate_sequential(const Netlist &nl) noexcept {
  std::uint8_t indeg[kMaxGates] = {};
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    const Gate &g = nl.gates[s];
    if (!g.live || is_sequential_kind(g.kind)) {
      continue; // flops take no cycle-edges: always Kahn roots
    }
    std::uint8_t d = 0;
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      if (g.drivers[i] != kNoDriver) {
        ++d;
      }
    }
    indeg[s] = d;
  }
  std::uint16_t queue[kMaxGates];
  std::size_t head = 0;
  std::size_t tail = 0;
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    if (nl.gates[s].live && indeg[s] == 0) {
      queue[tail++] = s;
    }
  }
  std::size_t order = 0;
  while (head < tail) {
    const std::uint16_t cur = queue[head++];
    ++order;
    // Successors in slot order (deterministic push order): users of cur
    // whose inputs are cycle-edges (combinational destinations only).
    for (std::uint16_t s = 0; s < kMaxGates; ++s) {
      const Gate &g = nl.gates[s];
      if (!g.live || is_sequential_kind(g.kind) || indeg[s] == 0) {
        continue;
      }
      for (std::uint8_t i = 0; i < g.input_count; ++i) {
        if (g.drivers[i] == cur) {
          --indeg[s];
          if (indeg[s] == 0) {
            queue[tail++] = s;
            break; // one push per gate (remaining inputs counted already)
          }
        }
      }
    }
  }
  if (order != nl.live_count) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  return core::Status::success();
}

} // namespace wengine::logic
