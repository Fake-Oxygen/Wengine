#pragma once

/**
 * @file
 * O2 post-cleanup passes that preserve the single-kind vocabulary
 * (LOGIC-4): the dedup stage of the O2 fresh build inside the optimize
 * stage of the logic pipeline (netlist parse -> state -> eval ->
 * layout/topo -> optimize). After the O2 emitter (optimize_o2_emit.hpp)
 * rebuilds the net in NAND-only/NOR-only form, these passes shrink the
 * result without leaving the target vocabulary: identical universal
 * gates merge (non-sinks only) and internal double inverters
 * U(U(E,E),U(E,E)) cancel back to E.
 *
 * What each pass preserves: the truth table exactly, the target
 * vocabulary (every gate stays the universal kind U), and the block
 * output count (sinks are never merged, so outputs stay positional).
 * Cost class: quadratic pairwise compare over live universal gates
 * per call, bounded by kMaxGates with fixed stack arrays — no
 * allocation, deterministic slot order throughout.
 *
 * Merge contract (optimize_barrier.hpp DedupKey/MergeTied): dangling
 * inputs never merge under kStrict (two [NoDriver,NoDriver] gates may
 * be distinct variables); under kMergeTied, tied dangling pins merge
 * by tie leader (sound because the O2 structural build ties
 * same-source duplicates at creation; sequential nets only, so
 * pure-combinational output stays bit-identical). Gates feeding a
 * sequential gate are never merged or cancelled (opaque barrier:
 * feeds_seq guards every removal).
 *
 * Dependency rules: header-only, allocation-free, exception-free,
 * noexcept throughout. Lives in the wengine::logic::detail seam —
 * internal to the optimizer family. Never depends on CLI types.
 * Consumers: optimize_o2_build.hpp post_cleanup and the logic
 * optimizer tests.
 */

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
/**
 * @brief One O2 dedup sweep: merge identical non-sink universal gates.
 *
 * Sorted-key multiset compare makes the match order-insensitive
 * (commutative kinds); merge direction is always higher slot into
 * lower slot. Sinks are never merged (output slots and count stay
 * stable for positional truth-equivalence checks); gates feeding a
 * sequential gate are never merged (opaque barrier).
 *
 * @param[in,out] nl Netlist being cleaned; mutated only on merges.
 * @param[in] u Universal target kind (kNand or kNor).
 * @param[in] merge kStrict keeps dangling pins distinct; kMergeTied
 *                  merges same-tie-class dangling pins.
 * @param[in] feeds_seq Precomputed feeds-sequential bits (nullptr when
 *                     the net has no sequential gates).
 * @return True when any merge happened (caller re-runs to fixpoint).
 */
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
      // 011: never merge through a shared span (unreachable today — O2
      // refuses marked netlists upfront — but the freeze is structural).
      if (drives_shared(nl, t) || drives_shared(nl, s)) {
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

/**
 * @brief Strict dedup entry point (dangling pins never merge).
 *
 * Named dedup entry points replacing the old bool flag: strict keeps
 * pure-combinational output bit-identical (dangling pins never merge);
 * tied merges same-source tied dangling pins (sequential nets only,
 * where the structural build ties duplicates at creation so toggles
 * converge).
 *
 * @param[in,out] nl Netlist being cleaned; mutated only on merges.
 * @param[in] u Universal target kind (kNand or kNor).
 * @param[in] feeds_seq Precomputed feeds-sequential bits (nullptr when
 *                     the net has no sequential gates).
 * @return True when any merge happened (caller re-runs to fixpoint).
 */
inline bool dedup_universal_pass(Netlist &nl, GateKind u, const bool *feeds_seq = nullptr) noexcept {
  return dedup_universal_pass_impl(nl, u, MergeTied::kStrict, feeds_seq);
}

/**
 * @brief Tied-dangling dedup entry point (same-tie-class pins merge).
 *
 * Merges same-source tied dangling pins by tie leader — used on
 * sequential nets, where the O2 structural build tied duplicated pins
 * at creation, so repeated NAND/NOR toggles converge instead of
 * multiplying gates.
 *
 * @param[in,out] nl Netlist being cleaned; mutated only on merges.
 * @param[in] u Universal target kind (kNand or kNor).
 * @param[in] feeds_seq Precomputed feeds-sequential bits (nullptr when
 *                     the net has no sequential gates).
 * @return True when any merge happened (caller re-runs to fixpoint).
 */
inline bool dedup_universal_tied_pass(Netlist &nl, GateKind u, const bool *feeds_seq = nullptr) noexcept {
  return dedup_universal_pass_impl(nl, u, MergeTied::kMergeTied, feeds_seq);
}

/**
 * @brief Cancel internal double inverters: G=U(D,D), D=U(E,E)
 *        implies G==E, so rewire G's users to E and drop G.
 *
 * Only non-sinks fire (preserves output slots/count); dangling bases
 * (E==NoDriver) are skipped to avoid interface churn. Gates feeding a
 * sequential gate are never cancelled (opaque barrier).
 *
 * @param[in,out] nl Netlist being cleaned; mutated only on cancels.
 * @param[in] u Universal target kind (kNand or kNor).
 * @param[in] feeds_seq Precomputed feeds-sequential bits (nullptr when
 *                     the net has no sequential gates).
 * @return True when any cancellation happened (caller re-runs to
 *         fixpoint; the orphaned D vanishes via DCE).
 */
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
    // 011: never cancel through a shared span (see the dedup guard above).
    if (drives_shared(nl, s)) {
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
