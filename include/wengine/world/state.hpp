#pragma once

/**
 * @file
 * Central simulation state: the clock (sim_time/step_index), the
 * entity registry, and the per-domain SoA pools (particles, signals,
 * plus the singleton sequential logic pool), together with the
 * Snapshot deep-copy machinery every sink consumes. State owns
 * consistency: spawn_entity() touches the registry only,
 * despawn_entity() detaches every pool before releasing the slot, and
 * snapshots are independent copies — sinks never see live mutable
 * state. Consumed by solvers (in-place mutation), core::Runner
 * (ownership + emission), serialization, and the emit/link edges; this
 * header must never grow I/O or solver knowledge.
 */

// Central simulation state: clock (sim_time/step_index) + entity registry +
// per-domain SoA component pools (particles, signal sources; same shape for
// more). State owns consistency: spawn_entity() touches the registry only,
// despawn_entity() detaches every pool before releasing the slot.
//
// Snapshot semantics: deep copy. fill_snapshot() copies the clock plus the
// live particle arrays (dense, slot order) into a caller-owned Snapshot,
// reusing its vector capacities, so steady same-shape traffic performs no
// per-emit allocation after warmup (first fill grows to the live counts;
// later growth only when the live population exceeds the warmed capacity).
// take_snapshot() is the owning-copy convenience over fill_snapshot for
// callers that retain the result (replay buffers, tests, display grabs).
// Sinks never borrow state lifetime — safe for decimated, file, or future
// async emit. A zero-copy view can be added later if profiling demands it
// (see emit/sink.hpp).
//
// LIFETIME: the Runner fans one reusable Snapshot buffer out to its sinks
// synchronously (see core/runner.hpp). The reference handed to
// ISink::on_snapshot is valid only for the duration of that call: a sink
// that needs the data after returning takes an owned copy (take_snapshot,
// push_back, ...) or enqueues an owned block into a pooled queue (see
// link/output_queue.hpp) before returning — never retaining the reference.

#include "wengine/logic/netlist.hpp"
#include "wengine/logic/seq_state.hpp"
#include "wengine/logic/types.hpp"
#include "wengine/world/entity.hpp"
#include "wengine/world/particles.hpp"
#include "wengine/world/signals.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace wengine::world {

/**
 * @brief The live simulation state: clock, registry, pools, and the
 *        sequential logic pool.
 *
 * @note Mutated in place by solvers in slot order under the fixed-step
 *       contract; sim_time carries the end-of-step value so snapshots
 *       stamped at time T hold the state after step T. The sequential
 *       pool is a singleton (not entity-attached): despawn_entity never
 *       touches it, clear() resets all of it.
 */
struct State {
  /**
   * @brief Simulation clock in seconds (_s); advanced by one dt per
   *        accepted step (end-of-step value).
   */
  double sim_time{0.0};
  /**
   * @brief Number of completed fixed steps (u64; clock-range guards
   *        protect its exhaustion).
   */
  std::uint64_t step_index{0};
  /**
   * @brief Entity registry: slot identity and generations.
   */
  EntityRegistry entities;
  /**
   * @brief Point-particle SoA pool, entity-attached by slot.
   */
  Particles particles;
  /**
   * @brief Signal-source SoA pool, entity-attached by slot; owns the
   *        stable serial space.
   */
  Signals signals;
  /**
   * @brief Sequential pool, part 1 of 3: netlist topology (immutable
   *        during the run). Singleton, not entity-attached (research
   *        R1); despawn_entity never touches these, clear() resets
   *        all five members.
   */
  // Sequential pool (singleton, not entity-attached — research R1):
  // netlist topology (immutable during the run) + clocked Q state +
  // retained stimulus (interface order) + retained member list (empty =
  // all domains) + retained settled outputs (rewritten every step).
  // despawn_entity never touches these; clear() resets all five.
  logic::Netlist netlist;
  /**
   * @brief Sequential pool, part 2 of 3: clocked Q state with stable
   *        serials (never reused across recycle).
   */
  logic::SeqState seq;
  /**
   * @brief Sequential pool, part 3 of 3a: retained stimulus in
   *        block-interface order (0/1 values).
   */
  std::vector<logic::LogicValue> seq_inputs;
  /**
   * @brief Sequential pool, part 3 of 3b: retained member list (live
   *        edge-flop slots; empty = all domains).
   */
  std::vector<logic::Slot> seq_members;
  /**
   * @brief Sequential pool, part 3 of 3c: retained settled block
   *        outputs (rewritten every step, 0/1 values).
   */
  std::vector<logic::LogicValue> seq_outputs;

  /**
   * @brief Registry-only spawn; attach components (particles.attach(...))
   *        opt-in.
   *
   * @return a fresh Entity handle (deterministic LIFO slot reuse).
   */
  // Registry-only spawn; attach components (particles.attach(...)) opt-in.
  Entity spawn_entity() { return entities.spawn(); }

  /**
   * @brief Release an entity: detach every pool, then free the slot.
   *
   * @param[in] e handle to release.
   * @return true when e was live and is now released; false when the
   *         handle is stale or foreign (nothing changed).
   * @note The sequential pool is a singleton and is never touched
   *       here; pool columns keep their last values for reuse.
   */
  // False when `e` is stale/foreign. Detaches pools, then releases the slot.
  // The sequential pool is a singleton and is never touched here.
  bool despawn_entity(Entity e) {
    if (!entities.alive(e)) {
      return false;
    }
    particles.detach(e.index);
    signals.detach(e.index);
    return entities.despawn(e);
  }

  /**
   * @brief Reset the whole state to defaults: clock, registry, both
   *        entity pools, and all five sequential-pool members.
   */
  void clear() noexcept {
    sim_time = 0.0;
    step_index = 0;
    entities.clear();
    particles.clear();
    signals.clear();
    netlist = logic::Netlist{};
    seq = logic::SeqState{};
    seq_inputs.clear();
    seq_members.clear();
    seq_outputs.clear();
  }

  /**
   * @brief Whole-state equality against another state: clock, registry,
   *        all pools, and the sequential members (golden and
   *        round-trip tests).
   *
   * @return true when every member matches.
   */
  bool operator==(const State &) const noexcept = default;
};

/**
 * @brief An independent deep copy of the live state at one instant:
 *        clock plus dense live arrays in slot order.
 *
 * @note Snapshots never alias live mutable state — sinks receive them
 *       by reference only for the duration of one on_snapshot call and
 *       must copy (or enqueue an owned copy) to retain data. Dense
 *       arrays are in slot order, so identical states produce
 *       identical snapshots (deterministic emission).
 */
struct Snapshot {
  /**
   * @brief Simulation clock at snapshot time, in seconds (_s)
   *        (end-of-step value).
   */
  double sim_time{0.0};
  /**
   * @brief Completed step count at snapshot time.
   */
  std::uint64_t step_index{0};
  /**
   * @brief Dense X positions of live particles in slot order, in
   *        metres (_m).
   */
  // Dense copy of live particles in slot order (deterministic).
  std::vector<double> pos_x;
  /**
   * @brief Dense Y positions of live particles in slot order, in
   *        metres (_m).
   */
  std::vector<double> pos_y;
  /**
   * @brief Dense X velocities of live particles in slot order, in
   *        metres per second (_m/s).
   */
  std::vector<double> vel_x;
  /**
   * @brief Dense Y velocities of live particles in slot order, in
   *        metres per second (_m/s).
   */
  std::vector<double> vel_y;
  /**
   * @brief Stable particle identity in the same dense slot order:
   *        entity slot index per live particle. A surviving particle
   *        keeps its (index, generation) when earlier slots disappear;
   *        a recycled slot carries a bumped generation, so the pair
   *        never aliases a distinct object.
   *
   * @note fill_snapshot always populates both identity vectors (sizes
   *       == particle_count); manually built or legacy-decoded
   *       snapshots may leave them empty, which means UNAVAILABLE —
   *       never treat dense positions as stable IDs (see
   *       link/frame.hpp, emit/csv_sink.hpp).
   */
  // Stable particle identity in the same dense slot order: entity slot
  // index + generation per live particle (see world/entity.hpp). A
  // surviving particle keeps its (index, generation) when earlier slots
  // disappear; a recycled slot carries a bumped generation, so the pair
  // never aliases a distinct object. fill_snapshot always populates both
  // vectors (sizes == particle_count); manually built or legacy-decoded
  // snapshots may leave them empty, which means UNAVAILABLE — never treat
  // dense positions as stable IDs (see link/frame.hpp, emit/csv_sink.hpp).
  std::vector<std::uint32_t> particle_index;
  /**
   * @brief Generations matching particle_index (identity's second
   *        half).
   */
  std::vector<std::uint32_t> particle_generation;
  /**
   * @brief Dense stable serials of live signal sources in slot order.
   */
  // Dense copy of live signal sources in slot order (deterministic):
  // stable serial, SI value, and QuantityId tag per source.
  std::vector<std::uint64_t> sig_serial;
  /**
   * @brief Dense output values of live sources in slot order, SI base
   *        unit of each source's quantity.
   */
  std::vector<double> sig_value;
  /**
   * @brief Dense QuantityId tags matching sig_value (dimension of each
   *        sample).
   */
  std::vector<std::uint8_t> sig_quantity;
  /**
   * @brief Dense stable serials of live sequential flops in slot order;
   *        serials never reused across recycle. Plus settled block
   *        outputs in block_output_count order (0/1).
   */
  // Dense copy of live sequential flops in slot order (deterministic):
  // stable serial + Q (0/1) per flop, plus settled block outputs in
  // block_output_count order (0/1). Serials never reused across recycle.
  std::vector<std::uint64_t> seq_serial;
  /**
   * @brief Dense Q values (0/1) matching seq_serial.
   */
  std::vector<std::uint8_t> seq_value;
  /**
   * @brief Settled block outputs in block_output_count order (0/1).
   */
  std::vector<std::uint8_t> seq_output;

  /**
   * @brief Number of dense particle rows (== pos_x.size()).
   *
   * @return live particle count in the snapshot.
   */
  [[nodiscard]] std::size_t particle_count() const noexcept { return pos_x.size(); }
  /**
   * @brief Number of dense signal rows (== sig_value.size()).
   *
   * @return live source count in the snapshot.
   */
  [[nodiscard]] std::size_t signal_count() const noexcept { return sig_value.size(); }
  /**
   * @brief Number of dense sequential rows (== seq_value.size()).
   *
   * @return live flop count in the snapshot.
   */
  [[nodiscard]] std::size_t seq_count() const noexcept { return seq_value.size(); }
  /**
   * @brief True when particle identity is present (fill_snapshot
   *        output, or an empty snapshot vacuously). False means
   *        legacy/unavailable: dense order only, no stable IDs.
   *
   * @return whether particle_index/particle_generation carry usable
   *         identity.
   */
  // True when particle identity is present (fill_snapshot output, or an
  // empty snapshot vacuously). False means legacy/unavailable: dense order
  // only, no stable IDs.
  [[nodiscard]] bool has_particle_identity() const noexcept {
    return particle_index.size() == particle_count() && particle_generation.size() == particle_count();
  }

  /**
   * @brief Whole-snapshot equality against another snapshot: every
   *        dense array and the clock.
   *
   * @return true when all members match.
   */
  bool operator==(const Snapshot &) const noexcept = default;
};

/**
 * @brief Fill `out` from `s` (clocks + dense live arrays in slot order),
 *        reusing `out`'s capacities.
 *
 * @param[in] s source state; read-only, never mutated.
 * @param[out] out caller-owned snapshot to refill; all dense arrays are
 *             cleared and repopulated (existing capacities are reused,
 *             so steady same-shape traffic performs no per-emit
 *             allocation after warmup; growth allocates at most once
 *             per vector per fill).
 * @note Slot-order traversal is the determinism contract: identical
 *       states produce identical snapshots. Value-identical to
 *       take_snapshot. `clear()` retains capacity, so shrinking (despawn,
 *       zero live sources) never allocates either.
 */
// Fill `out` from `s` (clocks + dense live arrays in slot order),
// reusing `out`'s capacities: allocation-free once `out` has warmed to the
// live population; growth allocates at most once per vector per fill (twelve
// vectors total). `clear()` retains capacity, so shrinking (despawn, zero
// live sources) never allocates either. Value-identical to take_snapshot.
inline void fill_snapshot(const State &s, Snapshot &out) {
  out.sim_time = s.sim_time;
  out.step_index = s.step_index;
  out.pos_x.clear();
  out.pos_y.clear();
  out.vel_x.clear();
  out.vel_y.clear();
  out.particle_index.clear();
  out.particle_generation.clear();
  if (out.pos_x.capacity() < s.particles.live_count) {
    out.pos_x.reserve(s.particles.live_count);
  }
  if (out.pos_y.capacity() < s.particles.live_count) {
    out.pos_y.reserve(s.particles.live_count);
  }
  if (out.vel_x.capacity() < s.particles.live_count) {
    out.vel_x.reserve(s.particles.live_count);
  }
  if (out.vel_y.capacity() < s.particles.live_count) {
    out.vel_y.reserve(s.particles.live_count);
  }
  if (out.particle_index.capacity() < s.particles.live_count) {
    out.particle_index.reserve(s.particles.live_count);
  }
  if (out.particle_generation.capacity() < s.particles.live_count) {
    out.particle_generation.reserve(s.particles.live_count);
  }
  const auto &gens = s.entities.generations();
  for (std::uint32_t i = 0; i < s.particles.capacity(); ++i) {
    if (s.particles.has(i)) {
      out.pos_x.push_back(s.particles.pos_x[i]);
      out.pos_y.push_back(s.particles.pos_y[i]);
      out.vel_x.push_back(s.particles.vel_x[i]);
      out.vel_y.push_back(s.particles.vel_y[i]);
      out.particle_index.push_back(i);
      out.particle_generation.push_back(i < gens.size() ? gens[i] : 0);
    }
  }
  out.sig_serial.clear();
  out.sig_value.clear();
  out.sig_quantity.clear();
  if (out.sig_serial.capacity() < s.signals.live_count) {
    out.sig_serial.reserve(s.signals.live_count);
  }
  if (out.sig_value.capacity() < s.signals.live_count) {
    out.sig_value.reserve(s.signals.live_count);
  }
  if (out.sig_quantity.capacity() < s.signals.live_count) {
    out.sig_quantity.reserve(s.signals.live_count);
  }
  for (std::uint32_t i = 0; i < s.signals.capacity(); ++i) {
    if (s.signals.has(i)) {
      out.sig_serial.push_back(s.signals.serial[i]);
      out.sig_value.push_back(s.signals.value[i]);
      out.sig_quantity.push_back(s.signals.quantity[i]);
    }
  }
  out.seq_serial.clear();
  out.seq_value.clear();
  out.seq_output.clear();
  if (out.seq_serial.capacity() < s.seq.live_count) {
    out.seq_serial.reserve(s.seq.live_count);
  }
  if (out.seq_value.capacity() < s.seq.live_count) {
    out.seq_value.reserve(s.seq.live_count);
  }
  if (out.seq_output.capacity() < s.seq_outputs.size()) {
    out.seq_output.reserve(s.seq_outputs.size());
  }
  for (std::size_t i = 0; i < s.seq.valid.size(); ++i) {
    if (s.seq.valid[i] != 0) {
      out.seq_serial.push_back(s.seq.serial[i]);
      out.seq_value.push_back(static_cast<std::uint8_t>(s.seq.q[i]));
    }
  }
  for (const logic::LogicValue v : s.seq_outputs) {
    out.seq_output.push_back(static_cast<std::uint8_t>(v));
  }
}

/**
 * @brief Owning-copy convenience over fill_snapshot: take an
 *        independent snapshot to retain (replay buffers, tests,
 *        display grabs).
 *
 * @param[in] s state to copy; read-only.
 * @return a fresh Snapshot deep-copying the clock and every dense live
 *         array in slot order.
 * @note Allocation happens here, not on the Runner's reused emission
 *       buffer — sinks that retain data take this copy (or
 *       select_signals) inside on_snapshot and never hold the Runner's
 *       reference.
 */
[[nodiscard]] inline Snapshot take_snapshot(const State &s) {
  Snapshot snap;
  fill_snapshot(s, snap);
  return snap;
}

/**
 * @brief Subscription filter (SIG-5 routing) into a caller-owned
 *        Snapshot: copy of `s` carrying only the listed source serials,
 *        in snapshot (slot) order.
 *
 * @param[in] s source snapshot; particles, clock, and sequential arrays
 *            pass through whole (filtering is signal-scoped).
 * @param[in] serials allowlist of source serials to keep.
 * @param[out] out caller-owned snapshot to refill; `out` must not alias
 *             `s` (signal arrays are cleared before refilling).
 * @note Unknown or despawned serials are skipped silently on this
 *       path — subscribers validate loudly at subscribe time via their
 *       own counters (see emit/routing.hpp), while the per-snapshot
 *       path stays infallible and allocation-bounded like
 *       fill_snapshot. Value-identical to select_signals.
 */
// Subscription filter (SIG-5 routing) into a caller-owned Snapshot:
// copy of `s` carrying only the listed source serials, in snapshot (slot)
// order. Unknown or despawned serials are skipped — subscribers validate
// loudly at subscribe time via their own counters (see emit/routing.hpp),
// while the per-snapshot path stays infallible and allocation-bounded like
// fill_snapshot. Particles pass through whole (filtering is signal-scoped).
// `out` must not alias `s` (signal arrays are cleared before refilling).
// Value-identical to select_signals.
inline void select_signals_into(const Snapshot &s, std::span<const std::uint64_t> serials, Snapshot &out) {
  out.sim_time = s.sim_time;
  out.step_index = s.step_index;
  out.pos_x = s.pos_x;
  out.pos_y = s.pos_y;
  out.vel_x = s.vel_x;
  out.vel_y = s.vel_y;
  out.particle_index = s.particle_index;
  out.particle_generation = s.particle_generation;
  out.seq_serial = s.seq_serial;
  out.seq_value = s.seq_value;
  out.seq_output = s.seq_output;
  out.sig_serial.clear();
  out.sig_value.clear();
  out.sig_quantity.clear();
  if (out.sig_serial.capacity() < s.sig_serial.size()) {
    out.sig_serial.reserve(s.sig_serial.size());
  }
  if (out.sig_value.capacity() < s.sig_value.size()) {
    out.sig_value.reserve(s.sig_value.size());
  }
  if (out.sig_quantity.capacity() < s.sig_quantity.size()) {
    out.sig_quantity.reserve(s.sig_quantity.size());
  }
  for (std::size_t k = 0; k < s.signal_count(); ++k) {
    for (std::uint64_t want : serials) {
      if (s.sig_serial[k] == want) {
        out.sig_serial.push_back(s.sig_serial[k]);
        out.sig_value.push_back(s.sig_value[k]);
        out.sig_quantity.push_back(s.sig_quantity[k]);
        break;
      }
    }
  }
}

/**
 * @brief Subscription filter (SIG-5 routing): owning-copy convenience
 *        over select_signals_into for callers that retain the result.
 *
 * @param[in] s source snapshot; read-only.
 * @param[in] serials allowlist of source serials to keep.
 * @return a fresh Snapshot with the filtered signal arrays in snapshot
 *         (slot) order; particles and sequential data pass through
 *         whole.
 */
// Subscription filter (SIG-5 routing): copy of `s` carrying only the
// listed source serials, in snapshot (slot) order. Owning-copy convenience
// over select_signals_into for callers that retain the result.
[[nodiscard]] inline Snapshot select_signals(const Snapshot &s, std::span<const std::uint64_t> serials) {
  Snapshot out;
  select_signals_into(s, serials, out);
  return out;
}

} // namespace wengine::world
