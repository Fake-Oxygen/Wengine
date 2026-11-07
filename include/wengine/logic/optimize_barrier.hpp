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
//   - collect_barrier_roots: DCE + O2 reachability share one root rule.
//     (Topology itself lives in topo.hpp: rebuild/reaches/compile_comb_order
//     over the EdgeFilter seam; this header only classifies barrier gates.)
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

// Generic reverse driver-cone DFS: marks every live gate reachable from
// any seeded gate by following drivers. Shared by DCE keep-sets and O2
// reachability so the traversal lives once.
inline void follow_drivers(const Netlist &nl, const bool *seeds, bool *keep) noexcept {
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    keep[s] = false;
  }
  std::uint16_t stack[kMaxGates];
  std::size_t top = 0;
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    if (seeds[s] && nl.gates[s].live) {
      keep[s] = true;
      stack[top++] = s;
    }
  }
  while (top > 0) {
    const std::uint16_t cur = stack[--top];
    const Gate &g = nl.gates[cur];
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      const std::uint16_t d = g.drivers[i];
      if (d != kNoDriver && d < kMaxGates && nl.gates[d].live && !keep[d]) {
        keep[d] = true;
        stack[top++] = d;
      }
    }
  }
}

// Reachable set for the O2 fresh build: reverse DFS from every barrier
// root (original sinks plus every live sequential gate, so closed state
// clusters with no block-output path still carry through; driver cones of
// kept flops follow via the DFS).
inline void collect_reachable(const Netlist &nl, bool *reach) noexcept {
  bool seeds[kMaxGates] = {};
  collect_barrier_roots(nl, seeds);
  follow_drivers(nl, seeds, reach);
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
