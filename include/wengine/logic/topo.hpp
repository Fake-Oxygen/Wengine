#pragma once

/**
 * @file
 * Canonical topology module (C1 deepening) of the logic pipeline: the one Kahn core
 * every compile/validation path runs. Compiled once per mutation
 * (dirty flag): Kahn's algorithm over slot order yields a deterministic
 * level order with a cycle check included; steady-state solve never
 * re-sorts and allocates only amortized (workspace growth on first
 * sizing).
 *
 * CompiledOrder seam: two entries own their scratch, so no caller
 * repeats the ensure_workspace + scratch-bundle + rebuild prologue:
 * compile (mutating compile into topo_order/topo_level) and query
 * (pure verdict, workspace-backed or const heap-backed). One
 * scratch-taking core (detail::kahn_core) stays behind as the internal
 * seam the undersized-scratch refusal tests pin. Per-gate edge
 * vocabulary is one callback (detail::for_each_driver_edge): indegree
 * and depth share the single primary-then-extras walk, and dangling
 * drivers report through core::Result instead of the old 0xFF magic.
 *
 * Edge-filter rule: kAll counts every driver edge (combinational cycle
 * check); kCombOnly excludes edges into sequential-gate inputs (flop Q
 * outputs break loops, so feedback through flops validates while pure
 * combinational cycles still fail loudly).
 *
 * Dynamic pool: the Kahn core and all entries work over Netlist with
 * pool-owned workspace scratch (indeg/queue/order/levels/seen/stack);
 * every bound is the pool's current size (slot_count), never kMaxGates.
 * The workspace-backed entries grow scratch on demand (amortized).
 *
 * Dependency rules (docs/logic.md section 4): header-only,
 * allocation-free on the core paths (the owning entries may grow
 * workspace amortized), exception-free; depends on types.hpp,
 * shared_net.hpp (shared-extra driver-edge scan), tie_set.hpp (flat
 * indexing — tie_set itself needs only types.hpp plus shared_net.hpp,
 * so no include cycle), plus core/status (Netlist lives in types.hpp,
 * so netlist/eval/seq_step/cli can all include this with no cycle).
 * Logic is a standalone domain: this header must never depend on CLI
 * types. Consumers: eval.hpp, netlist.hpp mutation, seq_step.hpp, the
 * optimize pipeline, layout.hpp, the CLI wire/truth verbs, and the
 * test_logic* suite.
 */

#include "wengine/core/status.hpp"
#include "wengine/logic/shared_net.hpp"
#include "wengine/logic/tie_set.hpp"
#include "wengine/logic/types.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace wengine::logic {

/**
 * @brief Which driver edges the Kahn core counts.
 */
enum class EdgeFilter : std::uint8_t {
  /** @brief Every driver edge counts (combinational cycle check). */
  kAll = 0,
  /** @brief Edges into sequential-gate inputs excluded (flop Q outputs break loops). */
  kCombOnly = 1,
};

namespace topo {

namespace detail {

// --- Single per-gate edge walk (the one driver-edge vocabulary) ---------------
// Every per-gate scan (indegree, depth) rides this callback: each stored
// driver edge visited once, primaries then extras per input, input
// order. Callers apply their own filter (edge counting, level lookup)
// and their own dead policy (loud refusal vs transparent skip).

/**
 * @brief Visit every stored driver edge of one gate once (primary then
 *        extras per input, input order).
 *
 * kNoDriver primaries are skipped (unwired, never an edge); extras are
 * always stored drivers. Slots are passed verbatim (dead/out-of-range
 * included) so callers own the dead policy loudly in one place.
 *
 * @tparam Fn Callable invoked as fn(driver, input, is_extra).
 * @param[in] nl Netlist holding drivers and extras.
 * @param[in] s Gate slot to walk.
 * @param[in] fn Callable invoked as fn(driver, input, is_extra).
 */
template <typename Fn>
inline void for_each_driver_edge(const Netlist &nl, Slot s, Fn &&fn) noexcept {
  if (s >= slot_count(nl)) {
    return;
  }
  const Gate &g = nl.slots[s];
  if (!g.live) {
    return;
  }
  for (std::uint8_t i = 0; i < g.input_count; ++i) {
    const std::size_t flat = tie_flat(s, i);
    const Slot drv = g.drivers[i];
    if (drv != kNoDriver) {
      fn(drv, i, false);
    }
    for_each_extra(nl, flat, [&](std::uint8_t, Slot edrv, std::uint8_t) { fn(edrv, i, true); });
  }
}

/**
 * @brief Per-gate Kahn indegree over primary + extra driver edges.
 *
 * Single-walk form over for_each_driver_edge: extras are driver-graph
 * edges exactly like primaries. comb_only=false counts every present
 * edge (a dangling primary counts and never drains on the kAll path);
 * comb_only=true counts only non-sequential drivers. Any dangling
 * (dead or out-of-range) driver fails loudly through the Result
 * instead of the old 0xFF magic — the Kahn core maps it to the same
 * loud verdict the drain-mismatch path produced (rebuild clears on
 * kAll, leaves untouched on kCombOnly; validate fails either way).
 * Unmarked netlists visit primaries only and read exactly today's
 * counts.
 *
 * @param[in] nl Netlist holding drivers, marks, and kinds.
 * @param[in] s Gate slot to count.
 * @param[in] comb_only True counts only non-sequential drivers.
 * @return Success with the edge count (at most input_count * 4 = 32),
 *         or failure on any dangling driver.
 */
[[nodiscard]] inline core::Result<std::uint8_t> gate_indeg(const Netlist &nl, Slot s, bool comb_only) noexcept {
  if (s >= slot_count(nl) || !nl.slots[s].live) {
    return core::Result<std::uint8_t>::success(0);
  }
  std::uint8_t d = 0;
  bool bad = false;
  for_each_driver_edge(nl, s, [&](Slot drv, std::uint8_t, bool) noexcept {
    if (bad) {
      return;
    }
    if (drv >= slot_count(nl) || !nl.slots[drv].live) {
      bad = true; // dangling edge: loud, never partial
      return;
    }
    if (!comb_only || !is_sequential_kind(nl.slots[drv].kind)) {
      ++d;
    }
  });
  if (bad) {
    return core::Result<std::uint8_t>::failure("topo: dangling driver");
  }
  return core::Result<std::uint8_t>::success(d);
}

/**
 * @brief Max driver level + 1 over primary + extra edges: extra drivers
 *        order the sink the same way primaries do.
 *
 * Single-walk form over for_each_driver_edge (was two loops: primaries
 * then extras). level_out may be nullptr (the stepper path skips
 * levels); out-of-range drivers read 0.
 *
 * @param[in] nl Netlist holding drivers and extras.
 * @param[in] cur Gate slot whose depth to compute.
 * @param[in] level_out Per-slot levels (may be nullptr to skip).
 * @return Max driver level + 1 (0 for sources).
 */
[[nodiscard]] inline Slot emit_depth(const Netlist &nl, Slot cur, const Slot *level_out) noexcept {
  Slot depth = 0;
  for_each_driver_edge(nl, cur, [&](Slot drv, std::uint8_t, bool) noexcept {
    if (drv >= slot_count(nl)) {
      return;
    }
    const Slot base = (level_out != nullptr) ? level_out[drv] : 0;
    const Slot cand = static_cast<Slot>(base + 1);
    if (cand > depth) {
      depth = cand;
    }
  });
  return depth;
}

// (016) Per-pop successor update: drain one indegree unit per recorded
// edge from cur, queueing newly-ready gates. The chain enumerates
// sinks in ascending (sink, input, extra) order — the same visit order
// the full-pool scan produced (primaries-then-extras per sink), so
// queue pushes land identically: decrements commute, pushes fire at
// the zero crossing, and later same-sink edges skip on the indeg==0
// guard exactly as before. kCombOnly skips sequential sinks (seq edges
// cost 0 and seq gates never pop).
inline void drain_successors(const Netlist &nl, EdgeFilter filter, Slot cur, std::uint8_t *indeg, Slot *queue, std::size_t &tail) noexcept {
  if (cur >= nl.succ_head.size()) {
    return;
  }
  const bool skip_seq = (filter == EdgeFilter::kCombOnly);
  for (Slot e = nl.succ_head[cur]; e != kNoDriver; e = nl.succ_next[e]) {
    const Slot s = nl.succ_sink[e];
    if (s >= slot_count(nl) || !nl.slots[s].live || indeg[s] == 0) {
      continue;
    }
    if (skip_seq && is_sequential_kind(nl.slots[s].kind)) {
      continue;
    }
    --indeg[s];
    if (indeg[s] == 0) {
      queue[tail++] = s;
    }
  }
}

/**
 * @brief The one Kahn skeleton: pure slot-order topological compile.
 *
 * The single scratch-taking core behind every entry point (the seam
 * the undersized-scratch refusal tests pin). Reads nl, writes caller
 * buffers, never mutates. Slot-order seeds and successors (FIFO)
 * everywhere, so every entry point shares one determinism story.
 * Levels: 0 for sources (and sequential roots under kCombOnly), else
 * 1 + max(driver levels).
 *
 * Sequential handling needs no matrix: kAll emits every gate through
 * the same drain (no special-casing); kCombOnly always treats
 * sequential gates as always-ready roots — prepended to the output
 * unless skip_seq asks for the stepper-only tail.
 *
 * @param[in] nl Netlist to compile.
 * @param[in] filter Edge set to count (kAll vs kCombOnly).
 * @param[out] order_out Emission-order slots (caller buffer, pool-sized).
 * @param[out] level_out Per-slot levels (may be nullptr to skip; zeroed here when present).
 * @param[out] n_out Number of emitted slots.
 * @param[in] indeg Caller-owned scratch (pool-size indegrees; zeroed here).
 * @param[in] queue Caller-owned scratch (pool-size slots: FIFO work queue).
 * @param[in] skip_seq True emits combinational gates only (stepper tail);
 *            only meaningful under kCombOnly (ignored under kAll).
 * @return True when every expected gate emitted; false on any defect
 *         (dangling driver, combinational cycle) or undersized scratch.
 */
inline bool kahn_core(const Netlist &nl, EdgeFilter filter, Slot *order_out, Slot *level_out, std::size_t &n_out,
                      std::span<std::uint8_t> indeg, std::span<Slot> queue, bool skip_seq) noexcept {
  const std::size_t n = slot_count(nl);
  n_out = 0;
  if (indeg.size() < n || queue.size() < n) {
    return false;
  }
  if (level_out != nullptr) {
    for (std::size_t i = 0; i < n; ++i) {
      level_out[i] = 0;
    }
  }
  if (filter == EdgeFilter::kAll) {
    std::size_t live = 0;
    for (Slot s = 0; s < n; ++s) {
      indeg[s] = 0;
      if (!nl.slots[s].live) {
        continue;
      }
      ++live;
      const auto r = gate_indeg(nl, s, false);
      if (!r.ok()) {
        return false; // dangling defect: same loud verdict as the old drain mismatch
      }
      indeg[s] = r.value;
    }
    std::size_t head = 0;
    std::size_t tail = 0;
    for (Slot s = 0; s < n; ++s) {
      if (nl.slots[s].live && indeg[s] == 0) {
        queue[tail++] = s;
      }
    }
    std::size_t n_emit = 0;
    while (head < tail) {
      const Slot cur = queue[head++];
      const Slot depth = emit_depth(nl, cur, level_out);
      if (level_out != nullptr) {
        level_out[cur] = depth;
      }
      order_out[n_emit++] = cur;
      drain_successors(nl, EdgeFilter::kAll, cur, indeg.data(), queue.data(), tail);
    }
    n_out = n_emit;
    return n_emit == live;
  }
  // kCombOnly: sequential gates are always-ready roots (level 0). Only
  // combinational pops decrement successors — seq edges cost 0 and seq
  // gates never pop, so mixed fan-in (seq + comb) cannot queue early.
  std::size_t n_comb = 0;
  for (Slot s = 0; s < n; ++s) {
    indeg[s] = 0;
    if (!nl.slots[s].live || is_sequential_kind(nl.slots[s].kind)) {
      continue;
    }
    ++n_comb;
    const auto r = gate_indeg(nl, s, true);
    if (!r.ok()) {
      return false; // dangling driver defect: loud, never partial
    }
    indeg[s] = r.value;
  }
  std::size_t n_emit = 0;
  if (!skip_seq) {
    for (Slot s = 0; s < n; ++s) {
      if (nl.slots[s].live && is_sequential_kind(nl.slots[s].kind)) {
        order_out[n_emit++] = s;
      }
    }
  }
  std::size_t head = 0;
  std::size_t tail = 0;
  for (Slot s = 0; s < n; ++s) {
    if (nl.slots[s].live && !is_sequential_kind(nl.slots[s].kind) && indeg[s] == 0) {
      queue[tail++] = s;
    }
  }
  std::size_t n_comb_emit = 0;
  while (head < tail) {
    const Slot cur = queue[head++];
    const Slot depth = emit_depth(nl, cur, level_out);
    if (level_out != nullptr) {
      level_out[cur] = depth;
    }
    order_out[n_emit++] = cur;
    ++n_comb_emit;
    drain_successors(nl, EdgeFilter::kCombOnly, cur, indeg.data(), queue.data(), tail);
  }
  n_out = n_emit;
  return n_comb_emit == n_comb;
}

} // namespace detail

/**
 * @brief Compile (or reuse) the deterministic level order.
 *
 * The CompiledOrder seam: owns workspace growth internally, so no
 * caller repeats the ensure_workspace + scratch-bundle prologue.
 * Zero-indegree seeds scan in slot order; successors of each popped
 * gate scan in slot order; FIFO queue. kAll: Kahn over all edges
 * (early-out included). kCombOnly: sequential gates emit first as
 * slot-order roots (level 0), then Kahn over comb-only edges.
 *
 * (No noexcept: workspace growth may allocate.)
 *
 * @param[out] nl Netlist receiving topo_order/level/dirty.
 * @param[in] filter Edge set to count.
 * @return kOk on success; kInvalidArgument on any defect (dangling
 *         driver, combinational cycle). The kAll path clears the order
 *         and keeps dirty on failure; kCombOnly leaves dirty/size
 *         untouched on failure.
 */
[[nodiscard]] inline core::Status compile(Netlist &nl, EdgeFilter filter) {
  const std::size_t n = slot_count(nl);
  ensure_workspace(nl, n);
  if (filter == EdgeFilter::kAll) {
    if (!nl.dirty && nl.topo_order.size() == nl.live_count) {
      return core::Status::success();
    }
    std::size_t n_out = 0;
    if (!detail::kahn_core(nl, filter, nl.ws_order.data(), nl.ws_levels.data(), n_out,
                           std::span<std::uint8_t>(nl.ws_indeg.data(), n), std::span<Slot>(nl.ws_queue.data(), n),
                           false)) {
      nl.topo_order.clear();
      nl.dirty = true;
      return core::Status::failure(core::StatusCode::kInvalidArgument);
    }
    nl.topo_order.assign(nl.ws_order.data(), nl.ws_order.data() + n_out);
    for (Slot s = 0; s < n; ++s) {
      if (nl.slots[s].live) {
        nl.topo_level[s] = nl.ws_levels[s];
      }
    }
    nl.dirty = false;
    return core::Status::success();
  }
  // kCombOnly: always recompiles; leaves dirty/size untouched on failure.
  std::size_t n_out = 0;
  if (!detail::kahn_core(nl, filter, nl.ws_order.data(), nl.ws_levels.data(), n_out,
                         std::span<std::uint8_t>(nl.ws_indeg.data(), n), std::span<Slot>(nl.ws_queue.data(), n),
                         false)) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  nl.topo_order.assign(nl.ws_order.data(), nl.ws_order.data() + n_out);
  for (Slot s = 0; s < n; ++s) {
    if (nl.slots[s].live) {
      nl.topo_level[s] = nl.ws_levels[s];
    }
  }
  nl.dirty = false;
  return core::Status::success();
}

/**
 * @brief Pure verdict over the same Kahn core: no mutation.
 *
 * Workspace-backed entry: grows the pool's Kahn scratch to the pool
 * size, then runs the core. (No noexcept: workspace growth may
 * allocate.)
 *
 * kAll sees every edge (combinational cycle check); kCombOnly excludes
 * flop-breaking edges (feedback through flops validates, pure
 * combinational cycles still fail loudly).
 *
 * @param[in,out] nl Netlist to check (workspace grows; topology untouched).
 * @param[in] filter Edge set to count.
 * @return kOk when the netlist compiles; kInvalidArgument on any defect
 *         (dangling driver, combinational cycle).
 */
[[nodiscard]] inline core::Status query(Netlist &nl, EdgeFilter filter) {
  const std::size_t n = slot_count(nl);
  ensure_workspace(nl, n);
  std::size_t n_out = 0;
  if (!detail::kahn_core(nl, filter, nl.ws_order.data(), nl.ws_levels.data(), n_out,
                         std::span<std::uint8_t>(nl.ws_indeg.data(), n), std::span<Slot>(nl.ws_queue.data(), n),
                         false)) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  return core::Status::success();
}

/**
 * @brief Pure verdict over the same Kahn core (const overload).
 *
 * For read-only nets: builds heap scratch instead of touching the pool
 * workspace, so const callers (config validation, export previews)
 * keep working. Prefer the mutable entry in hot paths (no allocation
 * once sized).
 *
 * @param[in] nl Netlist to check.
 * @param[in] filter Edge set to count.
 * @return kOk when the netlist compiles; kInvalidArgument on any defect
 *         (dangling driver, combinational cycle).
 */
[[nodiscard]] inline core::Status query(const Netlist &nl, EdgeFilter filter) {
  const std::size_t n = slot_count(nl);
  std::vector<Slot> order(n), levels(n), queue(n);
  std::vector<std::uint8_t> indeg(n);
  std::size_t n_out = 0;
  if (!detail::kahn_core(nl, filter, order.data(), levels.data(), n_out, std::span<std::uint8_t>(indeg.data(), n),
                         std::span<Slot>(queue.data(), n), false)) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  return core::Status::success();
}

/**
 * @brief Reachability over the driver graph: iterative DFS on caller
 *        scratch (no allocation).
 *
 * Successors come from the maintained adjacency (016): each pop walks
 * only the driver's recorded edges in ascending sink order — the same
 * visit order the full-pool scan produced — so verdicts are identical
 * while the cost tracks the reachable cone, not the pool size.
 *
 * kCombOnly excludes edges into sequential-gate inputs from the edge
 * set (flop Q outputs break loops); kAll counts every edge.
 *
 * @param[in] nl Netlist holding the driver graph.
 * @param[in] from Source slot.
 * @param[in] to Destination slot.
 * @param[in] filter Edge set to follow.
 * @param[in] seen Caller-owned scratch (kCap visited flags, zeroed here).
 * @param[in] stack Caller-owned scratch (kCap slots: DFS stack).
 * @return True when from can reach to (including from == to).
 */
[[nodiscard]] inline bool reaches(const Netlist &nl, Slot from, Slot to, EdgeFilter filter,
                                  std::span<std::uint8_t> seen, std::span<Slot> stack) noexcept {
  if (from == to) {
    return true;
  }
  if (seen.size() < slot_count(nl) || stack.size() < slot_count(nl)) {
    return false;
  }
  for (std::size_t i = 0; i < slot_count(nl); ++i) {
    seen[i] = false;
  }
  const bool skip_seq = (filter == EdgeFilter::kCombOnly);
  std::size_t top = 0;
  stack[top++] = from;
  seen[from] = true;
  while (top > 0) {
    const Slot cur = stack[--top];
    if (cur >= nl.succ_head.size()) {
      continue;
    }
    for (Slot e = nl.succ_head[cur]; e != kNoDriver; e = nl.succ_next[e]) {
      const Slot s = nl.succ_sink[e];
      if (s >= slot_count(nl) || !nl.slots[s].live || seen[s]) {
        continue;
      }
      if (skip_seq && is_sequential_kind(nl.slots[s].kind)) {
        continue; // Q-boundary: no cycle path continues through a flop
      }
      if (s == to) {
        return true;
      }
      seen[s] = true;
      stack[top++] = s;
    }
  }
  return false;
}

/**
 * @brief Reachability over the driver graph.
 *
 * Workspace-backed entry: grows the pool's DFS scratch to the pool
 * size, then runs the core above. (No noexcept: workspace growth may
 * allocate.)
 *
 * @param[in,out] nl Netlist holding the driver graph (workspace grows).
 * @param[in] from Source slot.
 * @param[in] to Destination slot.
 * @param[in] filter Edge set to follow.
 * @return True when from can reach to (including from == to); false for
 *         out-of-pool endpoints.
 */
[[nodiscard]] inline bool reaches(Netlist &nl, Slot from, Slot to, EdgeFilter filter) {
  const std::size_t n = slot_count(nl);
  if (from >= n || to >= n) {
    return from == to;
  }
  ensure_workspace(nl, n);
  return reaches(nl, from, to, filter, std::span<std::uint8_t>(nl.ws_seen.data(), n), std::span<Slot>(nl.ws_stack.data(), n));
}

/**
 * @brief Combinational-only level order for the stepper: one skip-seq
 *        Kahn drain over workspace scratch.
 *
 * Sequential gates act as roots (their Q is the source value) and never
 * appear in the output. Runs the single scratch-taking core
 * (detail::kahn_core, skip_seq) over the CompiledOrder seam's workspace
 * scratch — like the old pure path, it leaves the topo cache
 * (topo_order/topo_level/dirty) untouched, so a later compile(kAll)
 * still recompiles from dirty instead of reusing a comb-only order.
 *
 * (No noexcept: workspace growth may allocate.)
 *
 * @param[in,out] nl Netlist to read (workspace grows; topology cache untouched).
 * @param[out] order_out Emission-order combinational slots (caller
 *             buffer, must hold pool-size slots).
 * @param[out] n_out Number of emitted slots.
 * @return True on success; false on any combinational cycle or
 *         dangling-driver defect, so stepping never evaluates a partial
 *         order.
 */
[[nodiscard]] inline bool comb_order(Netlist &nl, Slot *order_out, std::size_t &n_out) {
  const std::size_t n = slot_count(nl);
  ensure_workspace(nl, n);
  return detail::kahn_core(nl, EdgeFilter::kCombOnly, order_out, nullptr, n_out,
                           std::span<std::uint8_t>(nl.ws_indeg.data(), n),
                           std::span<Slot>(nl.ws_queue.data(), n), true);
}

} // namespace topo

} // namespace wengine::logic
