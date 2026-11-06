#pragma once

// Opaque-barrier policy for the logic optimizer (LOGIC-V2-SEQ11).
//
// Sequential gates (DFF/DLATCH/JK/T/SRLATCH) are opaque barriers: no pass
// reads through, folds through, rewrites, or deletes a state element.
// State readback observes flops outside the block interface, so lack of
// fanout never makes a state element dead.
//
// This header is the single home for barrier mechanics so the pass
// implementations stay combinational-only in spirit:
//   - is_frozen_kind / is_barrier_root: one predicate, not N switch cases.
//   - compute_feeds_seq: one O(G*fanin) reverse scan per fixpoint
//     iteration; passes read feeds_seq[s] instead of rescanning all
//     sequential users per gate (the old has_live_seq_user hot loop).
//   - collect_barrier_roots / collect_reachable_roots: DCE + O2 reachability
//     share one root rule.
//   - rebuild_seq_topology: canonical sequential-aware Kahn order. Nets
//     without sequential gates delegate to ensure_topology bit-identically;
//     otherwise validate_sequential + slot-order seq roots + combinational
//     Kahn over comb-only edges (same determinism as ensure_topology and
//     seq_step::compile_comb_order: slot-order seeds and successors).
//   - wire_fresh_with_polarity: fresh-build wiring with Q/NQ accounting
//     matching wire_pins (Q edges bump output_fanout, NQ edges bump both
//     output_fanout and nq_fanout to preserve the historical recount
//     semantics used by DCE roots).
//   - DedupKey / MergeTied: explicit typed contract replacing the old
//     bool flag + magic 0x10000/0x20000 offsets.
//
// Header-only, allocation-free, exception-free, noexcept throughout.

#include "wengine/logic/eval.hpp"
#include "wengine/logic/netlist.hpp"
#include "wengine/logic/seq_state.hpp"
#include "wengine/logic/types.hpp"

#include <cstddef>
#include <cstdint>

namespace wengine::logic::detail {

// A sequential gate is frozen: passes never retype, fold, drop, bypass,
// fuse, dedup, or cancel through it.
[[nodiscard]] constexpr bool is_frozen_kind(GateKind k) noexcept { return is_sequential_kind(k); }

[[nodiscard]] inline bool is_barrier_gate(const Netlist &nl, std::uint16_t s) noexcept {
  return s < kMaxGates && nl.gates[s].live && is_frozen_kind(nl.gates[s].kind);
}

// DCE / reachability root rule: original sinks plus every live sequential
// gate (an unobserved flop stays observable through state readback).
[[nodiscard]] inline bool is_barrier_root(const Netlist &nl, std::uint16_t s) noexcept {
  if (s >= kMaxGates || !nl.gates[s].live) {
    return false;
  }
  return nl.gates[s].output_fanout == 0 || is_frozen_kind(nl.gates[s].kind);
}

inline void collect_barrier_roots(const Netlist &nl, bool *roots) noexcept {
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    roots[s] = is_barrier_root(nl, s);
  }
}

// Single reverse scan: feeds_seq[d] is true when any live sequential gate
// reads gate d. Passes consult this bit instead of scanning all users.
inline void compute_feeds_seq(const Netlist &nl, bool *feeds_seq) noexcept {
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    feeds_seq[s] = false;
  }
  for (std::uint16_t u = 0; u < kMaxGates; ++u) {
    const Gate &user = nl.gates[u];
    if (!user.live || !is_frozen_kind(user.kind)) {
      continue;
    }
    for (std::uint8_t i = 0; i < user.input_count; ++i) {
      const std::uint16_t d = user.drivers[i];
      if (d < kMaxGates && nl.gates[d].live) {
        feeds_seq[d] = true;
      }
    }
  }
}

// Combinational indegree over comb-only edges (flop outputs read as
// always-ready sources, so seq-driven edges cost 0). Shared by the
// stepper order and the optimizer rebuild below.
inline void count_comb_indeg(const Netlist &nl, std::uint8_t *indeg, std::size_t &n_comb) noexcept {
  n_comb = 0;
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    indeg[s] = 0;
    const Gate &g = nl.gates[s];
    if (!g.live || is_frozen_kind(g.kind)) {
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
      if (!is_frozen_kind(nl.gates[drv].kind)) {
        ++d;
      }
    }
    indeg[s] = d;
  }
}

// Canonical sequential-aware rebuild. Returns false on genuine defects
// (validate_sequential failure, dangling driver, combinational cycle);
// never leaves a partial order on failure paths the caller treats as loud.
inline bool rebuild_seq_topology(Netlist &nl) noexcept {
  if (!has_sequential_gates(nl)) {
    return ensure_topology(nl).ok();
  }
  if (!validate_sequential(nl).ok()) {
    return false;
  }
  std::uint16_t n_out = 0;
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    const Gate &g = nl.gates[s];
    if (!g.live) {
      continue;
    }
    if (is_frozen_kind(g.kind)) {
      nl.topo_order[n_out++] = s;
      nl.topo_level[s] = 0;
    }
  }
  std::uint8_t indeg[kMaxGates] = {};
  std::size_t n_comb = 0;
  count_comb_indeg(nl, indeg, n_comb);
  // Surface dangling-driver defects loudly (saturated marker above).
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    if (nl.gates[s].live && !is_frozen_kind(nl.gates[s].kind) && indeg[s] >= kMaxInputs) {
      return false;
    }
  }
  std::uint16_t queue[kMaxGates];
  std::size_t head = 0;
  std::size_t tail = 0;
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    const Gate &g = nl.gates[s];
    if (!g.live || is_frozen_kind(g.kind)) {
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
      if (!g.live || is_frozen_kind(g.kind) || indeg[s] == 0) {
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
    return false; // combinational cycle: loud, never partial
  }
  nl.topo_size = n_out;
  nl.dirty = false;
  return true;
}

// Fresh-build wiring with polarity accounting (mirrors wire_pins without
// the cycle check: the O2 creation pass defers wiring until the driver map
// is complete, so order-dependent cycle checks would spuriously fail).
// Q edges bump output_fanout; NQ edges bump output_fanout and nq_fanout to
// preserve the historical recount semantics DCE roots rely on.
inline void wire_fresh_with_polarity(Netlist &out, std::uint16_t fresh, std::uint8_t input,
                                     std::uint16_t drv, std::uint8_t inv) noexcept {
  if (fresh >= kMaxGates || input >= kMaxInputs) {
    return;
  }
  out.gates[fresh].drivers[input] = drv;
  out.driver_inv[tie_flat(fresh, input)] = inv;
  if (drv < kMaxGates && out.gates[drv].live) {
    ++out.gates[drv].output_fanout;
    if (inv != 0) {
      ++out.gates[drv].nq_fanout;
    }
  }
}

// Explicit dedup contract: untied dangling pins never merge (two
// [NoDriver,NoDriver] gates may be distinct variables); tied dangling pins
// merge only under kMergeTied, which the O2 structural build guarantees by
// tying same-source duplicates at creation (sequential nets only, so
// pure-combinational output stays bit-identical).
enum class MergeTied : std::uint8_t { kStrict = 0, kMergeTied = 1 };

struct DedupKey {
  std::uint32_t v = 0; // 0 == dead/never-match
  [[nodiscard]] constexpr bool matchable() const noexcept { return v != 0; }
  [[nodiscard]] static constexpr DedupKey dead() noexcept { return DedupKey{0}; }
  [[nodiscard]] static constexpr DedupKey wired(std::uint16_t d) noexcept {
    return DedupKey{static_cast<std::uint32_t>(d) + 1u};
  }
  [[nodiscard]] static constexpr DedupKey tied(std::uint32_t leader) noexcept {
    return DedupKey{0x10000u + leader};
  }
  [[nodiscard]] static DedupKey unique(std::uint16_t s, std::uint8_t i) noexcept {
    return DedupKey{0x20000u + static_cast<std::uint32_t>(tie_flat(s, i))};
  }
};

[[nodiscard]] inline DedupKey dedup_key_for(const Netlist &nl, std::uint16_t s, std::uint8_t i,
                                            MergeTied merge) noexcept {
  const std::uint16_t d = nl.gates[s].drivers[i];
  if (d != kNoDriver) {
    return (d < kMaxGates && nl.gates[d].live) ? DedupKey::wired(d) : DedupKey::dead();
  }
  if (merge == MergeTied::kStrict) {
    return DedupKey::unique(s, i); // never merges
  }
  return DedupKey::tied(static_cast<std::uint32_t>(tie_leader_flat(nl, tie_flat(s, i))));
}

} // namespace wengine::logic::detail
