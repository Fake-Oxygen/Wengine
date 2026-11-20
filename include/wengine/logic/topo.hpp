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
[[nodiscard]] inline std::uint8_t extra_edge_count(const Netlist &nl, const Gate &g, std::uint16_t s, bool comb_only) noexcept {
  std::uint8_t n = 0;
  for (std::uint8_t i = 0; i < g.input_count; ++i) {
    const std::size_t flat = tie_flat(s, i);
    bool dangling = false;
    for_each_extra(nl, flat, [&](std::uint8_t, std::uint16_t edrv, std::uint8_t) {
      if (dangling) {
        return;
      }
      if (edrv >= kMaxGates || !nl.gates[edrv].live) {
        dangling = true; // dangling extra: loud, never partial
        return;
      }
      if (!comb_only || !is_sequential_kind(nl.gates[edrv].kind)) {
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
[[nodiscard]] inline std::uint8_t gate_indeg(const Netlist &nl, const Gate &g, std::uint16_t s, bool comb_only) noexcept {
  std::uint8_t d = 0;
  for (std::uint8_t i = 0; i < g.input_count; ++i) {
    const std::uint16_t drv = g.drivers[i];
    if (drv == kNoDriver) {
      continue;
    }
    if (!comb_only) {
      ++d;
      continue;
    }
    if (drv >= kMaxGates || !nl.gates[drv].live) {
      // Dangling-driver defect marker: 0xFF is unreachable for
      // legitimate counts (at most input_count * 4 = 32 edges per
      // gate), so only truly dangling graphs trip the caller check.
      // (011 correction: the old kMaxInputs saturation collided with
      // fully-wired 8-input gates, which are legal — not dangling.)
      return 0xFF;
    }
    if (!is_sequential_kind(nl.gates[drv].kind)) {
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
[[nodiscard]] inline std::uint16_t emit_depth(const Netlist &nl, std::uint16_t cur, const std::uint16_t *level_out) noexcept {
  std::uint16_t depth = 0;
  const Gate &cg = nl.gates[cur];
  for (std::uint8_t i = 0; i < cg.input_count; ++i) {
    const std::uint16_t drv = cg.drivers[i];
    if (drv == kNoDriver || drv >= kMaxGates) {
      continue;
    }
    const std::uint16_t base = (level_out != nullptr) ? level_out[drv] : 0;
    const std::uint16_t cand = static_cast<std::uint16_t>(base + 1);
    if (cand > depth) {
      depth = cand;
    }
  }
  for (std::uint8_t i = 0; i < cg.input_count; ++i) {
    for_each_extra(nl, tie_flat(cur, i), [&](std::uint8_t, std::uint16_t edrv, std::uint8_t) {
      if (edrv >= kMaxGates) {
        return;
      }
      const std::uint16_t base = (level_out != nullptr) ? level_out[edrv] : 0;
      const std::uint16_t cand = static_cast<std::uint16_t>(base + 1);
      if (cand > depth) {
        depth = cand;
      }
    });
  }
  return depth;
}

// (011) Per-pop successor update: drain one indegree unit per
// primary/extra edge from cur, queueing newly-ready gates. Single
// for_each_extra walk (no separate uses_extra pre-scan); extra edges
// drain symmetrically with primaries. kCombOnly skips sequential sinks
// (seq edges cost 0 and seq gates never pop).
inline void drain_successors(const Netlist &nl, EdgeFilter filter, std::uint16_t cur, std::uint8_t *indeg, std::uint16_t *queue, std::size_t &tail) noexcept {
  const bool skip_seq = (filter == EdgeFilter::kCombOnly);
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    const Gate &g = nl.gates[s];
    if (!g.live || indeg[s] == 0) {
      continue;
    }
    if (skip_seq && is_sequential_kind(g.kind)) {
      continue;
    }
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      if (g.drivers[i] == cur) {
        --indeg[s];
        if (indeg[s] == 0) {
          queue[tail++] = s;
          break; // one push per gate (remaining inputs from cur already counted)
        }
      }
    }
    if (indeg[s] != 0) {
      bool drained = false;
      for (std::uint8_t i = 0; i < g.input_count; ++i) {
        for_each_extra(nl, tie_flat(s, i), [&](std::uint8_t, std::uint16_t edrv, std::uint8_t) {
          if (edrv == cur) {
            --indeg[s];
            drained = true;
          }
        });
      }
      if (drained && indeg[s] == 0) {
        queue[tail++] = s;
      }
    }
  }
}

// (011) True when cur drives s through any extra attachment (seam
// adapter: single-input predicate is shared_uses_driver_flat).
// Retained for detail-level callers; the Kahn drains below use the
// single-walk for_each form directly.

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
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    indeg[s] = 0;
    const Gate &g = nl.gates[s];
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
 * @return True when every gate emitted; false on any defect (dangling
 *         driver, combinational cycle).
 */
inline bool kahn_pure(const Netlist &nl, EdgeFilter filter, std::uint16_t *order_out,
                      std::uint16_t *level_out, std::size_t &n_out, SeqOut seq_out) noexcept {
  if (filter == EdgeFilter::kAll) {
    std::uint8_t indeg[kMaxGates] = {};
    std::size_t live = 0;
    for (std::uint16_t s = 0; s < kMaxGates; ++s) {
      const Gate &g = nl.gates[s];
      if (!g.live) {
        continue;
      }
      ++live;
      indeg[s] = gate_indeg(nl, g, s, false);
      if (level_out != nullptr) {
        level_out[s] = 0;
      }
    }
    std::uint16_t queue[kMaxGates];
    std::size_t head = 0;
    std::size_t tail = 0;
    for (std::uint16_t s = 0; s < kMaxGates; ++s) {
      if (nl.gates[s].live && indeg[s] == 0) {
        queue[tail++] = s;
      }
    }
    std::size_t n_emit = 0;
    while (head < tail) {
      const std::uint16_t cur = queue[head++];
      const std::uint16_t depth = emit_depth(nl, cur, level_out);
      if (level_out != nullptr) {
        level_out[cur] = depth;
      }
      order_out[n_emit++] = cur;
      drain_successors(nl, EdgeFilter::kAll, cur, indeg, queue, tail);
    }
    n_out = n_emit;
    return n_emit == live;
  }
  // kCombOnly: sequential gates are always-ready roots (level 0). Only
  // combinational pops decrement successors — seq edges cost 0 and seq
  // gates never pop, so mixed fan-in (seq + comb) cannot queue early.
  std::uint8_t indeg[kMaxGates] = {};
  std::size_t n_comb = 0;
  count_comb_indeg(nl, indeg, n_comb);
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    if (nl.gates[s].live && !is_sequential_kind(nl.gates[s].kind) && indeg[s] == 0xFF) {
      return false; // dangling driver defect: loud, never partial
    }
  }
  std::size_t n_emit = 0;
  if (seq_out == SeqOut::kPrependSeq) {
    for (std::uint16_t s = 0; s < kMaxGates; ++s) {
      const Gate &g = nl.gates[s];
      if (!g.live || !is_sequential_kind(g.kind)) {
        continue;
      }
      order_out[n_emit++] = s;
      if (level_out != nullptr) {
        level_out[s] = 0;
      }
    }
  } else if (level_out != nullptr) {
    for (std::uint16_t s = 0; s < kMaxGates; ++s) {
      if (nl.gates[s].live && is_sequential_kind(nl.gates[s].kind)) {
        level_out[s] = 0;
      }
    }
  }
  std::uint16_t queue[kMaxGates];
  std::size_t head = 0;
  std::size_t tail = 0;
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    const Gate &g = nl.gates[s];
    if (!g.live || is_sequential_kind(g.kind)) {
      continue;
    }
    if (indeg[s] == 0) {
      queue[tail++] = s;
    }
  }
  std::size_t n_comb_emit = 0;
  while (head < tail) {
    const std::uint16_t cur = queue[head++];
    const std::uint16_t depth = emit_depth(nl, cur, level_out);
    if (level_out != nullptr) {
      level_out[cur] = depth;
    }
    order_out[n_emit++] = cur;
    ++n_comb_emit;
    drain_successors(nl, EdgeFilter::kCombOnly, cur, indeg, queue, tail);
  }
  n_out = n_emit;
  return n_comb_emit == n_comb;
}

} // namespace detail

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
 * @return kOk on success; kInvalidArgument on any defect (dangling
 *         driver, combinational cycle). The kAll path zeroes topo_size
 *         and keeps dirty on failure; kCombOnly leaves dirty/size
 *         untouched on failure.
 */
[[nodiscard]] inline core::Status rebuild(Netlist &nl, EdgeFilter filter) noexcept {
  if (filter == EdgeFilter::kAll) {
    if (!nl.dirty && nl.topo_size == nl.live_count) {
      return core::Status::success();
    }
    std::uint16_t order[kMaxGates];
    std::uint16_t levels[kMaxGates] = {};
    std::size_t n_out = 0;
    if (!detail::kahn_pure(nl, filter, order, levels, n_out, detail::SeqOut::kNone)) {
      nl.topo_size = 0;
      nl.dirty = true;
      return core::Status::failure(core::StatusCode::kInvalidArgument);
    }
    for (std::size_t i = 0; i < n_out; ++i) {
      nl.topo_order[i] = order[i];
    }
    for (std::uint16_t s = 0; s < kMaxGates; ++s) {
      if (nl.gates[s].live) {
        nl.topo_level[s] = levels[s];
      }
    }
    nl.topo_size = static_cast<std::uint16_t>(n_out);
    nl.dirty = false;
    return core::Status::success();
  }
  // kCombOnly: always recompiles; leaves dirty/size untouched on failure.
  std::uint16_t order[kMaxGates];
  std::uint16_t levels[kMaxGates] = {};
  std::size_t n_out = 0;
  if (!detail::kahn_pure(nl, filter, order, levels, n_out, detail::SeqOut::kPrependSeq)) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  for (std::size_t i = 0; i < n_out; ++i) {
    nl.topo_order[i] = order[i];
  }
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    if (nl.gates[s].live) {
      nl.topo_level[s] = levels[s];
    }
  }
  nl.topo_size = static_cast<std::uint16_t>(n_out);
  nl.dirty = false;
  return core::Status::success();
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
 * @return kOk when the netlist compiles; kInvalidArgument on any defect
 *         (dangling driver, combinational cycle).
 */
[[nodiscard]] inline core::Status validate(const Netlist &nl, EdgeFilter filter) noexcept {
  std::uint16_t order[kMaxGates];
  std::uint16_t levels[kMaxGates] = {};
  std::size_t n_out = 0;
  const detail::SeqOut mode =
      (filter == EdgeFilter::kAll) ? detail::SeqOut::kNone : detail::SeqOut::kPrependSeq;
  if (!detail::kahn_pure(nl, filter, order, levels, n_out, mode)) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  return core::Status::success();
}

/**
 * @brief Reachability over the driver graph: iterative DFS on a fixed
 *        stack (no allocation).
 *
 * kCombOnly excludes edges into sequential-gate inputs from the edge
 * set (flop Q outputs break loops); kAll counts every edge.
 *
 * @param[in] nl Netlist holding the driver graph.
 * @param[in] from Source slot.
 * @param[in] to Destination slot.
 * @param[in] filter Edge set to follow.
 * @return True when from can reach to (including from == to).
 */
[[nodiscard]] inline bool reaches(const Netlist &nl, std::uint16_t from, std::uint16_t to,
                                  EdgeFilter filter) noexcept {
  if (from == to) {
    return true;
  }
  const bool skip_seq = (filter == EdgeFilter::kCombOnly);
  bool seen[kMaxGates] = {};
  std::uint16_t stack[kMaxGates];
  std::size_t top = 0;
  stack[top++] = from;
  seen[from] = true;
  while (top > 0) {
    const std::uint16_t cur = stack[--top];
    for (std::uint16_t s = 0; s < kMaxGates; ++s) {
      const Gate &g = nl.gates[s];
      if (!g.live) {
        continue;
      }
      if (skip_seq && is_sequential_kind(g.kind)) {
        continue; // Q-boundary: no cycle path continues through a flop
      }
      bool uses_cur = false;
      for (std::uint8_t i = 0; i < g.input_count; ++i) {
        if (g.drivers[i] == cur) {
          uses_cur = true;
          break;
        }
      }
      if (!uses_cur) {
        // Shared extras are driver-graph edges too (011): a cycle
        // closing through an extra attachment must fail the same
        // flop-aware check as any wire. Unmarked netlists hold no
        // extras, so this loop is a no-op there.
        for (std::uint8_t i = 0; i < g.input_count && !uses_cur; ++i) {
          if (shared_uses_driver_flat(nl, tie_flat(s, i), cur)) {
            uses_cur = true;
          }
        }
      }
      if (!uses_cur || seen[s]) {
        continue;
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
 * @return True on success; false on any combinational cycle or
 *         dangling-driver defect, so stepping never evaluates a partial
 *         order.
 */
[[nodiscard]] inline bool compile_comb_order(const Netlist &nl, std::uint16_t *order_out, std::size_t &n_out) noexcept {
  return detail::kahn_pure(nl, EdgeFilter::kCombOnly, order_out, nullptr, n_out, detail::SeqOut::kSkipSeq);
}

} // namespace topo

} // namespace wengine::logic
