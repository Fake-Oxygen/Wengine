#pragma once

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
// new serial doesn't exist yet). Operands evaluate in solver pass 1, sums
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

struct Signals {
  std::vector<std::uint64_t> serial; // stable source id, never reused
  std::vector<std::uint8_t> kind; // signal::SignalKind
  std::vector<std::uint8_t> quantity; // signal::QuantityId
  std::vector<double> freq;
  std::vector<double> amp;
  std::vector<double> offset;
  std::vector<double> phase;
  std::vector<double> duty;
  std::vector<std::uint64_t> seed; // spawn seed (replay clarity)
  std::vector<std::uint64_t> rng; // live noise state
  std::vector<double> t0;
  std::vector<double> value; // current output, SI base
  std::vector<std::uint8_t> running; // 1 = stepping, 0 = frozen
  std::vector<std::uint64_t> ops; // sum operand serials, kMaxOps per slot
  std::vector<std::uint8_t> op_count;
  std::vector<std::uint8_t> expr_op; // expression RPN opcodes, kMaxExprNodes/slot
  std::vector<double> expr_arg; // expression kConst payloads, kMaxExprNodes/slot
  std::vector<std::uint8_t> expr_len; // expression program length per slot
  std::vector<std::uint64_t> table_off; // table range start in the store
  std::vector<std::uint64_t> table_len; // table range length per slot
  std::vector<double> table_times; // append-only global table store (times)
  std::vector<double> table_values; // append-only global table store (values)
  std::vector<std::uint64_t> lua_off; // script range start in lua_text
  std::vector<std::uint64_t> lua_len; // script byte length per slot
  std::string lua_text; // append-only global script store (SIG-4b)
  std::vector<std::uint8_t> present; // 1 = live source in slot
  std::size_t live_count{0};
  std::uint64_t next_serial{0};

  [[nodiscard]] std::size_t capacity() const noexcept { return present.size(); }

  [[nodiscard]] bool has(std::uint32_t index) const noexcept {
    return index < present.size() && present[index] != 0;
  }

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

  // Attach a live source to `index` (idempotent; keeps existing data).
  void attach(std::uint32_t index) {
    ensure(index);
    if (present[index] == 0) {
      present[index] = 1;
      ++live_count;
    }
  }

  // Detach (idempotent no-op when absent). Capacity is retained.
  void detach(std::uint32_t index) noexcept {
    if (index < present.size() && present[index] != 0) {
      present[index] = 0;
      --live_count;
    }
  }

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

  bool operator==(const Signals &) const noexcept = default;
};

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

// Attach entity `e` as a table source over the already-stored range
// [off, off + len) (from append_table_points). Initial sample is the
// table value at t = 0 (clamped when the table starts later).
[[nodiscard]] inline core::Status attach_table_source(Entity e, Signals &pool,
                                                     std::uint64_t off, std::uint64_t len,
                                                     signal::QuantityId quantity,
                                                     double t_now) {
  if (len == 0 || len > signal::kMaxTablePoints || off > pool.table_times.size() ||
      len > pool.table_times.size() - off ||
      pool.table_values.size() < pool.table_times.size()) {
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

// Attach entity `e` as a Lua source over the already-stored range
// [off, off + len) (from append_lua_script). The initial sample is 0: Lua
// sources sample on their first stepped evaluation (the Lua solver runs
// before SignalSolver, so sums over Lua operands still read fresh values
// within the step). Batch spawns and direct configures agree on this —
// run_headless always steps at least once, so the 0 is never observed
// there; direct API users read it only before the first step.
[[nodiscard]] inline core::Status attach_lua_source(Entity e, Signals &pool,
                                                   std::uint64_t off, std::uint64_t len,
                                                   signal::QuantityId quantity,
                                                   double t_now) {
  if (len == 0 || len > signal::kMaxLuaScriptBytes || off > pool.lua_text.size() ||
      len > pool.lua_text.size() - off) {
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
