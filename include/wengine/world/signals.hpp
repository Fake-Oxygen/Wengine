#pragma once

/**
 * @file
 * The second per-domain SoA component pool: stamped signal sources
 * (same parallel-vectors + occupancy shape as Particles), plus the
 * configure-time attach APIs that validate and mint sources with
 * stable serials. Owned by world::State, which detaches the pool on
 * despawn; snapshots carry live values in dense slot order and the
 * publish path adapts them into Frame channels. Serial identity, the
 * end-of-step timing convention, sum flattening, and the append-only
 * table/script stores are this file's contracts — see the block comments
 * below. This header must stay free of solver I/O and of any unit-text
 * knowledge: quantity parsing goes through cli::parse_quantity_value
 * against signal/types.hpp's kQuantities table.
 */

// Second per-domain SoA component pool: signal sources (same shape as
// Particles — parallel vectors + occupancy, indexed by entity slot).
// Owned by world::State, which keeps registry and pools consistent on
// despawn. Snapshot carries the live values in dense slot order; the
// publish path adapts them into Frame channels (one per source).
//
// IDENTITY (SIG-5): every source owns a stable `serial` (u64, monotonic
// per State, never reused — even across despawn/respawn). Slots recycle,
// serials don't, so subscriptions, CSV `sig_id`s, Frame channels, and the
// control plane address sources unambiguously. `find_serial` resolves a
// serial to its live slot (linear scan; signal counts are small).
//
// TIMING: `t0` is the source time base (spawn time, rebased on start).
// The solver evaluates `value(t_end - t0)` with `t_end = sim_time + dt`
// so the snapshot stamped at time T carries value(T) — the same
// end-of-step convention as the integrated particle positions.
// Stopped sources freeze: `value` keeps its last sample and the noise
// RNG does not advance.
//
// SUMS (SIG-5): `kSum` sources add operand sources' current values.
// Operands are serials (stable), validated at configure time against the
// live pool: each must resolve to a live NON-SUM source (sums compose only
// flattened — no sum-of-sum, no dangling refs, no self-reference since the
// new serial doesn't exist yet) AND carry the sum's own quantity
// (dimensional rule: QuantityId equality, generic over kQuantities — mixed
// U+I or a mismatched output tag fails before pool mutation). Operands evaluate in solver pass 1, sums
// in pass 2, so sums always read fresh values regardless of slot order.
// A later-despawned operand fails the step loudly (kInvalidArgument,
// deterministic) instead of reading a stale slot; a stopped operand
// contributes its frozen sample.
//
// EXPRESSIONS/TABLES (SIG-4a): custom generators evaluated in solver pass
// 1 like every other generator (same source-time base t = t_end - t0).
// Expression programs store inline per slot (fixed RPN, SoA columns
// expr_op/expr_arg/expr_len). Table points store in one append-only global
// store (table_times/table_values) with per-slot (table_off, table_len)
// ranges: slots recycle, store ranges never move, so references stay valid
// across despawn/respawn and serialization is one flat tail. Detach does
// not reclaim store rows (stability over compaction — same tradeoff as
// never-reused serials).

#include "wengine/core/status.hpp"
#include "wengine/signal/table.hpp"
#include "wengine/signal/types.hpp"
#include "wengine/world/entity.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace wengine::world {

/**
 * @brief SoA pool of signal sources: per-slot generator columns plus
 *        the shared append-only table and script stores.
 *
 * @note Slots recycle, serials never do: `next_serial` is monotonic
 *       per pool and every live source owns a serial from it, so
 *       subscriptions, CSV sig_ids, Frame channels, and the control
 *       plane address sources unambiguously across despawn/respawn.
 *       The hot path stays SI double + quantity tag (zero-cost);
 *       dimensional safety lives at configure/parse edges.
 */
struct Signals {
  /**
   * @brief Stable source identity per slot (u64, monotonic per pool,
   *        never reused — even across despawn/respawn).
   */
  std::vector<std::uint64_t> serial; // stable source id, never reused
  /**
   * @brief Generator kind per slot, a signal::SignalKind value as u8.
   */
  std::vector<std::uint8_t> kind; // signal::SignalKind
  /**
   * @brief Quantity tag per slot, a signal::QuantityId value as u8;
   *        fixes the dimension of amp/offset/value.
   */
  std::vector<std::uint8_t> quantity; // signal::QuantityId
  /**
   * @brief Frequency per slot in hertz (SI base 1/s).
   */
  std::vector<double> freq;
  /**
   * @brief Amplitude per slot, in the SI base unit of the slot's
   *        quantity.
   */
  std::vector<double> amp;
  /**
   * @brief DC offset per slot, in the SI base unit of the slot's
   *        quantity.
   */
  std::vector<double> offset;
  /**
   * @brief Phase per slot in radians.
   */
  std::vector<double> phase;
  /**
   * @brief Duty cycle per slot as a fraction in [0, 1] (square/pulse).
   */
  std::vector<double> duty;
  /**
   * @brief Spawn seed per slot (raw u64 bits; kept for replay clarity).
   */
  std::vector<std::uint64_t> seed; // spawn seed (replay clarity)
  /**
   * @brief Live noise RNG state per slot; advances once per step per
   *        live slot (never at configure time).
   */
  std::vector<std::uint64_t> rng; // live noise state
  /**
   * @brief Source time base per slot in seconds (_s): spawn time,
   *        rebased on start; the solver evaluates
   *        value(t_end - t0).
   */
  std::vector<double> t0;
  /**
   * @brief Current output per slot, in the SI base unit of the slot's
   *        quantity; frozen (not zeroed) while running == 0.
   */
  std::vector<double> value; // current output, SI base
  /**
   * @brief Stepping flag per slot: 1 = stepping, 0 = frozen (value
   *        keeps its last sample, noise RNG does not advance).
   */
  std::vector<std::uint8_t> running; // 1 = stepping, 0 = frozen
  /**
   * @brief Sum operand serials, kMaxOps per slot (flattened rows;
   *        slot i owns [i*kMaxOps, (i+1)*kMaxOps)).
   */
  std::vector<std::uint64_t> ops; // sum operand serials, kMaxOps per slot
  /**
   * @brief Number of live operands for kSum slots; 0 for every other
   *        kind.
   */
  std::vector<std::uint8_t> op_count;
  /**
   * @brief Expression RPN opcodes, kMaxExprNodes per slot.
   */
  std::vector<std::uint8_t> expr_op; // expression RPN opcodes, kMaxExprNodes/slot
  /**
   * @brief Expression kConst payloads, kMaxExprNodes per slot.
   */
  std::vector<double> expr_arg; // expression kConst payloads, kMaxExprNodes/slot
  /**
   * @brief Expression program length per slot (0 for non-expression
   *        slots).
   */
  std::vector<std::uint8_t> expr_len; // expression program length per slot
  /**
   * @brief Table range start per slot into table_times/table_values.
   */
  std::vector<std::uint64_t> table_off; // table range start in the store
  /**
   * @brief Table range length per slot (0 for non-table slots).
   */
  std::vector<std::uint64_t> table_len; // table range length per slot
  /**
   * @brief Append-only global table store: time points in seconds
   *        (_s), strictly increasing within each range.
   */
  std::vector<double> table_times; // append-only global table store (times)
  /**
   * @brief Append-only global table store: values in the SI base unit
   *        of the referencing slot's quantity.
   */
  std::vector<double> table_values; // append-only global table store (values)
  /**
   * @brief Script range start per slot into lua_text.
   */
  std::vector<std::uint64_t> lua_off; // script range start in lua_text
  /**
   * @brief Script byte length per slot (0 for non-Lua slots).
   */
  std::vector<std::uint64_t> lua_len; // script byte length per slot
  /**
   * @brief Append-only global script store (SIG-4b): opaque bytes to
   *        the engine, compiled only where Lua exists.
   */
  std::string lua_text; // append-only global script store (SIG-4b)
  /**
   * @brief Occupancy column: 1 = live source in the slot.
   */
  std::vector<std::uint8_t> present; // 1 = live source in slot
  /**
   * @brief Number of present slots (exact, maintained by
   *        attach()/detach()).
   */
  std::size_t live_count{0};
  /**
   * @brief Next serial to hand out; monotonic, never decremented, never
   *        reused (including across clear()).
   */
  std::uint64_t next_serial{0};

  /**
   * @brief Number of allocated slots (present.size()).
   *
   * @return slots addressable by ensure()/attach() today.
   */
  [[nodiscard]] std::size_t capacity() const noexcept { return present.size(); }

  /**
   * @brief Occupancy predicate.
   *
   * @param[in] index slot to test.
   * @return true when the index is in range and the slot is present.
   */
  [[nodiscard]] bool has(std::uint32_t index) const noexcept {
    return index < present.size() && present[index] != 0;
  }

  /**
   * @brief Grow arrays so `index` is addressable. New slots are absent
   *        and zeroed (including the per-slot operand and expression
   *        rows).
   *
   * @param[in] index slot that must become addressable.
   * @note Setup-path allocation (not the hot step); every SoA column
   *       and both per-slot row groups (ops, expr) resize together so
   *       flat indexing stays aligned.
   */
  // Grow arrays so `index` is addressable. New slots are absent and zeroed.
  void ensure(std::uint32_t index) {
    const std::size_t need = static_cast<std::size_t>(index) + 1;
    if (need <= present.size()) {
      return;
    }
    const std::size_t n_ops =
        need * signal::SignalParams::kMaxOps - ops.size();
    const std::size_t n_expr = need * signal::kMaxExprNodes - expr_op.size();
    serial.resize(need, 0);
    kind.resize(need, 0);
    quantity.resize(need, 0);
    freq.resize(need, 0.0);
    amp.resize(need, 0.0);
    offset.resize(need, 0.0);
    phase.resize(need, 0.0);
    duty.resize(need, 0.0);
    seed.resize(need, 0);
    rng.resize(need, 0);
    t0.resize(need, 0.0);
    value.resize(need, 0.0);
    running.resize(need, 0);
    ops.resize(ops.size() + n_ops, 0);
    op_count.resize(need, 0);
    expr_op.resize(expr_op.size() + n_expr, 0);
    expr_arg.resize(expr_arg.size() + n_expr, 0.0);
    expr_len.resize(need, 0);
    table_off.resize(need, 0);
    table_len.resize(need, 0);
    lua_off.resize(need, 0);
    lua_len.resize(need, 0);
    present.resize(need, 0);
  }

  /**
   * @brief Attach a live source to `index` (idempotent; keeps existing
   *        columns).
   *
   * @param[in] index slot to mark present.
   * @note The caller (configure_source / attach_table_source /
   *       attach_lua_source) assigns the serial and columns after
   *       attach; attach only flips presence and maintains live_count.
   */
  // Attach a live source to `index` (idempotent; keeps existing data).
  void attach(std::uint32_t index) {
    ensure(index);
    if (present[index] == 0) {
      present[index] = 1;
      ++live_count;
    }
  }

  /**
   * @brief Detach (idempotent no-op when absent). Capacity is retained.
   *
   * @param[in] index slot to clear.
   * @note Clears presence only: all other columns retain their last
   *       live values verbatim (serialization stores them; every
   *       configure path overwrites them on reuse), and the serial is
   *       never reused — next_serial does not rewind.
   */
  // Detach (idempotent no-op when absent). Capacity is retained.
  void detach(std::uint32_t index) noexcept {
    if (index < present.size() && present[index] != 0) {
      present[index] = 0;
      --live_count;
    }
  }

  /**
   * @brief Reset the pool completely: all columns, both shared stores,
   *        live count, and next_serial (serials restart from 0 in a
   *        fresh pool — clear means a new identity space).
   */
  void clear() noexcept {
    serial.clear();
    kind.clear();
    quantity.clear();
    freq.clear();
    amp.clear();
    offset.clear();
    phase.clear();
    duty.clear();
    seed.clear();
    rng.clear();
    t0.clear();
    value.clear();
    running.clear();
    ops.clear();
    op_count.clear();
    expr_op.clear();
    expr_arg.clear();
    expr_len.clear();
    table_off.clear();
    table_len.clear();
    table_times.clear();
    table_values.clear();
    lua_off.clear();
    lua_len.clear();
    lua_text.clear();
    present.clear();
    live_count = 0;
    next_serial = 0;
  }

  /**
   * @brief Whole-pool equality against another pool: every column,
   *        both stores, and the counters.
   *
   * @return true when all columns match (used by round-trip tests).
   */
  bool operator==(const Signals &) const noexcept = default;
};

/**
 * @brief Resolve a source serial to its live slot.
 *
 * @param[in] pool pool to scan.
 * @param[in] serial stable identity to look up.
 * @return the slot index holding the live source with this serial;
 *         nullopt when the serial is unknown or despawned —
 *         subscribers treat that as "source gone", never as slot 0.
 * @note Linear scan over the pool (signal counts are small); pass order
 *       is slot order, so resolution is deterministic.
 */
// Resolve a source serial to its live slot. Absent (unknown or despawned)
// yields nullopt — subscribers treat that as "source gone", never as slot 0.
[[nodiscard]] inline std::optional<std::uint32_t> find_serial(const Signals &pool,
                                                              std::uint64_t serial) noexcept {
  for (std::uint32_t i = 0; i < pool.capacity(); ++i) {
    if (pool.has(i) && pool.serial[i] == serial) {
      return i;
    }
  }
  return std::nullopt;
}

/**
 * @brief Configure entity `e` as a signal source (attach + params +
 *        time base).
 *
 * @param[in] e entity whose .index addresses the pool slot; the pool
 *            is slot-indexed and State keeps registry and pools
 *            consistent.
 * @param[in, out] pool pool to configure into; columns of slot e.index
 *                    are (over)written and a fresh serial is consumed.
 * @param[in] p validated shape: kind, quantity tag, freq in hertz
 *            (freq_hz), amplitude/offset in the quantity's SI base
 *            unit, phase in radians, duty in [0,1], seed bits, and for
 *            kSum the flattened operand serials (op_count entries of
 *            p.ops) with the sum's own quantity.
 * @param[in] t_now current simulation time in seconds (_s); becomes
 *            the slot's time base t0 (evaluations use t_end - t0).
 * @return kOk on attach; kInvalidArgument when the params fail
 *         validate_params, when the kind is kTable or kLua (rejected
 *         here by design — use configure_table_source or
 *         lua::configure_lua_source), when a kSum operand serial is
 *         unknown/despawned, resolves to another kSum (sums stay
 *         flattened), carries a quantity different from the sum's
 *         own, or when a kExpression program already fails at t = 0.
 * @note On failure nothing is attached and no serial is consumed —
 *       the pool is untouched. Initial sample is eval(0): periodic
 *       kinds start at phase, noise starts at the seed peek (the RNG
 *       advances on the first step, never here), sums start at the
 *       operands' current sum, expressions evaluate at t = 0 — so
 *       spawn is a pure function of (params, pool, t_now).
 */
// Configure entity `e` as a signal source (attach + params + time base).
// Validates shape (validate_params) and, for sums, operand serials against
// the live pool; on failure nothing is attached and the pool is untouched.
// Initial sample is eval(0): periodic kinds start at phase, noise starts
// at the seed peek (RNG advances on the first step, never here), sums
// start at the operands' current sum, expressions evaluate at t = 0 — so
// spawn is a pure function of (params, pool, t_now). kTable and kLua are
// rejected here by design (points/scripts live outside SignalParams — see
// configure_table_source and lua::configure_lua_source); every other kind
// zeroes both ranges.
[[nodiscard]] inline core::Status configure_source(Entity e, Signals &pool,
                                                   const signal::SignalParams &p,
                                                   double t_now) {
  if (!signal::validate_params(p).ok()) {
    return {core::StatusCode::kInvalidArgument};
  }
  if (p.kind == signal::SignalKind::kTable || p.kind == signal::SignalKind::kLua) {
    return {core::StatusCode::kInvalidArgument}; // needs points/script: see below
  }
  if (p.kind == signal::SignalKind::kSum) {
    for (std::uint8_t k = 0; k < p.op_count; ++k) {
      const auto slot = find_serial(pool, p.ops[static_cast<std::size_t>(k)]);
      if (!slot.has_value()) {
        return {core::StatusCode::kInvalidArgument}; // unknown/despawned
      }
      if (pool.kind[*slot] == static_cast<std::uint8_t>(signal::SignalKind::kSum)) {
        return {core::StatusCode::kInvalidArgument}; // sums stay flattened
      }
      // Dimensional rule: every operand must carry the sum's own quantity
      // (QuantityId equality only — generic over kQuantities, no per-unit
      // switch). Mixed U+I or a mismatched output tag fails pre-mutation.
      const auto op_q = static_cast<signal::QuantityId>(pool.quantity[*slot]);
      if (!signal::is_valid_quantity(op_q) || op_q != p.quantity) {
        return {core::StatusCode::kInvalidArgument};
      }
    }
  }
  double initial = 0.0;
  if (p.kind == signal::SignalKind::kExpression) {
    const auto r = signal::eval_expression(p.expr, 0.0);
    if (!r.ok()) {
      return {core::StatusCode::kInvalidArgument}; // fails already at t = 0
    }
    initial = r.value;
  }
  pool.attach(e.index);
  const std::uint32_t i = e.index;
  const std::uint64_t serial = pool.next_serial++;
  pool.serial[i] = serial;
  pool.kind[i] = static_cast<std::uint8_t>(p.kind);
  pool.quantity[i] = static_cast<std::uint8_t>(p.quantity);
  pool.freq[i] = p.freq_hz;
  pool.amp[i] = p.amp;
  pool.offset[i] = p.offset;
  pool.phase[i] = p.phase_rad;
  pool.duty[i] = p.duty;
  pool.seed[i] = p.seed;
  pool.rng[i] = p.seed;
  pool.t0[i] = t_now;
  pool.running[i] = 1;
  pool.op_count[i] = (p.kind == signal::SignalKind::kSum) ? p.op_count : 0;
  for (std::size_t k = 0; k < signal::SignalParams::kMaxOps; ++k) {
    pool.ops[i * signal::SignalParams::kMaxOps + k] =
        (p.kind == signal::SignalKind::kSum && k < p.op_count)
            ? p.ops[k]
            : 0;
  }
  pool.expr_len[i] = (p.kind == signal::SignalKind::kExpression) ? p.expr.size : 0;
  for (std::size_t k = 0; k < signal::kMaxExprNodes; ++k) {
    const bool live =
        p.kind == signal::SignalKind::kExpression && k < p.expr.size;
    pool.expr_op[i * signal::kMaxExprNodes + k] =
        live ? static_cast<std::uint8_t>(p.expr.nodes[k].op) : 0;
    pool.expr_arg[i * signal::kMaxExprNodes + k] =
        live ? p.expr.nodes[k].value : 0.0;
  }
  pool.table_off[i] = 0;
  pool.table_len[i] = 0;
  pool.lua_off[i] = 0;
  pool.lua_len[i] = 0;
  if (p.kind == signal::SignalKind::kSum) {
    double acc = p.offset;
    for (std::uint8_t k = 0; k < p.op_count; ++k) {
      // Validated above: every operand resolves to a live non-sum source.
      acc += pool.value[*find_serial(pool, p.ops[static_cast<std::size_t>(k)])];
    }
    pool.value[i] = acc;
  } else if (p.kind == signal::SignalKind::kExpression) {
    pool.value[i] = initial;
  } else {
    pool.value[i] =
        signal::eval_signal(p, 0.0, signal::u01_from_bits(signal::rng_next(p.seed)));
  }
  return core::Status::success();
}

/**
 * @brief Append validated table points to the pool's shared store (no
 *        attach). Shared by single-source configure and batch spawns
 *        so N identical sources reference one range instead of N
 *        duplicate copies.
 *
 * @param[in, out] pool pool whose table_times/table_values grow.
 * @param[in] pts points to append: times in seconds (_s), finite and
 *            strictly increasing; values in the quantity's SI base
 *            unit.
 * @param[out] off receives the range start of the appended points.
 * @return kOk after appending; kInvalidArgument when the points fail
 *         validate_table_points (nothing appended, off untouched).
 * @note The store is append-only: ranges never move, so references stay
 *       valid across despawn/respawn, and detach never reclaims rows.
 */
// Append validated table points to the pool store (no attach). Shared by
// single-source configure and batch spawns so N identical sources reference
// one range instead of N duplicate copies. `off` receives the range start.
[[nodiscard]] inline core::Status append_table_points(Signals &pool,
                                                     std::span<const signal::TablePoint> pts,
                                                     std::uint64_t &off) {
  if (!signal::validate_table_points(pts).ok()) {
    return {core::StatusCode::kInvalidArgument};
  }
  off = static_cast<std::uint64_t>(pool.table_times.size());
  for (const signal::TablePoint &pt : pts) {
    pool.table_times.push_back(pt.t);
    pool.table_values.push_back(pt.v);
  }
  return core::Status::success();
}

/**
 * @brief Attach entity `e` as a table source over the already-stored
 *        range [off, off + len) (from append_table_points).
 *
 * @param[in] e entity whose .index addresses the pool slot.
 * @param[in, out] pool pool to attach into; consumes a fresh serial and
 *                    writes the slot's columns.
 * @param[in] off range start in table_times/table_values.
 * @param[in] len range length (1..kMaxTablePoints).
 * @param[in] quantity QuantityId fixing the dimension of the stored
 *            values and the slot's output.
 * @param[in] t_now current simulation time in seconds (_s); becomes
 *            the slot's time base t0.
 * @return kOk on attach; kInvalidArgument when the range is empty or
 *         oversized, lies outside the store, or the quantity is not a
 *         valid QuantityId (nothing attached).
 * @note The initial sample is the table value at t = 0 (clamped when
 *       the table starts later).
 */
// Attach entity `e` as a table source over the already-stored range
// [off, off + len) (from append_table_points). Initial sample is the
// table value at t = 0 (clamped when the table starts later).
[[nodiscard]] inline core::Status attach_table_source(Entity e, Signals &pool,
                                                     std::uint64_t off, std::uint64_t len,
                                                     signal::QuantityId quantity,
                                                     double t_now) {
  if (len == 0 || len > signal::kMaxTablePoints || off > pool.table_times.size() ||
      len > pool.table_times.size() - off || pool.table_values.size() < pool.table_times.size() || !signal::is_valid_quantity(quantity)) {
    return {core::StatusCode::kInvalidArgument};
  }
  pool.attach(e.index);
  const std::uint32_t i = e.index;
  pool.serial[i] = pool.next_serial++;
  pool.kind[i] = static_cast<std::uint8_t>(signal::SignalKind::kTable);
  pool.quantity[i] = static_cast<std::uint8_t>(quantity);
  pool.freq[i] = 0.0;
  pool.amp[i] = 0.0;
  pool.offset[i] = 0.0;
  pool.phase[i] = 0.0;
  pool.duty[i] = 0.0;
  pool.seed[i] = 0;
  pool.rng[i] = 0;
  pool.t0[i] = t_now;
  pool.running[i] = 1;
  pool.op_count[i] = 0;
  for (std::size_t k = 0; k < signal::SignalParams::kMaxOps; ++k) {
    pool.ops[i * signal::SignalParams::kMaxOps + k] = 0;
  }
  pool.expr_len[i] = 0;
  for (std::size_t k = 0; k < signal::kMaxExprNodes; ++k) {
    pool.expr_op[i * signal::kMaxExprNodes + k] = 0;
    pool.expr_arg[i * signal::kMaxExprNodes + k] = 0.0;
  }
  pool.table_off[i] = off;
  pool.table_len[i] = len;
  pool.lua_off[i] = 0;
  pool.lua_len[i] = 0;
  const std::span<const double> times(pool.table_times.data() + off,
                                      static_cast<std::size_t>(len));
  const std::span<const double> values(pool.table_values.data() + off,
                                       static_cast<std::size_t>(len));
  pool.value[i] = signal::eval_table(times, values, 0.0);
  return core::Status::success();
}

/**
 * @brief Configure entity `e` as a table-replay source: validates
 *        points, appends them once to the shared store, and attaches
 *        over the new range.
 *
 * @param[in] e entity whose .index addresses the pool slot.
 * @param[in, out] pool pool that grows by the appended range and gains
 *                    the live source.
 * @param[in] pts points to append (times in seconds (_s), finite,
 *            strictly increasing; values in the quantity's SI base
 *            unit).
 * @param[in] quantity QuantityId fixing the source's dimension.
 * @param[in] t_now current simulation time in seconds (_s).
 * @return kOk when points validated and the source attached;
 *         kInvalidArgument on invalid points or range (nothing
 *         attached; appended points from a failed attach remain in the
 *         store — ranges are inert until referenced).
 */
// Configure entity `e` as a table-replay source: validates points,
// appends them once, and attaches over the new range.
[[nodiscard]] inline core::Status
configure_table_source(Entity e, Signals &pool, std::span<const signal::TablePoint> pts,
                       signal::QuantityId quantity, double t_now) {
  std::uint64_t off = 0;
  if (!append_table_points(pool, pts, off).ok()) {
    return {core::StatusCode::kInvalidArgument};
  }
  return attach_table_source(e, pool, off, static_cast<std::uint64_t>(pts.size()),
                             quantity, t_now);
}

/**
 * @brief Append a validated Lua script to the pool's shared store (no
 *        attach).
 *
 * @param[in, out] pool pool whose lua_text grows by the script bytes.
 * @param[in] script opaque script bytes; the engine never compiles
 *            them here (compile-checked where Lua exists:
 *            lua::configure_lua_source, the CLI) — only the size is
 *            bounded so pools, batch sections, and blobs stay finite.
 * @param[out] off receives the range start of the appended script.
 * @return kOk after appending; kInvalidArgument when the script is
 *         empty or exceeds kMaxLuaScriptBytes (nothing appended).
 * @note The store is append-only; ranges never move, and detach never
 *       reclaims bytes.
 */
// Append a validated Lua script to the pool store (no attach). Script text
// is opaque bytes to the engine (compile-checked where Lua exists:
// lua::configure_lua_source, the CLI); the engine only bounds the size so
// pools, batch sections, and blobs stay finite. `off` receives the start.
[[nodiscard]] inline core::Status append_lua_script(Signals &pool,
                                                   std::string_view script,
                                                   std::uint64_t &off) {
  if (script.empty() || script.size() > signal::kMaxLuaScriptBytes) {
    return {core::StatusCode::kInvalidArgument};
  }
  off = static_cast<std::uint64_t>(pool.lua_text.size());
  pool.lua_text.append(script.data(), script.size());
  return core::Status::success();
}

/**
 * @brief Attach entity `e` as a Lua source over the already-stored
 *        range [off, off + len) (from append_lua_script).
 *
 * @param[in] e entity whose .index addresses the pool slot.
 * @param[in, out] pool pool to attach into; consumes a fresh serial and
 *                    writes the slot's columns.
 * @param[in] off range start in lua_text.
 * @param[in] len script byte length (1..kMaxLuaScriptBytes).
 * @param[in] quantity QuantityId fixing the source's dimension.
 * @param[in] t_now current simulation time in seconds (_s); becomes
 *            the slot's time base t0.
 * @return kOk on attach; kInvalidArgument when the range is empty or
 *         oversized, lies outside the store, or the quantity is not a
 *         valid QuantityId (nothing attached).
 * @note The initial sample is 0: Lua sources sample on their first
 *       stepped evaluation (the Lua solver runs before SignalSolver,
 *       so sums over Lua operands still read fresh values within the
 *       step). Batch spawns and direct configures agree on this —
 *       run_headless always steps at least once, so the 0 is never
 *       observed there; direct API users read it only before the
 *       first step.
 */
// Attach entity `e` as a Lua source over the already-stored range
// [off, off + len) (from append_lua_script). The initial sample is 0: Lua
// sources sample on their first stepped evaluation (the Lua solver runs
// before SignalSolver, so sums over Lua operands still read fresh values
// within the step). Batch spawns and direct configures agree on this —
// run_headless always steps at least once, so the 0 is never observed
// there; direct API users read it only before the first step.
[[nodiscard]] inline core::Status attach_lua_source(Entity e, Signals &pool, std::uint64_t off, std::uint64_t len, signal::QuantityId quantity,
                                                   double t_now) {
  if (len == 0 || len > signal::kMaxLuaScriptBytes || off > pool.lua_text.size() ||
      len > pool.lua_text.size() - off || !signal::is_valid_quantity(quantity)) {
    return {core::StatusCode::kInvalidArgument};
  }
  pool.attach(e.index);
  const std::uint32_t i = e.index;
  pool.serial[i] = pool.next_serial++;
  pool.kind[i] = static_cast<std::uint8_t>(signal::SignalKind::kLua);
  pool.quantity[i] = static_cast<std::uint8_t>(quantity);
  pool.freq[i] = 0.0;
  pool.amp[i] = 0.0;
  pool.offset[i] = 0.0;
  pool.phase[i] = 0.0;
  pool.duty[i] = 0.0;
  pool.seed[i] = 0;
  pool.rng[i] = 0;
  pool.t0[i] = t_now;
  pool.running[i] = 1;
  pool.op_count[i] = 0;
  for (std::size_t k = 0; k < signal::SignalParams::kMaxOps; ++k) {
    pool.ops[i * signal::SignalParams::kMaxOps + k] = 0;
  }
  pool.expr_len[i] = 0;
  for (std::size_t k = 0; k < signal::kMaxExprNodes; ++k) {
    pool.expr_op[i * signal::kMaxExprNodes + k] = 0;
    pool.expr_arg[i * signal::kMaxExprNodes + k] = 0.0;
  }
  pool.table_off[i] = 0;
  pool.table_len[i] = 0;
  pool.lua_off[i] = off;
  pool.lua_len[i] = len;
  pool.value[i] = 0.0;
  return core::Status::success();
}

} // namespace wengine::world
