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
 * per call, bounded by slot_count(nl) with fixed stack arrays — no
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
#include "wengine/logic/optimize_dedup_index.hpp"
#include "wengine/logic/optimize_exact.hpp"
#include "wengine/logic/optimize_o2_emit.hpp"
#include "wengine/logic/optimize_passes.hpp"
#include "wengine/logic/optimize_skip.hpp"
#include "wengine/logic/types.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

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

// --- O2 dedup key bucketing (016, US6) ----------------------------------
// See optimize_dedup_index.hpp (DedupIndex): phase A builds each live
// universal gate's sorted key once and folds it into a deterministic
// 64-bit FNV-1a hash; phase B visits pairs in ascending (s, t) order
// but skips hash-mismatched pairs after one integer compare, running
// the exact key compare on hash hits only. Merge decisions,
// directions, and skips are identical to the all-pairs scan —
// collisions fall through to the exact compare and never merge on
// hash equality alone.

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
 * @param[in] tie_survivors Caller-owned scratch (`slot_count(nl) * kMaxInputs`
 *            flats) for remove_gate's tie-detach sweep.
 * @param[in] bucket Caller-owned scratch (pool-sized u64): per-slot
 *            key hash, refreshed by the pass itself.
 * @param[in] bhead Caller-owned scratch (pool-sized slots): bucket
 *            chain heads, maintained with bucket.
 * @param[in] bnext Caller-owned scratch (pool-sized slots): bucket
 *            chain links, maintained with bucket.
 * @return True when any merge happened (caller re-runs to fixpoint).
 */
inline bool dedup_universal_pass_impl(Netlist &nl, GateKind u, MergeTied merge, const bool *feeds_seq, std::span<std::size_t> tie_survivors,
                                      std::span<std::uint64_t> bucket, std::span<Slot> bhead, std::span<Slot> bnext) noexcept {
  const std::size_t n = slot_count(nl);
  DedupIndex idx{bucket, bhead, bnext, n};
  if (!idx.valid()) {
    return false;
  }
  recount_fanout(nl);
  bool changed = false;
  // Phase A (DedupIndex): one sorted-key build + hash per candidate,
  // chained into buckets in ascending slot order. Non-candidates hash
  // 0 and stay out of every chain.
  idx.build(nl, u, merge);
  // Phase B: ascending s; within s, ascending bucket members above s.
  // Skipped pairs are cross-bucket (keys differ), which the all-pairs
  // loop rejects without state change — so the merge sequence is
  // identical while visits drop to in-bucket pairs. After every merge
  // the enumeration restarts from the bucket head (fresh view).
  for (Slot s = 0; s < static_cast<Slot>(n); ++s) {
    if (!nl.slots[s].live || nl.slots[s].kind != u || nl.slots[s].output_fanout == 0) {
      continue; // sinks preserved (output count stable)
    }
    if (nl.slots[s].ignored_mask != 0 || idx.bucket[s] == 0) {
      continue;
    }
    bool merged = true;
    while (merged) {
      merged = false;
      if (!nl.slots[s].live || nl.slots[s].kind != u || nl.slots[s].output_fanout == 0 ||
          nl.slots[s].ignored_mask != 0 || idx.bucket[s] == 0) {
        break; // s went quiet mid-sweep (defensive; keys are static here)
      }
      const std::size_t bkt = static_cast<std::size_t>(idx.bucket[s] % static_cast<std::uint64_t>(n));
      Slot t = idx.head[bkt];
      while (t != kNoDriver && t <= s) {
        t = idx.next[t];
      }
      while (t != kNoDriver) {
        const Slot nxt = idx.next[t];
        bool same = false;
        if (t < n && nl.slots[t].live && nl.slots[t].kind == u && nl.slots[t].output_fanout != 0 &&
            nl.slots[t].input_count == nl.slots[s].input_count && nl.slots[t].ignored_mask == 0 &&
            idx.bucket[t] != 0 && idx.bucket[t] == idx.bucket[s]) {
          // Exact compare on freshly rebuilt key sets (hash hits only):
          // collisions fall through here and never merge on hash alone.
          std::uint32_t a_sorted[kMaxInputs];
          std::uint32_t b_sorted[kMaxInputs];
          const std::uint64_t ha = dedup_sorted_key(nl, s, merge, a_sorted);
          const std::uint64_t hb = dedup_sorted_key(nl, t, merge, b_sorted);
          if (ha != 0 && ha == hb) {
            same = true;
            for (std::uint8_t i = 0; i < nl.slots[s].input_count; ++i) {
              if (a_sorted[i] != b_sorted[i]) {
                same = false;
                break;
              }
            }
          }
        }
        if (!same) {
          t = nxt;
          continue;
        }
        // Merge higher (t) into lower (s): rewire users, remove duplicate.
        // Never merge a gate feeding a sequential gate (opaque barrier).
        if (skip::seq_user(feeds_seq, t)) {
          t = nxt;
          continue;
        }
        // SkipPolicy merge freeze: never through a shared span.
        if (skip::merge_locked(nl, s, t)) {
          t = nxt;
          continue;
        }
        // Snapshot the slots whose keys the merge invalidates: t's primary
        // users (rewired onto s) and t's tie-class members (detached by
        // remove_gate, leaders re-elected), plus s itself. Cold path.
        std::vector<Slot> affected;
        affected.reserve(16);
        affected.push_back(s);
        for (Slot e = nl.succ_head[t]; e != kNoDriver && t < nl.succ_head.size(); e = nl.succ_next[e]) {
          if (nl.succ_extra[e] == 0) {
            affected.push_back(nl.succ_sink[e]);
          }
        }
        for (std::uint8_t i = 0; i < nl.slots[t].input_count; ++i) {
          const std::size_t f = tie_flat(t, i);
          if (nl.tie_rep[f] == 0) {
            continue;
          }
          const std::size_t l = tie_leader_flat(nl, f);
          for (Slot e = (l < nl.tie_head.size()) ? nl.tie_head[l] : kNoDriver; e != kNoDriver;
               e = nl.tie_next[e]) {
            affected.push_back(static_cast<Slot>(static_cast<std::size_t>(e) / kMaxInputs));
          }
        }
        rewire_users(nl, t, s);
        (void)remove_gate(nl, t, tie_survivors);
        // Selective rehash: the index is exact again before the next pair.
        for (std::size_t k = 0; k < affected.size(); ++k) {
          idx.refresh(nl, affected[k], u, merge);
        }
        changed = true;
        merged = true;
        break; // re-enumerate bucket[s] from the head (fresh view)
      }
    }
  }
  if (changed) {
    recount_fanout(nl);
    nl.dirty = true;
    nl.topo_order.clear();
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
 * @param[in] tie_survivors Caller-owned scratch (`slot_count(nl) * kMaxInputs`
 *            flats) for remove_gate's tie-detach sweep.
 * @param[in] bucket Caller-owned scratch (pool-sized u64): per-slot
 *            key hash, refreshed by the pass itself.
 * @param[in] bhead Caller-owned scratch (pool-sized slots): bucket
 *            chain heads, maintained with bucket.
 * @param[in] bnext Caller-owned scratch (pool-sized slots): bucket
 *            chain links, maintained with bucket.
 * @return True when any merge happened (caller re-runs to fixpoint).
 */
inline bool dedup_universal_pass(Netlist &nl, GateKind u, const bool *feeds_seq, std::span<std::size_t> tie_survivors,
                                 std::span<std::uint64_t> bucket, std::span<Slot> bhead, std::span<Slot> bnext) noexcept {
  return dedup_universal_pass_impl(nl, u, MergeTied::kStrict, feeds_seq, tie_survivors, bucket, bhead, bnext);
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
 * @param[in] tie_survivors Caller-owned scratch (`slot_count(nl) * kMaxInputs`
 *            flats) for remove_gate's tie-detach sweep.
 * @param[in] bucket Caller-owned scratch (pool-sized u64): per-slot
 *            key hash, refreshed by the pass itself.
 * @param[in] bhead Caller-owned scratch (pool-sized slots): bucket
 *            chain heads, maintained with bucket.
 * @param[in] bnext Caller-owned scratch (pool-sized slots): bucket
 *            chain links, maintained with bucket.
 * @return True when any merge happened (caller re-runs to fixpoint).
 */
inline bool dedup_universal_tied_pass(Netlist &nl, GateKind u, const bool *feeds_seq, std::span<std::size_t> tie_survivors,
                                      std::span<std::uint64_t> bucket, std::span<Slot> bhead, std::span<Slot> bnext) noexcept {
  return dedup_universal_pass_impl(nl, u, MergeTied::kMergeTied, feeds_seq, tie_survivors, bucket, bhead, bnext);
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
 * @param[in] tie_survivors Caller-owned scratch (`slot_count(nl) * kMaxInputs`
 *            flats) for remove_gate's tie-detach sweep.
 * @return True when any cancellation happened (caller re-runs to
 *         fixpoint; the orphaned D vanishes via DCE).
 */
inline bool double_inv_universal_pass(Netlist &nl, GateKind u, const bool *feeds_seq, std::span<std::size_t> tie_survivors) noexcept {
  recount_fanout(nl);
  bool changed = false;
  for (Slot s = 0; s < slot_count(nl); ++s) {
    Gate &g = nl.slots[s];
    if (!g.live || g.kind != u || g.output_fanout == 0 || g.input_count != 2) {
      continue;
    }
    const Slot d = g.drivers[0];
    if (d == kNoDriver || g.drivers[1] != d || d == s || d >= slot_count(nl)) {
      continue;
    }
    const Gate &dg = nl.slots[d];
    if (!dg.live || dg.kind != u || dg.input_count != 2) {
      continue;
    }
    const Slot e = dg.drivers[0];
    if (e == kNoDriver || dg.drivers[1] != e || e == s || e == d || e >= slot_count(nl)) {
      continue;
    }
    if (!nl.slots[e].live) {
      continue;
    }
    // G == E: rewire users, remove G (D becomes orphan if it only drove G;
    // later DCE removes it). Never cancel a gate feeding a sequential
    // gate (opaque barrier).
    if (skip::seq_user(feeds_seq, s)) {
      continue;
    }
    // SkipPolicy merge freeze: never cancel through a shared span.
    if (skip::shared_driver(nl, s)) {
      continue;
    }
    rewire_users(nl, s, e);
    (void)remove_gate(nl, s, tie_survivors);
    changed = true;
  }
  if (changed) {
    recount_fanout(nl);
    nl.dirty = true;
    nl.topo_order.clear();
  }
  return changed;
}


} // namespace detail

} // namespace wengine::logic
