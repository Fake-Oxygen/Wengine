#pragma once

/**
 * @file
 * Canonical topology module (C1 deepening) of the logic pipeline: the one Kahn core
 * every compile/validation path runs. Compiled once per mutation
 * (dirty flag): Kahn's algorithm over slot order yields a deterministic
 * level order with a cycle check included; steady-state solve never
 * re-sorts. Three direct entries, no cross-header forwarders:
 * rebuild (mutating compile), validate (pure verdict), and
 * compile_comb_order (pure combinational order for the stepper);
 * reaches is the pure reachability query.
 *
 * Edge-filter rule: kAll counts every driver edge (combinational cycle
 * check); kCombOnly excludes edges into sequential-gate inputs (flop Q
 * outputs break loops, so feedback through flops validates while pure
 * combinational cycles still fail loudly).
 *
 * Dynamic pool: the Kahn core and all four entries work over Netlist
 * with pool-owned workspace scratch (indeg/queue/order/levels/seen);
 * every bound is the pool's current size (slot_count), never kMaxGates.
 * The workspace-backed entries grow scratch on demand (amortized).
 *
 * Dependency rules (docs/logic.md section 4): header-only,
 * allocation-free, exception-free; depends on types.hpp, shared_net.hpp
 * (shared-extra driver-edge scan), tie_set.hpp (flat indexing — tie_set
 * itself needs only types.hpp plus shared_net.hpp, so no include cycle),
 * plus core/status (Netlist lives in types.hpp, so netlist/eval/seq_step/cli
 * can all include this with no cycle). Logic is a standalone domain:
 * this header must never depend on CLI types. Consumers: eval.hpp,
 * netlist.hpp mutation, seq_step.hpp, the optimize pipeline, layout.hpp,
 * the CLI wire/truth verbs, and the test_logic* suite.
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

// (011) Extra-edge count for one gate's Kahn indegree: extras are
// driver-graph edges exactly like primaries (a cycle or an ordering
// constraint closing through an extra must compile exactly as through
// a primary). Returns 0xFF when any extra driver is dangling (dead or
// out of range) so the caller fails loudly; unmarked netlists hold no
// extras and read exactly today's counts.
[[nodiscard]] inline std::uint8_t extra_edge_count(const Netlist &nl, const Gate &g, Slot s, bool comb_only) noexcept {
  std::uint8_t n = 0;
  for (std::uint8_t i = 0; i < g.input_count; ++i) {
    const std::size_t flat = tie_flat(s, i);
    bool dangling = false;
    for_each_extra(nl, flat, [&](std::uint8_t, Slot edrv, std::uint8_t) {
      if (dangling) {
        return;
      }
      if (edrv >= slot_count(nl) || !nl.slots[edrv].live) {
        dangling = true; // dangling extra: loud, never partial
        return;
      }
      if (!comb_only || !is_sequential_kind(nl.slots[edrv].kind)) {
        ++n;
      }
    });
    if (dangling) {
      return 0xFF;
    }
  }
  return n;
}

// (011) Per-gate Kahn indegree over primary + extra driver edges.
// comb_only=false counts every present primary (kAll: a dangling primary
// counts and never drains, like a dangling extra saturating below);
// comb_only=true counts only non-sequential drivers and returns 0xFF on
// any dangling primary/extra so the caller fails loudly. Unmarked
// netlists hold no extras and read exactly today's counts.
[[nodiscard]] inline std::uint8_t gate_indeg(const Netlist &nl, const Gate &g, Slot s, bool comb_only) noexcept {
  std::uint8_t d = 0;
  for (std::uint8_t i = 0; i < g.input_count; ++i) {
    const Slot drv = g.drivers[i];
    if (drv == kNoDriver) {
      continue;
    }
    if (!comb_only) {
      ++d;
      continue;
    }
    if (drv >= slot_count(nl) || !nl.slots[drv].live) {
      // Dangling-driver defect marker: 0xFF is unreachable for
      // legitimate counts (at most input_count * 4 = 32 edges per
      // gate), so only truly dangling graphs trip the caller check.
      // (011 correction: the old kMaxInputs saturation collided with
      // fully-wired 8-input gates, which are legal — not dangling.)
      return 0xFF;
    }
    if (!is_sequential_kind(nl.slots[drv].kind)) {
      ++d;
    }
  }
  // (011) Extras are driver-graph edges: a dangling extra saturates
  // (never drains, like a dangling primary on the kAll path).
  const std::uint8_t extra_d = extra_edge_count(nl, g, s, comb_only);
  if (extra_d == 0xFF) {
    return 0xFF;
  }
  return static_cast<std::uint8_t>(d + extra_d);
}

// (011) Max driver level + 1 over primary + extra edges: extra drivers
// order the sink the same way primaries do. level_out may be nullptr
// (the stepper path skips levels); out-of-range drivers read 0.
[[nodiscard]] inline Slot emit_depth(const Netlist &nl, Slot cur, const Slot *level_out) noexcept {
  Slot depth = 0;
  const Gate &cg = nl.slots[cur];
  for (std::uint8_t i = 0; i < cg.input_count; ++i) {
    const Slot drv = cg.drivers[i];
    if (drv == kNoDriver || drv >= slot_count(nl)) {
      continue;
    }
    const Slot base = (level_out != nullptr) ? level_out[drv] : 0;
    const Slot cand = static_cast<Slot>(base + 1);
    if (cand > depth) {
      depth = cand;
    }
  }
  for (std::uint8_t i = 0; i < cg.input_count; ++i) {
    for_each_extra(nl, tie_flat(cur, i), [&](std::uint8_t, Slot edrv, std::uint8_t) {
      if (edrv >= slot_count(nl)) {
        return;
      }
      const Slot base = (level_out != nullptr) ? level_out[edrv] : 0;
      const Slot cand = static_cast<Slot>(base + 1);
      if (cand > depth) {
        depth = cand;
      }
    });
  }
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

// (011) True when cur drives s through any extra attachment (seam
// adapter: single-input predicate is shared_uses_driver_flat).
// Retained for detail-level callers; the Kahn drains below use the
// single-walk for_each_extra form directly.

/**
 * @brief Combinational indegree over comb-only edges: flop outputs read
 *        as always-ready sources, so sequential-driven edges cost 0.
 *
 * Shared by every kCombOnly entry below.
 *
 * @param[in] nl Netlist to scan.
 * @param[out] indeg Per-slot combinational indegree (sequential slots read 0).
 * @param[out] n_comb Number of live combinational gates.
 */
inline void count_comb_indeg(const Netlist &nl, std::uint8_t *indeg, std::size_t &n_comb) noexcept {
  n_comb = 0;
  for (Slot s = 0; s < slot_count(nl); ++s) {
    indeg[s] = 0;
    const Gate &g = nl.slots[s];
    if (!g.live || is_sequential_kind(g.kind)) {
      continue;
    }
    ++n_comb;
    indeg[s] = gate_indeg(nl, g, s, true);
  }
}

/**
 * @brief How sequential gates appear in the Kahn output order.
 */
enum class SeqOut : std::uint8_t {
  /** @brief kAll path: no special-casing, every gate is emitted. */
  kNone = 0,       // kAll path: no special-casing, every gate is emitted.
  /** @brief rebuild/kCombOnly: sequential gates emitted first as roots. */
  kPrependSeq = 1, // rebuild/kCombOnly: seq gates emitted first as roots.
  /** @brief Stepper order: sequential gates seed readiness but are skipped. */
  kSkipSeq = 2,    // stepper order: seq gates seed readiness but are skipped.
};

/**
 * @brief The one Kahn skeleton: pure slot-order topological compile.
 *
 * Reads nl, writes caller buffers, never mutates. Slot-order seeds and
 * successors (FIFO) everywhere, so every entry point shares one
 * determinism story. Levels: 0 for sources (and sequential roots under
 * kCombOnly), else 1 + max(driver levels).
 *
 * @param[in] nl Netlist to compile.
 * @param[in] filter Edge set to count (kAll vs kCombOnly).
 * @param[out] order_out Emission-order slots.
 * @param[out] level_out Per-slot levels (may be nullptr to skip).
 * @param[out] n_out Number of emitted slots.
 * @param[in] seq_out How sequential gates appear in the output.
 * @param[in] indeg Caller-owned scratch (kCap indegrees; zeroed here).
 * @param[in] queue Caller-owned scratch (kCap slots: FIFO work queue).
 * @return True when every gate emitted; false on any defect (dangling
 *         driver, combinational cycle) or undersized scratch.
 */
inline bool kahn_pure(const Netlist &nl, EdgeFilter filter, Slot *order_out, Slot *level_out, std::size_t &n_out, SeqOut seq_out, std::span<std::uint8_t> indeg, std::span<Slot> queue) noexcept {
  if (indeg.size() < slot_count(nl) || queue.size() < slot_count(nl)) {
    return false;
  }
  if (filter == EdgeFilter::kAll) {
    for (Slot s = 0; s < slot_count(nl); ++s) {
      indeg[s] = 0;
    }
    std::size_t live = 0;
    for (Slot s = 0; s < slot_count(nl); ++s) {
      const Gate &g = nl.slots[s];
      if (!g.live) {
        continue;
      }
      ++live;
      indeg[s] = gate_indeg(nl, g, s, false);
      if (level_out != nullptr) {
        level_out[s] = 0;
      }
    }
    std::size_t head = 0;
    std::size_t tail = 0;
    for (Slot s = 0; s < slot_count(nl); ++s) {
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
  for (Slot s = 0; s < slot_count(nl); ++s) {
    indeg[s] = 0;
  }
  std::size_t n_comb = 0;
  count_comb_indeg(nl, indeg.data(), n_comb);
  for (Slot s = 0; s < slot_count(nl); ++s) {
    if (nl.slots[s].live && !is_sequential_kind(nl.slots[s].kind) && indeg[s] == 0xFF) {
      return false; // dangling driver defect: loud, never partial
    }
  }
  std::size_t n_emit = 0;
  if (seq_out == SeqOut::kPrependSeq) {
    for (Slot s = 0; s < slot_count(nl); ++s) {
      const Gate &g = nl.slots[s];
      if (!g.live || !is_sequential_kind(g.kind)) {
        continue;
      }
      order_out[n_emit++] = s;
      if (level_out != nullptr) {
        level_out[s] = 0;
      }
    }
  } else if (level_out != nullptr) {
    for (Slot s = 0; s < slot_count(nl); ++s) {
      if (nl.slots[s].live && is_sequential_kind(nl.slots[s].kind)) {
        level_out[s] = 0;
      }
    }
  }
  std::size_t head = 0;
  std::size_t tail = 0;
  for (Slot s = 0; s < slot_count(nl); ++s) {
    const Gate &g = nl.slots[s];
    if (!g.live || is_sequential_kind(g.kind)) {
      continue;
    }
    if (indeg[s] == 0) {
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
 * @brief Kahn scratch bundle: emission order, levels, indegrees, and
 *        the FIFO work queue.
 *
 * One bundle serves rebuild, validate, and (indeg/queue only)
 * compile_comb_order; reaches takes its own seen/stack pair (different
 * shape). Sized by the pool (see ensure_workspace); validated loudly
 * before any work.
 *
 */
struct TopoScratch {
  /** @brief Emission-order slots (pool-sized). */
  std::span<Slot> order;
  /** @brief Per-slot levels (pool-sized, zeroed by the caller). */
  std::span<Slot> levels;
  /** @brief Per-slot indegrees (pool-sized). */
  std::span<std::uint8_t> indeg;
  /** @brief FIFO work queue (pool-sized). */
  std::span<Slot> queue;
  /**
   * @brief Loud size validation before any work (engine-seam D3 rule).
   *
   * @param[in] n Addressable slot count the spans must cover.
   * @return kOk when every span holds at least `n` entries.
   */
  [[nodiscard]] core::Status validate(std::size_t n) const noexcept {
    if (order.size() < n || levels.size() < n || indeg.size() < n || queue.size() < n) {
      return core::Status::failure(core::StatusCode::kInvalidArgument);
    }
    return core::Status::success();
  }
};

/**
 * @brief Compile (or reuse) the deterministic level order.
 *
 * Zero-indegree seeds scan in slot order; successors of each popped gate
 * scan in slot order; FIFO queue. kAll: Kahn over all edges (early-out
 * included). kCombOnly: sequential gates emit first as slot-order roots
 * (level 0), then Kahn over comb-only edges.
 *
 * @param[out] nl Netlist receiving topo_order/level/size/dirty.
 * @param[in] filter Edge set to count.
 * @param[in] ts Caller-owned Kahn scratch (see TopoScratch).
 * @return kOk on success; kInvalidArgument on any defect (dangling
 *         driver, combinational cycle) or undersized scratch. The kAll
 *         path clears the order and keeps dirty on failure; kCombOnly
 *         leaves dirty/size untouched on failure.
 */
[[nodiscard]] inline core::Status rebuild(Netlist &nl, EdgeFilter filter, const TopoScratch &ts) noexcept {
  if (!ts.validate(slot_count(nl)).ok()) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  for (std::size_t i = 0; i < slot_count(nl); ++i) {
    ts.levels[i] = 0;
  }
  if (filter == EdgeFilter::kAll) {
    if (!nl.dirty && nl.topo_order.size() == nl.live_count) {
      return core::Status::success();
    }
    std::size_t n_out = 0;
    if (!detail::kahn_pure(nl, filter, ts.order.data(), ts.levels.data(), n_out, detail::SeqOut::kNone,
                                 ts.indeg, ts.queue)) {
      nl.topo_order.clear();
      nl.dirty = true;
      return core::Status::failure(core::StatusCode::kInvalidArgument);
    }
    nl.topo_order.assign(ts.order.data(), ts.order.data() + n_out);
    for (Slot s = 0; s < slot_count(nl); ++s) {
      if (nl.slots[s].live) {
        nl.topo_level[s] = ts.levels[s];
      }
    }
    nl.dirty = false;
    return core::Status::success();
  }
  // kCombOnly: always recompiles; leaves dirty/size untouched on failure.
  std::size_t n_out = 0;
  if (!detail::kahn_pure(nl, filter, ts.order.data(), ts.levels.data(), n_out, detail::SeqOut::kPrependSeq,
                               ts.indeg, ts.queue)) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  nl.topo_order.assign(ts.order.data(), ts.order.data() + n_out);
  for (Slot s = 0; s < slot_count(nl); ++s) {
    if (nl.slots[s].live) {
      nl.topo_level[s] = ts.levels[s];
    }
  }
  nl.dirty = false;
  return core::Status::success();
}

/**
 * @brief Compile (or reuse) the deterministic level order.
 *
 * Workspace-backed entry: grows the pool's Kahn scratch to the pool
 * size, then runs the core above. No caller scratch, no fixed stack.
 * (No noexcept: workspace growth may allocate.)
 *
 * @param[out] nl Netlist receiving topo_order/level/dirty.
 * @param[in] filter Edge set to count.
 * @return kOk on success; kInvalidArgument on any defect (dangling
 *         driver, combinational cycle). The kAll path clears the order
 *         and keeps dirty on failure; kCombOnly leaves dirty untouched
 *         on failure.
 */
[[nodiscard]] inline core::Status rebuild(Netlist &nl, EdgeFilter filter) {
  const std::size_t n = slot_count(nl);
  ensure_workspace(nl, n);
  const TopoScratch ts{std::span<Slot>(nl.ws_order.data(), n), std::span<Slot>(nl.ws_levels.data(), n),
                       std::span<std::uint8_t>(nl.ws_indeg.data(), n), std::span<Slot>(nl.ws_queue.data(), n)};
  return rebuild(nl, filter, ts);
}

/**
 * @brief Pure verdict over the same Kahn core: no mutation, fixed stack
 *        arrays.
 *
 * kAll sees every edge (combinational cycle check); kCombOnly excludes
 * flop-breaking edges (feedback through flops validates, pure
 * combinational cycles still fail loudly).
 *
 * @param[in] nl Netlist to check.
 * @param[in] filter Edge set to count.
 * @param[in] ts Caller-owned Kahn scratch (see TopoScratch).
 * @return kOk when the netlist compiles; kInvalidArgument on any defect
 *         (dangling driver, combinational cycle) or undersized scratch.
 */
[[nodiscard]] inline core::Status validate(const Netlist &nl, EdgeFilter filter, const TopoScratch &ts) noexcept {
  if (!ts.validate(slot_count(nl)).ok()) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  for (std::size_t i = 0; i < slot_count(nl); ++i) {
    ts.levels[i] = 0;
  }
  std::size_t n_out = 0;
  const detail::SeqOut mode =
      (filter == EdgeFilter::kAll) ? detail::SeqOut::kNone : detail::SeqOut::kPrependSeq;
  if (!detail::kahn_pure(nl, filter, ts.order.data(), ts.levels.data(), n_out, mode, ts.indeg, ts.queue)) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  return core::Status::success();
}

/**
 * @brief Pure verdict over the same Kahn core.
 *
 * Workspace-backed entry: grows the pool's Kahn scratch to the pool
 * size, then runs the core above. (No noexcept: workspace growth may
 * allocate.)
 *
 * @param[in,out] nl Netlist to check (workspace grows; topology untouched).
 * @param[in] filter Edge set to count.
 * @return kOk when the netlist compiles; kInvalidArgument on any defect
 *         (dangling driver, combinational cycle).
 */
[[nodiscard]] inline core::Status validate(Netlist &nl, EdgeFilter filter) {
  const std::size_t n = slot_count(nl);
  ensure_workspace(nl, n);
  const TopoScratch ts{std::span<Slot>(nl.ws_order.data(), n), std::span<Slot>(nl.ws_levels.data(), n),
                       std::span<std::uint8_t>(nl.ws_indeg.data(), n), std::span<Slot>(nl.ws_queue.data(), n)};
  return validate(nl, filter, ts);
}

/**
 * @brief Pure verdict over the same Kahn core (const overload).
 *
 * For read-only nets: builds thread-local-sized scratch on the heap
 * instead of touching the pool workspace, so const callers (config
 * validation, export previews) keep working. Prefer the mutable entry
 * in hot paths (no allocation once sized).
 *
 * @param[in] nl Netlist to check.
 * @param[in] filter Edge set to count.
 * @return kOk when the netlist compiles; kInvalidArgument on any defect
 *         (dangling driver, combinational cycle).
 */
[[nodiscard]] inline core::Status validate(const Netlist &nl, EdgeFilter filter) {
  const std::size_t n = slot_count(nl);
  std::vector<Slot> order(n), levels(n), queue(n);
  std::vector<std::uint8_t> indeg(n);
  const TopoScratch ts{std::span<Slot>(order.data(), n), std::span<Slot>(levels.data(), n),
                       std::span<std::uint8_t>(indeg.data(), n), std::span<Slot>(queue.data(), n)};
  return validate(nl, filter, ts);
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
 * @brief Combinational-only level order for the stepper: Kahn over
 *        combinational edges only, same slot-order determinism as
 *        rebuild.
 *
 * Sequential gates act as roots (their Q is the source value) and never
 * appear in the output.
 *
 * @param[in] nl Netlist to compile.
 * @param[out] order_out Emission-order combinational slots.
 * @param[out] n_out Number of emitted slots.
 * @param[in] indeg Caller-owned scratch (kCap indegrees).
 * @param[in] queue Caller-owned scratch (kCap slots: FIFO work queue).
 * @return True on success; false on any combinational cycle or
 *         dangling-driver defect (so stepping never evaluates a partial
 *         order) or undersized scratch.
 */
[[nodiscard]] inline bool compile_comb_order(const Netlist &nl, Slot *order_out, std::size_t &n_out, std::span<std::uint8_t> indeg, std::span<Slot> queue) noexcept {
  return detail::kahn_pure(nl, EdgeFilter::kCombOnly, order_out, nullptr, n_out, detail::SeqOut::kSkipSeq, indeg, queue);
}

/**
 * @brief Combinational-only level order for the stepper.
 *
 * Workspace-backed entry: grows the pool's Kahn scratch to the pool
 * size, then runs the core above. The caller-owned order buffer must
 * hold at least pool-size slots. (No noexcept: workspace growth may
 * allocate.)
 *
 * @param[in,out] nl Netlist to compile (workspace grows).
 * @param[out] order_out Emission-order combinational slots.
 * @param[out] n_out Number of emitted slots.
 * @return True on success; false on any combinational cycle or
 *         dangling-driver defect, so stepping never evaluates a partial
 *         order.
 */
[[nodiscard]] inline bool compile_comb_order(Netlist &nl, Slot *order_out, std::size_t &n_out) {
  const std::size_t n = slot_count(nl);
  ensure_workspace(nl, n);
  return compile_comb_order(nl, order_out, n_out, std::span<std::uint8_t>(nl.ws_indeg.data(), n), std::span<Slot>(nl.ws_queue.data(), n));
}

} // namespace topo

} // namespace wengine::logic
