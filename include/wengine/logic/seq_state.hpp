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
 * unambiguous across despawn/respawn. `find_seq_serial` resolves a
 * serial to its live slot (linear scan; flop counts are small). An
 * explicit `reset_seq_state` starts a new pool lifetime (serials
 * restart from 0), same as `Signals::clear`.
 *
 * LIFETIME: `init_seq_state` is a full resync and is idempotent —
 * still-valid slots keep BOTH serial and Q (state persists across
 * steps); fresh attaches power on at `kLow`; detaches retire the
 * serial and leave Q stale (mirrors `Signals::detach` keeping
 * `value[]` stale).
 *
 * Capacity (015): dynamic pool — SeqState grows beside Netlist via
 * ensure_seq_state (value-init zeros, loud false past kMaxGates);
 * every bound is the current pool size (slot_count for Netlist scans,
 * the state-pool size for state scans), never kMaxGates. Step scratch
 * travels in the caller-owned SeqScratch span bundle below, validated
 * loudly before any work.
 *
 * Dependency rules: header-only; hot paths allocation-free (growth
 * happens only in ensure/init entry points, amortized). No syscalls,
 * no iostream. Logic is a standalone domain — never depends on CLI
 * types. Consumers: seq_step.hpp (the stepper), the CLI `step`/`poke`/
 * `peek` verbs, the engine SequentialSolver, and test_logic_seq_state.
 */

#include "wengine/logic/netlist.hpp"
#include "wengine/logic/types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace wengine::logic {

// (is_sequential_kind lives in types.hpp beside the GateKind enum — kind
// vocabulary with kind vocabulary — and is used here, by the TieSet and
// BlockInterface seams, and by wire()'s cycle check.)

/**
 * @brief Parallel clocked-state pool beside the Netlist: per-slot Q
 *        plus valid flags plus never-reused serials.
 *
 * Dynamic pool: q/valid/serial grow beside the Netlist via
 * ensure_seq_state (value-init zeros); every scan is bounded by the
 * current pool size, never by kMaxGates.
 */
struct SeqState {
  /** @brief Latched Q per slot (kLow at fresh attach). */
  std::vector<LogicValue> q{}; // latched Q per slot (kLow at fresh attach)
  /** @brief 1 when the slot holds a live flop's state. */
  std::vector<std::uint8_t> valid{}; // 1 = slot holds a live flop's state
  /** @brief Stable flop id per slot, never reused across the pool lifetime. */
  std::vector<std::uint64_t> serial{}; // stable flop id, never reused
  /** @brief Next serial to assign; reset only by reset_seq_state. */
  std::uint64_t next_serial{0};
  /** @brief Number of attached (valid) slots. */
  Slot live_count{0};

  /**
   * @brief Value equality over pool semantics.
   *
   * Like Netlist: pool vectors may differ in allocated size (loaded
   * pools are stride-sized, live pools track the netlist). The common
   * prefix compares exactly; any longer tail must be inert (kLow Q,
   * detached valid, zero serials).
   *
   * @param[in] o Other pool to compare against.
   * @return True when Q, valid, serial, and counter fields all match.
   */
  [[nodiscard]] bool operator==(const SeqState &o) const noexcept {
    if (live_count != o.live_count || next_serial != o.next_serial) {
      return false;
    }
    return prefix_equal_inert(q, o.q, LogicValue::kLow) && prefix_equal_inert(valid, o.valid, std::uint8_t{0}) && prefix_equal_inert(serial, o.serial, std::uint64_t{0});
  }

 private:
  // Common-prefix equality with an all-`inert` tail on the longer side.
  template <typename T>
  [[nodiscard]] static bool prefix_equal_inert(const std::vector<T> &a, const std::vector<T> &b, const T &inert) noexcept {
    const std::size_t n = a.size() < b.size() ? a.size() : b.size();
    for (std::size_t i = 0; i < n; ++i) {
      if (!(a[i] == b[i])) {
        return false;
      }
    }
    for (std::size_t i = n; i < a.size(); ++i) {
      if (!(a[i] == inert)) {
        return false;
      }
    }
    for (std::size_t i = n; i < b.size(); ++i) {
      if (!(b[i] == inert)) {
        return false;
      }
    }
    return true;
  }
};

/**
 * @brief Grow the clocked-state pool so slot `n - 1` is addressable.
 *
 * Geometric growth (doubling from kInitialSlots); new entries
 * value-init to zero (kLow / detached / serial 0 — the inert state).
 * Never shrinks. Growth past kMaxGates fails loudly (false), never
 * silently. No-op when already sized (hot paths stay allocation-free).
 *
 * @param[out] st Pool to grow.
 * @param[in] n Minimum addressable slot count.
 * @return True when `n` slots are addressable; false when n exceeds
 *         kMaxGates ("pool full"). True requires no work when already
 *         sized.
 */
[[nodiscard]] inline bool ensure_seq_state(SeqState &st, std::size_t n) {
  if (n > kMaxGates) {
    return false; // loud max cap: the caller maps this to Status
  }
  if (st.q.size() >= n && st.valid.size() >= n && st.serial.size() >= n) {
    return true;
  }
  std::size_t cap = st.q.size() * 2;
  if (cap < kInitialSlots) {
    cap = kInitialSlots;
  }
  if (cap < n) {
    cap = n;
  }
  if (cap > kMaxGates) {
    cap = kMaxGates;
  }
  st.q.resize(cap, LogicValue::kLow);
  st.valid.resize(cap, std::uint8_t{0});
  st.serial.resize(cap, std::uint64_t{0});
  return true;
}

/**
 * @brief Caller-owned sequential-step scratch bundle (see the
 *        data-model scratch table): resolved values, combinational
 *        order, per-slot outputs, next-Q snapshots, and the async
 *        force channels. Sized by the pool (see ensure_workspace);
 *        validated loudly before any work.
 */
struct SeqScratch {
  /** @brief Resolved dangling values (`n * kMaxInputs` entries). */
  std::span<LogicValue> resolved;
  /** @brief Combinational emission order (`n` slots). */
  std::span<Slot> comb_order;
  /** @brief Per-slot settled outputs (`n` entries). */
  std::span<LogicValue> gate_out;
  /** @brief Next-Q snapshots (`n` entries). */
  std::span<LogicValue> next_q;
  /** @brief Async-forced Q values (`n` entries; async path only). */
  std::span<LogicValue> forced_q;
  /** @brief Async-force flags (`n` entries; async path only). */
  std::span<std::uint8_t> force;
  /**
   * @brief Loud size validation before any work (engine-seam D3 rule).
   *
   * @param[in] n Addressable slot count the spans must cover.
   * @return kOk when every span holds at least `n` entries
   *         (`n * kMaxInputs` for resolved).
   */
  [[nodiscard]] core::Status validate(std::size_t n) const noexcept {
    if (resolved.size() < n * kMaxInputs || comb_order.size() < n || gate_out.size() < n || next_q.size() < n || forced_q.size() < n || force.size() < n) {
      return core::Status::failure(core::StatusCode::kInvalidArgument);
    }
    return core::Status::success();
  }
};

/**
 * @brief True when slot holds an attached flop's state.
 *
 * @param[in] st Pool to query.
 * @param[in] slot Netlist slot.
 * @return True when slot is in range and valid.
 */
[[nodiscard]] inline bool seq_has(const SeqState &st, Slot slot) noexcept {
  return slot < st.valid.size() && st.valid[slot] != 0;
}

/**
 * @brief Resolve a flop serial to its live slot.
 *
 * @param[in] st Pool to search (linear scan; flop counts are small).
 * @param[in] serial Stable serial to find.
 * @return Live slot holding serial; nullopt when absent (unknown or
 *         retired) — never a stale slot.
 */
[[nodiscard]] inline std::optional<Slot> find_seq_serial(const SeqState &st, std::uint64_t serial) noexcept {
  for (Slot s = 0; s < st.valid.size(); ++s) {
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
 * stepper may sync every step. Idempotent. No Netlist mutation. Grows
 * the state pool to the Netlist size first (pool size never exceeds
 * kMaxGates, so growth cannot fail); no noexcept since growth may
 * allocate.
 *
 * @param[out] st Pool to resync (grown to slot_count(nl) as needed).
 * @param[in] nl Netlist defining which slots want state.
 */
inline void init_seq_state(SeqState &st, const Netlist &nl) {
  const std::size_t n = slot_count(nl);
  // Infallible: n <= kMaxGates by pool invariant (add_gate is the only
  // growth path and refuses past the cap). Early-out keeps the
  // impossible failure loud-adjacent instead of indexing short vectors.
  if (!ensure_seq_state(st, n)) {
    return;
  }  for (Slot s = 0; s < n; ++s) {
    const Gate &g = nl.slots[s];
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
 * Element-wise fill, capacity preserved (no allocation): sizes stay as
 * they were, every entry reads detached/kLow/serial 0.
 *
 * @param[out] st Pool to clear.
 */
inline void reset_seq_state(SeqState &st) noexcept {
  for (std::size_t i = 0; i < st.q.size(); ++i) {
    st.q[i] = LogicValue::kLow;
  }
  for (std::size_t i = 0; i < st.valid.size(); ++i) {
    st.valid[i] = 0;
  }
  for (std::size_t i = 0; i < st.serial.size(); ++i) {
    st.serial[i] = 0;
  }
  st.next_serial = 0;
  st.live_count = 0;
}

/**
 * @brief True when any live sequential gate exists (the wire verb takes
 *        the sequential validation path only then; otherwise the old
 *        combinational path).
 *
 * @param[in] nl Netlist to scan.
 * @return True when at least one live DFF/DLATCH/JK/T/SRLATCH exists.
 */
[[nodiscard]] inline bool has_sequential_gates(const Netlist &nl) noexcept {
  for (Slot s = 0; s < slot_count(nl); ++s) {
    const Gate &g = nl.slots[s];
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
