#pragma once

// Second per-domain SoA component pool: signal sources (same shape as
// Particles — parallel vectors + occupancy, indexed by entity slot).
// Owned by world::State, which keeps registry and pools consistent on
// despawn. Snapshot carries the live values in dense slot order; the
// publish path adapts them into Frame channels (one per source).
//
// Timing: `t0` is the source time base (spawn time, rebased on start).
// The solver evaluates `value(t_end - t0)` with `t_end = sim_time + dt`
// so the snapshot stamped at time T carries value(T) — the same
// end-of-step convention as the integrated particle positions.
// Stopped sources freeze: `value` keeps its last sample and the noise
// RNG does not advance.

#include "wengine/signal/types.hpp"
#include "wengine/world/entity.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace wengine::world {

struct Signals {
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
  std::vector<std::uint8_t> present; // 1 = live source in slot
  std::size_t live_count{0};

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
    present.clear();
    live_count = 0;
  }

  bool operator==(const Signals &) const noexcept = default;
};

// Configure entity `e` as a signal source (attach + params + time base).
// Initial sample is eval(0): periodic kinds start at phase, noise starts
// at the seed peek (RNG advances on the first step, never here), so the
// spawn itself is a pure function of (params, t_now).
inline void configure_source(Entity e, Signals &pool, const signal::SignalParams &p,
                             double t_now) {
  pool.attach(e.index);
  const std::uint32_t i = e.index;
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
  pool.value[i] = signal::eval_signal(p, 0.0, signal::u01_from_bits(signal::rng_next(p.seed)));
}

} // namespace wengine::world
