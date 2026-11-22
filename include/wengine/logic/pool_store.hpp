#pragma once

/**
 * @file
 * PoolStore seam of the logic pipeline: the single module behind every
 * derived + workspace store hanging off Netlist.
 *
 * Problem (SCALE2): Netlist exposes every array as public data, so one
 * new derived array touches ensure_slots + ensure_workspace + reset +
 * adjacency_clear/rebuild + free_heap_rebuild + pool_bytes + the equality
 * tail + serialization. The interface is the whole struct — low depth,
 * low locality. optimize.hpp:372,375 proved it with a duplicated
 * adjacency_rebuild.
 *
 * Rule after this header:
 * - Netlist keeps only truth (slots, counters, driver_inv, tie_rep,
 *   shared_*, pull_cfg, live_count, dirty, topo_order/level).
 * - Every derived store (succ_* chains, tie_head/next chains, free_heap,
 *   feeds_shared) plus every ws_* scratch array grows/clears/rebuilds
 *   only through this module: grow / clear_derived / rebuild_all /
 *   ensure_scratch.
 * - Incremental mutation paths funnel the dirty mark through
 *   note_edge_event(); bulk writers (loaders, O2 fresh builds) write
 *   truth tables directly and call rebuild_all() exactly once.
 * - rebuild_all() == incremental maintenance, exactly: the adjacency
 *   corpus asserts this (see test_logic_adjacency).
 *
 * Dependency rules (docs/logic.md section 4): header-only,
 * allocation-free except where noted; depends only on types.hpp,
 * shared_net.hpp, adjacency.hpp, tie_set.hpp (never netlist.hpp —
 * the seam sits below mutation, so no include cycle). Consumers:
 * netlist.hpp (feeds forwarders + reset), the YAML/binary loaders,
 * the O2 fresh builds, benchmarks (bytes), and tests.
 */

#include "wengine/logic/adjacency.hpp"
#include "wengine/logic/shared_net.hpp"
#include "wengine/logic/tie_set.hpp"
#include "wengine/logic/types.hpp"

#include <cstddef>
#include <cstdint>

namespace wengine::logic::pool_store {

namespace detail {

// --- Shared-feeder counters (moved from netlist.hpp; owned here with the
// rest of derived state) -----------------------------------------------
// feeds_shared[driver] counts the driver's live edges landing on
// shared-marked inputs. Invariant: exact on every valid net; bulk writers
// recompute via recompute_feeds below.

/**
 * @brief Decrement marked-input drivers of one sink (before its rewrite).
 *
 * @param[out] nl Netlist holding feeder counters.
 * @param[in] sink Sink slot being rewritten.
 */
inline void feeds_release_sink(Netlist &nl, Slot sink) noexcept {
  if (sink >= slot_count(nl)) {
    return;
  }
  const Gate &g = nl.slots[sink];
  for (std::uint8_t i = 0; i < g.input_count; ++i) {
    const std::size_t flat = tie_flat(sink, i);
    if (!is_shared_flat(nl, flat)) {
      continue;
    }
    const Slot drv = g.drivers[i];
    if (drv < slot_count(nl) && nl.slots[drv].live && nl.feeds_shared[drv] > 0) {
      --nl.feeds_shared[drv];
    }
    for_each_extra(nl, flat, [&](std::uint8_t, Slot edrv, std::uint8_t) {
      if (edrv < slot_count(nl) && nl.slots[edrv].live && nl.feeds_shared[edrv] > 0) {
        --nl.feeds_shared[edrv];
      }
    });
  }
}

/**
 * @brief Increment marked-input drivers of one sink (after its rewrite).
 *
 * @param[out] nl Netlist holding feeder counters.
 * @param[in] sink Sink slot just rewritten.
 */
inline void feeds_acquire_sink(Netlist &nl, Slot sink) noexcept {
  if (sink >= slot_count(nl)) {
    return;
  }
  const Gate &g = nl.slots[sink];
  for (std::uint8_t i = 0; i < g.input_count; ++i) {
    const std::size_t flat = tie_flat(sink, i);
    if (!is_shared_flat(nl, flat)) {
      continue;
    }
    const Slot drv = g.drivers[i];
    if (drv < slot_count(nl) && nl.slots[drv].live) {
      ++nl.feeds_shared[drv];
    }
    for_each_extra(nl, flat, [&](std::uint8_t, Slot edrv, std::uint8_t) {
      if (edrv < slot_count(nl) && nl.slots[edrv].live) {
        ++nl.feeds_shared[edrv];
      }
    });
  }
}

} // namespace detail

/**
 * @brief Recompute every shared-feeder counter from the tables
 *        (bulk-load/build path). Cold paths only.
 *
 * @param[out] nl Netlist whose counters rebuild.
 */
inline void recompute_feeds(Netlist &nl) noexcept {
  for (Slot s = 0; s < static_cast<Slot>(slot_count(nl)); ++s) {
    nl.feeds_shared[s] = 0;
  }
  for (Slot s = 0; s < static_cast<Slot>(slot_count(nl)); ++s) {
    detail::feeds_acquire_sink(nl, s);
  }
}

/**
 * @brief Grow the pool so slot `need - 1` is addressable (the one growth
 *        seam: forwards to ensure_slots, which sizes truth + derived +
 *        edge-pool + heap reservation together).
 *
 * @param[out] nl Pool to grow.
 * @param[in] need Minimum addressable slot count.
 * @return True when `need` slots are addressable; false past kMaxGates.
 */
[[nodiscard]] inline bool grow(Netlist &nl, std::size_t need) {
  return ensure_slots(nl, need);
}

/**
 * @brief Grow pool workspace scratch for `n` slots (the one scratch seam:
 *        forwards to ensure_workspace).
 *
 * @param[out] nl Pool whose workspace grows.
 * @param[in] n Addressable slot count the workspace must cover.
 */
inline void ensure_scratch(Netlist &nl, std::size_t n) {
  ensure_workspace(nl, n);
}

/**
 * @brief The one mutation mark: every incremental edge/table event funnels
 *        its dirty flag through here (adjacency + feeds stay exact
 *        inline at the event; this keeps the recompile trigger in one
 *        grep-able place).
 *
 * @param[out] nl Netlist whose topology is now stale.
 */
inline void note_edge_event(Netlist &nl) noexcept {
  nl.dirty = true;
}

/**
 * @brief Clear every derived store to its sized-empty state without
 *        touching truth: chains drop, heads park at kNoDriver, the
 *        free-slot heap reindexes every dead slot, feeder counters zero.
 *        Pool-sized capacity is retained (no shrink, no alloc on hot
 *        paths beyond what the callees already guarantee).
 *
 * @param[out] nl Netlist whose derived stores clear.
 */
inline void clear_derived(Netlist &nl) noexcept {
  adjacency::adjacency_clear(nl);
  for (std::size_t i = 0; i < nl.tie_head.size(); ++i) {
    nl.tie_head[i] = kNoDriver;
  }
  for (std::size_t i = 0; i < nl.tie_next.size(); ++i) {
    nl.tie_next[i] = kNoDriver;
  }
  free_heap_rebuild(nl);
  for (std::size_t s = 0; s < nl.feeds_shared.size(); ++s) {
    nl.feeds_shared[s] = 0;
  }
}

/**
 * @brief Rebuild every derived store from the truth tables in one pass
 *        (the one bulk-load/build seam): successor chains, tie-class
 *        chains, free-slot heap, feeder counters. Idempotent and exact:
 *        a freshly rebuilt net compares equal (chain-for-chain) to the
 *        same net built incrementally through the mutation seam.
 *
 * (No noexcept: defensive growth may allocate; steady-state callers find
 * everything sized.)
 *
 * @param[out] nl Netlist whose derived stores rebuild from its tables.
 */
inline void rebuild_all(Netlist &nl) {
  adjacency::adjacency_rebuild(nl);
  tie_chains_rebuild(nl);
  free_heap_rebuild(nl);
  recompute_feeds(nl);
}

/**
 * @brief Deterministic heap footprint of one live net (pool bytes): the
 *        sum over EVERY Netlist vector of size()*element_size, plus
 *        sizeof(Netlist) for the handle itself. No allocator sampling:
 *        every size is exact, so identical args re-run to bit-identical
 *        footprints. This is the single pool_bytes definition — callers
 *        (benchmarks, scale tests) use it instead of open-coding the sum.
 *
 * @param[in] nl Netlist to measure.
 * @return Heap footprint in bytes (handle + all stores).
 */
[[nodiscard]] inline std::size_t bytes(const Netlist &nl) noexcept {
  std::size_t b = sizeof(Netlist);
  b += nl.slots.size() * sizeof(Gate);
  b += nl.driver_inv.size() * sizeof(std::uint8_t);
  b += nl.tie_rep.size() * sizeof(Slot);
  b += nl.shared_meta.size() * sizeof(std::uint8_t);
  b += nl.extra_drivers.size() * sizeof(Slot);
  b += nl.extra_inv.size() * sizeof(std::uint8_t);
  b += nl.pull_cfg.size() * sizeof(std::uint8_t);
  b += nl.topo_order.size() * sizeof(Slot);
  b += nl.topo_level.size() * sizeof(Slot);
  // Derived stores (bench pool_bytes predates them — counted here so a
  // new derived array lands in exactly one place).
  b += nl.succ_head.size() * sizeof(Slot);
  b += nl.succ_next.size() * sizeof(Slot);
  b += nl.succ_sink.size() * sizeof(Slot);
  b += nl.succ_input.size() * sizeof(std::uint8_t);
  b += nl.succ_extra.size() * sizeof(std::uint8_t);
  b += nl.succ_free.size() * sizeof(Slot);
  b += nl.tie_head.size() * sizeof(Slot);
  b += nl.tie_next.size() * sizeof(Slot);
  b += nl.free_heap.size() * sizeof(Slot);
  b += nl.feeds_shared.size() * sizeof(Slot);
  // Workspace scratch.
  b += nl.ws_indeg.size() * sizeof(std::uint8_t);
  b += nl.ws_queue.size() * sizeof(Slot);
  b += nl.ws_order.size() * sizeof(Slot);
  b += nl.ws_levels.size() * sizeof(Slot);
  b += nl.ws_resolved.size() * sizeof(LogicValue);
  b += nl.ws_gate_out.size() * sizeof(LogicValue);
  b += nl.ws_in_pos.size() * sizeof(Slot);
  b += nl.ws_words.size() * sizeof(std::uint64_t);
  b += nl.ws_next_q.size() * sizeof(LogicValue);
  b += nl.ws_forced_q.size() * sizeof(LogicValue);
  b += nl.ws_force.size() * sizeof(std::uint8_t);
  b += nl.ws_comb_order.size() * sizeof(Slot);
  b += nl.ws_survivors.size() * sizeof(std::size_t);
  b += nl.ws_seen.size() * sizeof(std::uint8_t);
  b += nl.ws_stack.size() * sizeof(Slot);
  return b;
}

} // namespace wengine::logic::pool_store
