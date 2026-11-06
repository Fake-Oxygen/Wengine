#pragma once

// Canonical topology Module for the logic netlist (C1 deepening).
//
// One Kahn skeleton, one reachability skeleton, parameterized by edge
// filter — replacing four near-identical traversals that used to live in
// eval.hpp (ensure_topology), seq_step.hpp (compile_comb_order),
// seq_state.hpp-adjacent validation, optimize_barrier.hpp
// (rebuild_seq_topology/count_comb_indeg) and netlist.hpp
// (detail::reaches/reaches_seq).
//
// EdgeFilter rule (unchanged semantics):
//   kAll      — every driver edge counts (combinational cycle check).
//   kCombOnly — edges INTO sequential-gate inputs are excluded (flop Q
//               outputs break loops, so feedback through flops validates
//               while pure combinational cycles still fail loudly).
//
// Interface: rebuild() mutates nl.topo_order/topo_level/topo_size/dirty
// exactly like the code it replaces; reaches() is pure; compile_comb_order()
// is the stepper's local order (sequential gates act as roots and never
// appear in the output). validate_sequential() stays in seq_state.hpp as
// the pure const query — the mutating Kahn here is the check, so no second
// pass is needed inside rebuild (a combinational cycle or dangling driver
// fails both identically).
//
// Depends only on types.hpp + core/status (Netlist lives in types.hpp, so
// netlist/eval/barrier/seq_step can all include this with no cycle).
// Header-only, allocation-free, exception-free, noexcept throughout.

#include "wengine/core/status.hpp"
#include "wengine/logic/types.hpp"

#include <cstddef>
#include <cstdint>

namespace wengine::logic {

enum class EdgeFilter : std::uint8_t {
  kAll = 0,
  kCombOnly = 1,
};

namespace topo {

namespace detail {

// Combinational indegree over comb-only edges (flop outputs read as
// always-ready sources, so seq-driven edges cost 0). Shared by the
// stepper order and the CombOnly rebuild below.
inline void count_comb_indeg(const Netlist &nl, std::uint8_t *indeg, std::size_t &n_comb) noexcept {
  n_comb = 0;
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    indeg[s] = 0;
    const Gate &g = nl.gates[s];
    if (!g.live || is_sequential_kind(g.kind)) {
      continue;
    }
    ++n_comb;
    std::uint8_t d = 0;
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      const std::uint16_t drv = g.drivers[i];
      if (drv == kNoDriver) {
        continue;
      }
      if (drv >= kMaxGates || !nl.gates[drv].live) {
        // Dangling-driver defect marker: saturate so the caller fails
        // loudly (indeg can never drain to zero for this gate).
        d = kMaxInputs;
        break;
      }
      if (!is_sequential_kind(nl.gates[drv].kind)) {
        ++d;
      }
    }
    indeg[s] = d;
  }
}

} // namespace detail

// Compile (or reuse) the deterministic level order: zero-indegree seeds
// scan in slot order; successors of each popped gate scan in slot order;
// FIFO queue. Levels: 0 for sources, else 1 + max(driver levels).
// kAll: Kahn over all edges (the old ensure_topology, early-out included).
// kCombOnly: sequential gates emit first as slot-order roots (level 0),
// then Kahn over comb-only edges (the old rebuild_seq_topology).
// Returns failure on any defect (dangling driver, combinational cycle);
// keeps dirty on failure for the kAll path, matching the original.
[[nodiscard]] inline core::Status rebuild(Netlist &nl, EdgeFilter filter) noexcept {
  if (filter == EdgeFilter::kAll) {
    if (!nl.dirty && nl.topo_size == nl.live_count) {
      return core::Status::success();
    }
    std::uint8_t indeg[kMaxGates] = {};
    for (std::uint16_t s = 0; s < kMaxGates; ++s) {
      const Gate &g = nl.gates[s];
      if (!g.live) {
        continue;
      }
      std::uint8_t d = 0;
      for (std::uint8_t i = 0; i < g.input_count; ++i) {
        if (g.drivers[i] != kNoDriver) {
          ++d;
        }
      }
      indeg[s] = d;
      nl.topo_level[s] = 0;
    }
    std::uint16_t queue[kMaxGates];
    std::size_t head = 0;
    std::size_t tail = 0;
    for (std::uint16_t s = 0; s < kMaxGates; ++s) {
      if (nl.gates[s].live && indeg[s] == 0) {
        queue[tail++] = s;
      }
    }
    std::size_t order_idx = 0;
    while (head < tail) {
      const std::uint16_t cur = queue[head++];
      // Depth is final now: every driver popped earlier (topo order).
      std::uint16_t depth = 0;
      const Gate &cg = nl.gates[cur];
      for (std::uint8_t i = 0; i < cg.input_count; ++i) {
        const std::uint16_t drv = cg.drivers[i];
        if (drv == kNoDriver) {
          continue;
        }
        const std::uint16_t cand = static_cast<std::uint16_t>(nl.topo_level[drv] + 1);
        if (cand > depth) {
          depth = cand;
        }
      }
      nl.topo_level[cur] = depth;
      nl.topo_order[order_idx++] = cur;
      // Successors in slot order (deterministic push order).
      for (std::uint16_t s = 0; s < kMaxGates; ++s) {
        Gate &g = nl.gates[s];
        if (!g.live || indeg[s] == 0) {
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
      }
    }
    if (order_idx != nl.live_count) {
      nl.topo_size = 0;
      nl.dirty = true;
      return core::Status::failure(core::StatusCode::kInvalidArgument);
    }
    nl.topo_size = static_cast<std::uint16_t>(order_idx);
    nl.dirty = false;
    return core::Status::success();
  }
  // kCombOnly.
  std::uint16_t n_out = 0;
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    const Gate &g = nl.gates[s];
    if (!g.live) {
      continue;
    }
    if (is_sequential_kind(g.kind)) {
      nl.topo_order[n_out++] = s;
      nl.topo_level[s] = 0;
    }
  }
  std::uint8_t indeg[kMaxGates] = {};
  std::size_t n_comb = 0;
  detail::count_comb_indeg(nl, indeg, n_comb);
  // Surface dangling-driver defects loudly (saturated marker above).
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    if (nl.gates[s].live && !is_sequential_kind(nl.gates[s].kind) && indeg[s] >= kMaxInputs) {
      return core::Status::failure(core::StatusCode::kInvalidArgument);
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
  std::size_t n_emit = 0;
  while (head < tail) {
    const std::uint16_t cur = queue[head++];
    nl.topo_order[n_out++] = cur;
    ++n_emit;
    std::uint16_t depth = 0;
    const Gate &cg = nl.gates[cur];
    for (std::uint8_t i = 0; i < cg.input_count; ++i) {
      const std::uint16_t drv = cg.drivers[i];
      if (drv == kNoDriver) {
        continue;
      }
      const std::uint16_t cand = static_cast<std::uint16_t>(nl.topo_level[drv] + 1);
      if (cand > depth) {
        depth = cand;
      }
    }
    nl.topo_level[cur] = depth;
    for (std::uint16_t s = 0; s < kMaxGates; ++s) {
      Gate &g = nl.gates[s];
      if (!g.live || is_sequential_kind(g.kind) || indeg[s] == 0) {
        continue;
      }
      for (std::uint8_t i = 0; i < g.input_count; ++i) {
        if (g.drivers[i] == cur) {
          --indeg[s];
          if (indeg[s] == 0) {
            queue[tail++] = s;
            break; // one push per gate (remaining inputs counted already)
          }
        }
      }
    }
  }
  if (n_emit != n_comb) {
    return core::Status::failure(core::StatusCode::kInvalidArgument); // combinational cycle
  }
  nl.topo_size = n_out;
  nl.dirty = false;
  return core::Status::success();
}

// Reachability over the driver graph: can `from` reach `to` following
// output->input edges? Iterative DFS on a fixed stack (no alloc).
// kCombOnly excludes edges into sequential-gate inputs from the edge set
// (flop Q outputs break loops); kAll counts every edge.
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

// Combinational-only level order for the stepper: Kahn over combinational
// edges only, same determinism as rebuild (slot-order seeds and
// successors). Sequential gates act as roots (their Q is the source value)
// and never appear in the output. Returns false on any combinational cycle
// or dangling-driver defect so stepping never evaluates a partial order.
[[nodiscard]] inline bool compile_comb_order(const Netlist &nl, std::uint16_t *order_out,
                                             std::size_t &n_out) noexcept {
  std::uint8_t indeg[kMaxGates] = {};
  std::size_t n_comb = 0;
  detail::count_comb_indeg(nl, indeg, n_comb);
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    if (nl.gates[s].live && !is_sequential_kind(nl.gates[s].kind) && indeg[s] >= kMaxInputs) {
      return false; // dangling driver defect: loud, never partial
    }
  }
  std::uint16_t queue[kMaxGates];
  std::size_t head = 0;
  std::size_t tail = 0;
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    const Gate &g = nl.gates[s];
    if (!g.live) {
      continue;
    }
    if (is_sequential_kind(g.kind)) {
      queue[tail++] = s; // roots: Q sources, never emitted
    } else if (indeg[s] == 0) {
      queue[tail++] = s;
    }
  }
  std::size_t n_emit = 0;
  while (head < tail) {
    const std::uint16_t cur = queue[head++];
    if (!is_sequential_kind(nl.gates[cur].kind)) {
      order_out[n_emit++] = cur;
    }
    for (std::uint16_t s = 0; s < kMaxGates; ++s) {
      const Gate &g = nl.gates[s];
      if (!g.live || is_sequential_kind(g.kind) || indeg[s] == 0) {
        continue;
      }
      for (std::uint8_t i = 0; i < g.input_count; ++i) {
        if (g.drivers[i] == cur) {
          --indeg[s];
          if (indeg[s] == 0) {
            queue[tail++] = s;
            break; // one push per gate (remaining inputs counted already)
          }
        }
      }
    }
  }
  n_out = n_emit;
  return n_emit == n_comb;
}

} // namespace topo

} // namespace wengine::logic
