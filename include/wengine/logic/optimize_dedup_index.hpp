#pragma once

/**
 * @file
 * DedupIndex for the logic optimizer: the one module behind the
 * dedup key-hashing + bucketing machinery. Before this seam,
 * dedup_universal_pass_impl mixed hashing (sorted-key FNV-1a),
 * bucketing (hash % n chains with unlink/refresh helpers), and merge
 * decisions in one 440-line pass — every index touch re-derived the
 * same chain discipline by hand. The merge loop now reads pairs
 * through this index; hashing/bucketing fix once, here.
 *
 * The index never merges on hash equality alone: hash hits re-run the
 * exact sorted-key compare at the merge site, so collisions fall
 * through without state change.
 *
 * Dependency rules: header-only, allocation-free, exception-free,
 * noexcept throughout. Depends only on optimize_barrier.hpp (DedupKey
 * / MergeTied / dedup_key_for) and types.hpp. Logic is a standalone
 * domain: never depends on CLI types. Consumers:
 * optimize_o2_dedup.hpp (the merge loop) and the logic optimizer
 * tests.
 */

#include "wengine/logic/optimize_barrier.hpp"
#include "wengine/logic/types.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace wengine::logic::detail {

/**
 * @brief Build one gate's sorted dedup key multiset + its hash.
 *
 * Key semantics identical to the pre-bucket inline code (same
 * matchable rule, same insertion sort). A zero hash is remapped to 1
 * so 0 stays the unmatchable sentinel.
 *
 * @param[in] nl Netlist holding drivers and ties.
 * @param[in] s Candidate gate slot.
 * @param[in] merge kStrict / kMergeTied key rule.
 * @param[out] sorted Sorted key multiset (kMaxInputs entries).
 * @return Non-zero hash when matchable; 0 when not matchable.
 */
[[nodiscard]] inline std::uint64_t dedup_sorted_key(const Netlist &nl, Slot s, MergeTied merge,
                                                    std::uint32_t (&sorted)[kMaxInputs]) noexcept {
  const Gate &a = nl.slots[s];
  for (std::uint8_t i = 0; i < a.input_count; ++i) {
    const DedupKey k = dedup_key_for(nl, s, i, merge);
    sorted[i] = k.v;
    if (!k.matchable()) {
      return 0;
    }
  }
  for (std::uint8_t i = 1; i < a.input_count; ++i) {
    std::uint32_t key = sorted[i];
    std::uint8_t j = i;
    while (j > 0 && sorted[j - 1] > key) {
      sorted[j] = sorted[j - 1];
      --j;
    }
    sorted[j] = key;
  }
  std::uint64_t h = 1469598103934665603ULL; // FNV-1a offset basis (fixed)
  const std::uint8_t hdr[2] = {static_cast<std::uint8_t>(merge), a.input_count};
  for (std::size_t k = 0; k < 2; ++k) {
    h ^= hdr[k];
    h *= 1099511628211ULL;
  }
  for (std::uint8_t i = 0; i < a.input_count; ++i) {
    for (unsigned b = 0; b < 4; ++b) {
      h ^= static_cast<std::uint64_t>((sorted[i] >> (b * 8u)) & 0xFFu);
      h *= 1099511628211ULL;
    }
  }
  return (h != 0) ? h : 1;
}

/**
 * @brief Caller-owned dedup bucket index: per-slot key hashes chained
 *        into (hash % n) buckets in ascending slot order.
 *
 * Non-candidates hash 0 and stay out of every chain. After every merge
 * the caller refreshes the affected slots, so the chains are exact
 * before the next pair — enumeration restarts from the bucket head
 * (fresh view). Skipped pairs are cross-bucket (keys differ), which
 * the all-pairs scan rejects without state change, so the merge
 * sequence is identical to the all-pairs scan while visits drop to
 * in-bucket pairs.
 */
struct DedupIndex {
  /** @brief Per-slot key hash (0 = non-candidate, out of every chain). */
  std::span<std::uint64_t> bucket;
  /** @brief Bucket chain heads (hash % n). */
  std::span<Slot> head;
  /** @brief Bucket chain links. */
  std::span<Slot> next;
  /** @brief Pool size the spans cover. */
  std::size_t n = 0;

  /**
   * @brief True when the spans cover the pool (loud refusal otherwise).
   *
   * @return True when bucket/head/next each cover n slots and n > 0.
   */
  [[nodiscard]] bool valid() const noexcept {
    return n != 0 && bucket.size() >= n && head.size() >= n && next.size() >= n;
  }

  /**
   * @brief Phase A: one sorted-key build + hash per candidate, chained
   *        into buckets in ascending slot order.
   *
   * @param[in] nl Netlist holding drivers and ties.
   * @param[in] u Universal target kind (only U gates index).
   * @param[in] merge kStrict / kMergeTied key rule.
   */
  void build(const Netlist &nl, GateKind u, MergeTied merge) noexcept {
    for (std::size_t i = 0; i < n; ++i) {
      head[i] = kNoDriver;
      bucket[i] = 0;
    }
    for (Slot s = static_cast<Slot>(n); s-- > 0;) {
      const Gate &a = nl.slots[s];
      if (!a.live || a.kind != u || a.ignored_mask != 0) {
        continue;
      }
      std::uint32_t sorted[kMaxInputs];
      const std::uint64_t h = dedup_sorted_key(nl, s, merge, sorted);
      if (h == 0) {
        continue;
      }
      bucket[s] = h;
      const std::size_t idx = static_cast<std::size_t>(h % static_cast<std::uint64_t>(n));
      next[s] = head[idx];
      head[idx] = s;
    }
  }

  /**
   * @brief Unlink one slot from its bucket chain.
   *
   * @param[in] x Slot to unlink (no-op when unindexed).
   */
  void unlink(Slot x) noexcept {
    const std::uint64_t h = (static_cast<std::size_t>(x) < n) ? bucket[x] : 0;
    if (h == 0) {
      return;
    }
    const std::size_t idx = static_cast<std::size_t>(h % static_cast<std::uint64_t>(n));
    Slot *link = &head[idx];
    while (*link != kNoDriver && *link != x) {
      link = &next[*link];
    }
    if (*link == x) {
      *link = next[x];
      next[x] = kNoDriver;
    }
  }

  /**
   * @brief Rebuild one slot's hash and bucket membership from the
   *        current tables (call after every merge for affected slots).
   *
   * @param[in] nl Netlist holding drivers and ties.
   * @param[in] x Slot to refresh.
   * @param[in] u Universal target kind.
   * @param[in] merge kStrict / kMergeTied key rule.
   */
  void refresh(const Netlist &nl, Slot x, GateKind u, MergeTied merge) noexcept {
    unlink(x);
    bucket[x] = 0;
    if (static_cast<std::size_t>(x) >= n || !nl.slots[x].live || nl.slots[x].kind != u ||
        nl.slots[x].ignored_mask != 0) {
      return;
    }
    std::uint32_t sorted[kMaxInputs];
    const std::uint64_t h = dedup_sorted_key(nl, x, merge, sorted);
    if (h == 0) {
      return;
    }
    bucket[x] = h;
    const std::size_t idx = static_cast<std::size_t>(h % static_cast<std::uint64_t>(n));
    Slot *link = &head[idx];
    while (*link != kNoDriver && *link < x) {
      link = &next[*link];
    }
    next[x] = *link;
    *link = x;
  }
};

} // namespace wengine::logic::detail
