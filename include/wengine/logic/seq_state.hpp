#pragma once

/**
 * @file
 * Parallel clocked-state pool beside the Netlist (LOGIC-V2-SEQ2): the
 * state stage of the logic pipeline (netlist parse -> state -> eval ->
 * layout/topo -> optimize). Sequential gates (DFF/DLATCH/JK/T/SRLATCH)
 * carry state across steps, but `Netlist` never reshapes
 * (docs/logic.md §12): clocked Q values live here, indexed by
 * Netlist slot — the same pattern as `world::Signals` beside
 * `Particles`. The stepper (SEQ4) owns both objects and re-syncs via
 * `init_seq_state` (same ownership shape as the CLI/GUI controllers
 * re-attaching non-owning sinks after every rebuild).
 *
 * IDENTITY (mirrors SIG-5): every attached flop owns a stable
 * `serial` (u64, monotonic per pool, never reused — even across slot
 * recycle), so later control-plane/CSV/Frame addressing (SEQ5+) stays
 * unambiguous across despawn/respawn. Slots recycle, serials don't.
 * `find_seq_serial` resolves a serial to its live slot (linear scan;
 * flop counts are small). An explicit `reset_seq_state` starts a new
 * pool lifetime (serials restart from 0), same as `Signals::clear`.
 *
 * LIFETIME: `init_seq_state` is a full resync and is idempotent —
 * still-valid slots keep BOTH serial and Q (state persists across
 * steps); fresh attaches power on at `kLow`; detaches retire the
 * serial and leave Q stale (mirrors `Signals::detach` keeping
 * `value[]` stale).
 *
 * Dependency rules: header-only, allocation-free, exception-free,
 * noexcept throughout. Fixed capacity (`kMaxGates` slots, like
 * `Netlist` itself); no syscalls, no iostream. Logic is a standalone
 * domain — never depends on CLI types. Consumers: seq_step.hpp (the
 * stepper), the CLI `step`/`poke`/`peek` verbs, the engine
 * SequentialSolver, and test_logic_seq_state.
 */

#include "wengine/logic/netlist.hpp"
#include "wengine/logic/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace wengine::logic {

// (is_sequential_kind lives in types.hpp beside the GateKind enum — kind
// vocabulary with kind vocabulary — and is used here, by the TieSet and
// BlockInterface seams, and by wire()'s cycle check.)

/**
 * @brief Parallel clocked-state pool beside the Netlist: per-slot Q
 *        plus valid flags plus never-reused serials.
 */
struct SeqState {
  /** @brief Latched Q per slot (kLow at fresh attach). */
  std::array<LogicValue, kMaxGates> q{};       // latched Q per slot (kLow at fresh attach)
  /** @brief 1 when the slot holds a live flop's state. */
  std::array<std::uint8_t, kMaxGates> valid{}; // 1 = slot holds a live flop's state
  /** @brief Stable flop id per slot, never reused across the pool lifetime. */
  std::array<std::uint64_t, kMaxGates> serial{}; // stable flop id, never reused
  /** @brief Next serial to assign; reset only by reset_seq_state. */
  std::uint64_t next_serial{0};
  /** @brief Number of attached (valid) slots. */
  std::uint16_t live_count{0};

  /**
   * @brief Value equality (whole-pool compare, defaulted).
   *
   * @return True when Q, valid, serial, and counter fields all match.
   */
  bool operator==(const SeqState &) const noexcept = default;
};

/**
 * @brief True when slot holds an attached flop's state.
 *
 * @param[in] st Pool to query.
 * @param[in] slot Netlist slot.
 * @return True when slot is in range and valid.
 */
[[nodiscard]] inline bool seq_has(const SeqState &st, std::uint16_t slot) noexcept {
  return slot < kMaxGates && st.valid[slot] != 0;
}

/**
 * @brief Resolve a flop serial to its live slot.
 *
 * @param[in] st Pool to search (linear scan; flop counts are small).
 * @param[in] serial Stable serial to find.
 * @return Live slot holding serial; nullopt when absent (unknown or
 *         retired) — never a stale slot.
 */
[[nodiscard]] inline std::optional<std::uint16_t> find_seq_serial(const SeqState &st, std::uint64_t serial) noexcept {
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    if (st.valid[s] != 0 && st.serial[s] == serial) {
      return s;
    }
  }
  return std::nullopt;
}

/**
 * @brief Full resync from a Netlist: live sequential slots attach
 *        (fresh serial, power-on kLow), dead/combinational slots detach
 *        (serial retired, Q left stale).
 *
 * Still-valid slots are untouched — Q persists across steps, so the
 * stepper may sync every step. Idempotent. No Netlist mutation.
 *
 * @param[out] st Pool to resync.
 * @param[in] nl Netlist defining which slots want state.
 */
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

/**
 * @brief Clear the whole pool (new lifetime: serials restart from 0).
 *
 * @param[out] st Pool to clear.
 */
inline void reset_seq_state(SeqState &st) noexcept { st = SeqState{}; }

/**
 * @brief True when any live sequential gate exists (the wire verb takes
 *        the sequential validation path only then; otherwise the old
 *        combinational path).
 *
 * @param[in] nl Netlist to scan.
 * @return True when at least one live DFF/DLATCH/JK/T/SRLATCH exists.
 */
[[nodiscard]] inline bool has_sequential_gates(const Netlist &nl) noexcept {
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    const Gate &g = nl.gates[s];
    if (g.live && is_sequential_kind(g.kind)) {
      return true;
    }
  }
  return false;
}

// Cycle validation lives in topo.hpp (`topo::validate` with
// EdgeFilter::kCombOnly): driver edges into sequential-gate inputs
// (DFF.D/CLK, DLATCH.D/EN, JK.J/K/CLK, T.T/CLK, SRLATCH.S/R) are excluded
// from the cycle edge set — flop Q outputs break loops, so feedback
// through flops (counters, shift registers) validates while pure
// combinational cycles still fail loudly. `topo::rebuild(kAll)` still
// sees ALL edges, so solve() on sequential loops keeps failing loudly;
// loaders/steppers call `topo::validate` instead.

} // namespace wengine::logic
