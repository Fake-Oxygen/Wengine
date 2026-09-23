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

#include "wengine/core/status.hpp"
#include "wengine/signal/types.hpp"
#include "wengine/world/entity.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
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
// start at the operands' current sum — so spawn is a pure function of
// (params, pool, t_now).
[[nodiscard]] inline core::Status configure_source(Entity e, Signals &pool,
                                                   const signal::SignalParams &p,
                                                   double t_now) {
  if (!signal::validate_params(p).ok()) {
    return {core::StatusCode::kInvalidArgument};
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
  if (p.kind == signal::SignalKind::kSum) {
    double acc = p.offset;
    for (std::uint8_t k = 0; k < p.op_count; ++k) {
      // Validated above: every operand resolves to a live non-sum source.
      acc += pool.value[*find_serial(pool, p.ops[static_cast<std::size_t>(k)])];
    }
    pool.value[i] = acc;
  } else {
    pool.value[i] =
        signal::eval_signal(p, 0.0, signal::u01_from_bits(signal::rng_next(p.seed)));
  }
  return core::Status::success();
}

} // namespace wengine::world
