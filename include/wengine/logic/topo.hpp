#pragma once

// Canonical topology Module for the logic netlist (C1 deepening).
//
// One Kahn core (`detail::kahn_pure`), three direct entries — no
// cross-header forwarders:
//   rebuild()            mutating compile into nl.topo_order/level/size/dirty.
//   validate()           pure verdict over the same core, no mutation.
//   compile_comb_order() pure combinational order for the stepper.
// reaches() is the pure reachability query (unchanged).
//
// EdgeFilter rule:
//   kAll      — every driver edge counts (combinational cycle check).
//   kCombOnly — edges INTO sequential-gate inputs are excluded (flop Q
//               outputs break loops, so feedback through flops validates
//               while pure combinational cycles still fail loudly).
// Sequential-driven edges cost 0 under kCombOnly (flop Q is always ready;
// the old validate_sequential counted them at 1 with the same verdict but
// a different schedule — 0-cost is canonical since the stepper and the
// optimizer already used it).
//
// Failure contracts (preserved):
//   rebuild/kAll:   zeroes topo_size, keeps dirty, fails loudly.
//   rebuild/kCombOnly: leaves dirty/size untouched on failure.
//   validate/compile: pure, never touch the netlist.
//
// Depends only on types.hpp + core/status (Netlist lives in types.hpp, so
// netlist/eval/seq_step/cli can all include this with no cycle).
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
// always-ready sources, so seq-driven edges cost 0). Shared by every
// kCombOnly entry below.
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

// How sequential gates appear in the output order.
enum class SeqOut : std::uint8_t {
  kNone = 0,       // kAll path: no special-casing, every gate is emitted.
  kPrependSeq = 1, // rebuild/kCombOnly: seq gates emitted first as roots.
  kSkipSeq = 2,    // stepper order: seq gates seed readiness but are skipped.
};

// The one Kahn skeleton. Pure: reads nl, writes caller buffers, never
// mutates. Slot-order seeds and successors (FIFO) everywhere, so every
// entry point shares one determinism story. Levels: 0 for sources (and
// seq roots under kCombOnly), else 1 + max(driver levels).
// Returns false on any defect (dangling driver, combinational cycle).
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
      std::uint8_t d = 0;
      for (std::uint8_t i = 0; i < g.input_count; ++i) {
        if (g.drivers[i] != kNoDriver) {
          ++d;
        }
      }
      indeg[s] = d;
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
      if (level_out != nullptr) {
        level_out[cur] = depth;
      }
      order_out[n_emit++] = cur;
      for (std::uint16_t s = 0; s < kMaxGates; ++s) {
        const Gate &g = nl.gates[s];
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
    if (nl.gates[s].live && !is_sequential_kind(nl.gates[s].kind) && indeg[s] >= kMaxInputs) {
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
    if (level_out != nullptr) {
      level_out[cur] = depth;
    }
    order_out[n_emit++] = cur;
    ++n_comb_emit;
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
  return n_comb_emit == n_comb;
}

} // namespace detail

// Compile (or reuse) the deterministic level order: zero-indegree seeds
// scan in slot order; successors of each popped gate scan in slot order;
// FIFO queue.
// kAll: Kahn over all edges (early-out included).
// kCombOnly: sequential gates emit first as slot-order roots (level 0),
// then Kahn over comb-only edges.
// Returns failure on any defect (dangling driver, combinational cycle);
// keeps dirty on failure for the kAll path.
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

// Pure verdict over the same core: no mutation, fixed stack arrays,
// noexcept. kAll sees every edge (combinational cycle check);
// kCombOnly excludes flop-breaking edges (feedback through flops
// validates, pure combinational cycles still fail loudly).
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
  return detail::kahn_pure(nl, EdgeFilter::kCombOnly, order_out, nullptr, n_out,
                           detail::SeqOut::kSkipSeq);
}

} // namespace topo

} // namespace wengine::logic
