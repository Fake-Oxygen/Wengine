#pragma once

#include "wengine/core/status.hpp"
#include "wengine/logic/netlist.hpp"
#include "wengine/logic/optimize_types.hpp"
#include "wengine/logic/optimize_barrier.hpp"
#include "wengine/logic/optimize_exact.hpp"
#include "wengine/logic/optimize_o2_emit.hpp"
#include "wengine/logic/optimize_passes.hpp"
#include "wengine/logic/types.hpp"

#include <cstddef>
#include <cstdint>

namespace wengine::logic {

namespace detail {

// --- O2 post-cleanup (vocabulary-preserving) ---------------------------
// Merge identical universal gates (non-sinks) and cancel double inverters
// U(U(E,E),U(E,E)) == E for internal gates. Both preserve the target
// vocabulary (all gates stay U) and block output count (sinks are never
// merged). Deterministic (slot order), allocation-free, noexcept.
// Dangling inputs never merge under kStrict (two [NoDriver,NoDriver]
// gates may be distinct variables); under kMergeTied, tied dangling pins
// merge by tie leader (sound because the O2 structural build ties
// same-source duplicates at creation; sequential nets only, so
// pure-combinational output stays bit-identical).
inline bool dedup_universal_pass_impl(Netlist &nl, GateKind u, MergeTied merge,
                                      const bool *feeds_seq) noexcept {
  recount_fanout(nl);
  bool changed = false;
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    Gate &a = nl.gates[s];
    if (!a.live || a.kind != u || a.output_fanout == 0) {
      continue; // sinks preserved (output count stable)
    }
    if (a.ignored_mask != 0) {
      continue;
    }
    // Sorted key multiset for order-insensitive (commutative) compare.
    std::uint32_t a_sorted[kMaxInputs];
    bool a_matchable = true;
    for (std::uint8_t i = 0; i < a.input_count; ++i) {
      const DedupKey k = dedup_key_for(nl, s, i, merge);
      a_sorted[i] = k.v;
      if (!k.matchable()) {
        a_matchable = false;
        break;
      }
    }
    if (!a_matchable) {
      continue; // dead driver (or dangling with merging off): never merge
    }
    for (std::uint8_t i = 1; i < a.input_count; ++i) {
      std::uint32_t key = a_sorted[i];
      std::uint8_t j = i;
      while (j > 0 && a_sorted[j - 1] > key) {
        a_sorted[j] = a_sorted[j - 1];
        --j;
      }
      a_sorted[j] = key;
    }
    for (std::uint16_t t = static_cast<std::uint16_t>(s + 1); t < kMaxGates; ++t) {
      Gate &b = nl.gates[t];
      if (!b.live || b.kind != u || b.output_fanout == 0) {
        continue;
      }
      if (b.input_count != a.input_count || b.ignored_mask != 0) {
        continue;
      }
      std::uint32_t b_sorted[kMaxInputs];
      bool b_matchable = true;
      for (std::uint8_t i = 0; i < b.input_count; ++i) {
        const DedupKey k = dedup_key_for(nl, t, i, merge);
        b_sorted[i] = k.v;
        if (!k.matchable()) {
          b_matchable = false;
          break;
        }
      }
      if (!b_matchable) {
        continue;
      }
      for (std::uint8_t i = 1; i < b.input_count; ++i) {
        std::uint32_t key = b_sorted[i];
        std::uint8_t j = i;
        while (j > 0 && b_sorted[j - 1] > key) {
          b_sorted[j] = b_sorted[j - 1];
          --j;
        }
        b_sorted[j] = key;
      }
      bool same = true;
      for (std::uint8_t i = 0; i < a.input_count; ++i) {
        if (a_sorted[i] != b_sorted[i]) {
          same = false;
          break;
        }
      }
      if (!same) {
        continue;
      }
      // Merge higher (t) into lower (s): rewire users, remove duplicate.
      // Never merge a gate feeding a sequential gate (opaque barrier).
      if (feeds_seq != nullptr && feeds_seq[t]) {
        continue;
      }
      rewire_users(nl, t, s);
      (void)remove_gate(nl, t);
      changed = true;
    }
  }
  if (changed) {
    recount_fanout(nl);
    nl.dirty = true;
    nl.topo_size = 0;
  }
  return changed;
}

// Named dedup entry points replacing the old bool flag: strict keeps
// pure-combinational output bit-identical (dangling pins never merge);
// tied merges same-source tied dangling pins (sequential nets only, where
// the structural build ties duplicates at creation so toggles converge).
inline bool dedup_universal_pass(Netlist &nl, GateKind u, const bool *feeds_seq = nullptr) noexcept {
  return dedup_universal_pass_impl(nl, u, MergeTied::kStrict, feeds_seq);
}

inline bool dedup_universal_tied_pass(Netlist &nl, GateKind u, const bool *feeds_seq = nullptr) noexcept {
  return dedup_universal_pass_impl(nl, u, MergeTied::kMergeTied, feeds_seq);
}

// Cancel internal double inverters: G=U(D,D), D=U(E,E) with D,E wired live
// gates => G==E. Only non-sinks (preserves output slots/count); dangling
// bases (E==NoDriver) are skipped to avoid interface churn.
// feeds_seq is the precomputed barrier bit (nullptr == no sequential gates).
inline bool double_inv_universal_pass(Netlist &nl, GateKind u, const bool *feeds_seq = nullptr) noexcept {
  recount_fanout(nl);
  bool changed = false;
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    Gate &g = nl.gates[s];
    if (!g.live || g.kind != u || g.output_fanout == 0 || g.input_count != 2) {
      continue;
    }
    const std::uint16_t d = g.drivers[0];
    if (d == kNoDriver || g.drivers[1] != d || d == s || d >= kMaxGates) {
      continue;
    }
    const Gate &dg = nl.gates[d];
    if (!dg.live || dg.kind != u || dg.input_count != 2) {
      continue;
    }
    const std::uint16_t e = dg.drivers[0];
    if (e == kNoDriver || dg.drivers[1] != e || e == s || e == d || e >= kMaxGates) {
      continue;
    }
    if (!nl.gates[e].live) {
      continue;
    }
    // G == E: rewire users, remove G (D becomes orphan if it only drove G;
    // later DCE removes it). Never cancel a gate feeding a sequential
    // gate (opaque barrier).
    if (feeds_seq != nullptr && feeds_seq[s]) {
      continue;
    }
    rewire_users(nl, s, e);
    (void)remove_gate(nl, s);
    changed = true;
  }
  if (changed) {
    recount_fanout(nl);
    nl.dirty = true;
    nl.topo_size = 0;
  }
  return changed;
}


} // namespace detail

} // namespace wengine::logic
