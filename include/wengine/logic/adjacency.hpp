#pragma once

/**
 * @file
 * Successor-adjacency seam (016) of the logic pipeline: per-driver,
 * slot-ordered edge chains over flat arrays. Only this header may
 * read/write `Netlist::succ_*` directly; everyone else (mutation seam,
 * topo, optimizer passes, loaders, tests) calls the functions below.
 *
 * Representation: `succ_head[from]` is the first edge index of the
 * driver's chain (`kNoDriver` doubles as the no-edge sentinel — edge
 * indices stay far below it); each edge carries sink slot, sink input,
 * and the extra flag (shared-net extras are driver-graph edges exactly
 * like primaries). Chains enumerate sinks in ascending (sink, input,
 * extra) order — the order the Kahn drain and the reachability probe
 * consume. `succ_free` recycles edge indices (no tombstones).
 *
 * Capacity: the edge pool is pool-sized
 * (`cap * kMaxInputs * (1 + kMaxSharedExtra)` in `ensure_slots`), so
 * noexcept seam inserts are infallible by construction — primaries and
 * extras can never overflow it. Growth lives only in `ensure_slots`
 * and `adjacency_rebuild`.
 *
 * Dependency rules (docs/logic.md section 4): header-only,
 * allocation-free, exception-free; depends only on types.hpp plus
 * shared_net.hpp (flat extra scans) plus core/status (never
 * netlist.hpp/topo.hpp: the seam is below both, so no include cycle).
 * Logic is a standalone domain: this header must never depend on CLI
 * types. Consumers: netlist.hpp mutation, topo.hpp drain/probe,
 * optimize passes, the YAML/binary loaders, O2 fresh-build install,
 * reset_netlist, and the test_logic_adjacency corpus.
 */

#include "wengine/core/status.hpp"
#include "wengine/logic/shared_net.hpp"
#include "wengine/logic/types.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace wengine::logic {
namespace adjacency {

/**
 * @brief Insert one edge into the driver's chain, ordered ascending by
 *        (sink, input, extra); equal keys append after existing equals
 *        (stable for duplicate extras from one driver).
 *
 * Assumes edge-pool capacity (pool-sized: infallible by construction);
 * returns false only when the free list is empty or the driver is out
 * of range. Callers fail loudly on false.
 *
 * @param[out] nl Netlist receiving the edge.
 * @param[in] from Driver slot.
 * @param[in] sink Sink slot.
 * @param[in] input Sink input index.
 * @param[in] extra 1 when the edge is a shared-net extra attachment.
 * @return True on insert; false on exhaustion/out-of-range.
 */
[[nodiscard]] inline bool adj_insert_edge(Netlist &nl, Slot from, Slot sink, std::uint8_t input, std::uint8_t extra) noexcept {
  if (from >= nl.succ_head.size() || nl.succ_free.empty()) {
    return false;
  }
  const Slot e = nl.succ_free.back();
  nl.succ_free.pop_back();
  nl.succ_sink[e] = sink;
  nl.succ_input[e] = input;
  nl.succ_extra[e] = extra;
  Slot *link = &nl.succ_head[from];
  while (*link != kNoDriver) {
    const Slot c = *link;
    if (nl.succ_sink[c] > sink ||
        (nl.succ_sink[c] == sink && nl.succ_input[c] > input) ||
        (nl.succ_sink[c] == sink && nl.succ_input[c] == input && nl.succ_extra[c] > extra)) {
      break;
    }
    link = &nl.succ_next[c];
  }
  nl.succ_next[e] = *link;
  *link = e;
  return true;
}

/**
 * @brief Remove the first chain edge matching (from, sink, input,
 *        extra) and recycle its index.
 *
 * @param[out] nl Netlist holding the chains.
 * @param[in] from Driver slot.
 * @param[in] sink Sink slot.
 * @param[in] input Sink input index.
 * @param[in] extra 1 to remove an extra attachment.
 * @return True when an entry was removed; false when no entry matched
 *         (content-invariant violation — callers fail loudly).
 */
[[nodiscard]] inline bool adj_unlink_edge(Netlist &nl, Slot from, Slot sink, std::uint8_t input, std::uint8_t extra) noexcept {
  if (from >= nl.succ_head.size()) {
    return false;
  }
  Slot *link = &nl.succ_head[from];
  while (*link != kNoDriver) {
    const Slot c = *link;
    if (nl.succ_sink[c] == sink && nl.succ_input[c] == input && nl.succ_extra[c] == extra) {
      *link = nl.succ_next[c];
      nl.succ_next[c] = kNoDriver;
      nl.succ_free.push_back(c);
      return true;
    }
    link = &nl.succ_next[c];
  }
  return false;
}

/**
 * @brief Unlink every chain edge targeting sink, reading the current
 *        tables (primaries + extras). Call BEFORE mutating the sink's
 *        inputs; re-insert survivors after (see adj_insert_sink).
 *
 * @param[out] nl Netlist whose chains drop the sink's edges.
 * @param[in] sink Sink slot being rewritten.
 * @return True when every table edge unlinked; false on any miss.
 */
[[nodiscard]] inline bool adj_detach_sink(Netlist &nl, Slot sink) noexcept {
  if (sink >= slot_count(nl)) {
    return true;
  }
  bool ok = true;
  const Gate &g = nl.slots[sink];
  for (std::uint8_t i = 0; i < g.input_count; ++i) {
    const Slot drv = g.drivers[i];
    if (drv != kNoDriver) {
      if (!adj_unlink_edge(nl, drv, sink, i, 0)) {
        ok = false;
      }
    }
  }
  for (std::uint8_t i = 0; i < g.input_count; ++i) {
    const std::size_t flat = tie_flat(sink, i);
    for_each_extra(nl, flat, [&](std::uint8_t, Slot edrv, std::uint8_t) {
      if (!adj_unlink_edge(nl, edrv, sink, i, 1)) {
        ok = false;
      }
    });
  }
  return ok;
}

/**
 * @brief Insert every table edge targeting sink, reading the current
 *        tables. Call AFTER mutating the sink's inputs (the detach half
 *        ran before).
 *
 * @param[out] nl Netlist whose chains gain the sink's edges.
 * @param[in] sink Sink just rewritten.
 * @return True when every table edge inserted; false on any refusal.
 */
[[nodiscard]] inline bool adj_insert_sink(Netlist &nl, Slot sink) noexcept {
  if (sink >= slot_count(nl)) {
    return true;
  }
  bool ok = true;
  const Gate &g = nl.slots[sink];
  for (std::uint8_t i = 0; i < g.input_count; ++i) {
    const Slot drv = g.drivers[i];
    if (drv != kNoDriver) {
      if (!adj_insert_edge(nl, drv, sink, i, 0)) {
        ok = false;
      }
    }
  }
  for (std::uint8_t i = 0; i < g.input_count; ++i) {
    const std::size_t flat = tie_flat(sink, i);
    for_each_extra(nl, flat, [&](std::uint8_t, Slot edrv, std::uint8_t) {
      if (!adj_insert_edge(nl, edrv, sink, i, 1)) {
        ok = false;
      }
    });
  }
  return ok;
}

/**
 * @brief Atomic detach/mutate/re-insert for one sink (the paired path).
 *
 * Detaches the sink's chain edges, runs the table edit, then re-inserts
 * from the edited tables. Returns false when any edge half fails, so a
 * half-applied chain state is loud at the call site instead of silent
 * behind two void calls.
 *
 * @tparam F Table-edit callable invoked as edit() between the halves.
 * @param[out] nl Netlist holding chains + tables.
 * @param[in] sink Sink slot being rewritten.
 * @param[in] edit Table mutation running between detach and insert.
 * @return True when both halves maintained every edge.
 */
template <typename F>
inline bool adj_rewire_sink(Netlist &nl, Slot sink, F &&edit) noexcept(noexcept(edit())) {
  const bool detached = adj_detach_sink(nl, sink);
  edit();
  const bool inserted = adj_insert_sink(nl, sink);
  return detached && inserted;
}

/**
 * @brief Clear every chain and recycle the whole edge pool (the
 *        reset_netlist path). Pool-sized capacity is retained.
 *
 * @param[out] nl Netlist whose chains clear.
 */
inline void adjacency_clear(Netlist &nl) noexcept {
  for (std::size_t s = 0; s < nl.succ_head.size(); ++s) {
    nl.succ_head[s] = kNoDriver;
  }
  nl.succ_next.clear();
  nl.succ_sink.clear();
  nl.succ_input.clear();
  nl.succ_extra.clear();
  nl.succ_free.clear();
  const std::size_t edge_cap = slot_count(nl) * kMaxInputs * (1u + kMaxSharedExtra);
  nl.succ_next.resize(edge_cap, kNoDriver);
  nl.succ_sink.resize(edge_cap, kNoDriver);
  nl.succ_input.resize(edge_cap, 0);
  nl.succ_extra.resize(edge_cap, 0);
  for (std::size_t e = 0; e < edge_cap; ++e) {
    nl.succ_free.push_back(static_cast<Slot>(e));
  }
}

/**
 * @brief Rebuild every chain from the tables in one ascending scan
 *        (the bulk-load path: checkpoint/YAML loaders, O2 fresh-build
 *        install). Defensive about array sizing; appends in scan order
 *        so the layout matches incremental ordered inserts exactly.
 *
 * (No noexcept: defensive growth may allocate; steady-state callers
 * find everything sized.)
 *
 * @param[out] nl Netlist whose chains rebuild from its tables.
 */
inline void adjacency_rebuild(Netlist &nl) {
  const std::size_t n = slot_count(nl);
  if (!ensure_slots(nl, n)) {
    return; // unreachable: need == size is always a no-op success
  }
  if (nl.succ_head.size() < n) {
    nl.succ_head.resize(n, kNoDriver); // defensive: heads always cover the pool
  }
  const std::size_t edge_cap = n * kMaxInputs * (1u + kMaxSharedExtra);
  for (std::size_t s = 0; s < nl.succ_head.size(); ++s) {
    nl.succ_head[s] = kNoDriver;
  }
  nl.succ_next.assign(edge_cap, kNoDriver);
  nl.succ_sink.assign(edge_cap, kNoDriver);
  nl.succ_input.assign(edge_cap, 0);
  nl.succ_extra.assign(edge_cap, 0);
  nl.succ_free.clear();
  for (std::size_t e = 0; e < edge_cap; ++e) {
    nl.succ_free.push_back(static_cast<Slot>(e));
  }
  // Per-driver tails for O(1) appends: ascending scan + append equals
  // the incremental ordered-insert layout exactly.
  std::vector<Slot> tail(n, kNoDriver);
  std::size_t free_top = 0;
  for (Slot s = 0; s < n; ++s) {
    const Gate &g = nl.slots[s];
    if (!g.live) {
      continue;
    }
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      const Slot drv = g.drivers[i];
      if (drv != kNoDriver && drv < n) {
        const std::size_t e = free_top++;
        nl.succ_sink[e] = s;
        nl.succ_input[e] = i;
        nl.succ_extra[e] = 0;
        nl.succ_next[e] = kNoDriver;
        if (tail[drv] == kNoDriver) {
          nl.succ_head[drv] = static_cast<Slot>(e);
        } else {
          nl.succ_next[tail[drv]] = static_cast<Slot>(e);
        }
        tail[drv] = static_cast<Slot>(e);
      }
    }
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      const std::size_t flat = tie_flat(s, i);
      for_each_extra(nl, flat, [&](std::uint8_t, Slot edrv, std::uint8_t) {
        if (edrv >= n) {
          return;
        }
        const std::size_t e = free_top++;
        nl.succ_sink[e] = s;
        nl.succ_input[e] = i;
        nl.succ_extra[e] = 1;
        nl.succ_next[e] = kNoDriver;
        if (tail[edrv] == kNoDriver) {
          nl.succ_head[edrv] = static_cast<Slot>(e);
        } else {
          nl.succ_next[tail[edrv]] = static_cast<Slot>(e);
        }
        tail[edrv] = static_cast<Slot>(e);
      });
    }
  }
  nl.succ_free.erase(nl.succ_free.begin(), nl.succ_free.begin() + static_cast<std::ptrdiff_t>(free_top));
}

/**
 * @brief Count recorded edges over all chains (test/content-invariant
 *        readout).
 *
 * @param[in] nl Netlist whose chains count.
 * @return Total chain entries.
 */
[[nodiscard]] inline std::size_t adj_edge_count(const Netlist &nl) noexcept {
  std::size_t total = 0;
  for (std::size_t d = 0; d < nl.succ_head.size(); ++d) {
    for (Slot e = nl.succ_head[d]; e != kNoDriver; e = nl.succ_next[e]) {
      ++total;
    }
  }
  return total;
}

} // namespace adjacency
} // namespace wengine::logic
