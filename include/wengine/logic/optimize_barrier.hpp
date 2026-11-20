#pragma once

/**
 * @file
 * Opaque-barrier policy for the logic optimizer: the one rule that keeps
 * sequential gates intact through every pass. Sequential gates
 * (DFF/DLATCH/JK/T/SRLATCH) are opaque barriers: no pass reads through,
 * folds through, rewrites, or deletes a state element. State readback
 * observes flops outside the block interface, so lack of fanout never
 * makes a state element dead.
 *
 * This header is the single home for barrier mechanics so the pass
 * implementations stay combinational-only in spirit:
 * is_frozen_kind / is_barrier_root is one predicate, not N switch cases;
 * compute_feeds_seq is one O(G*fanin) reverse scan per fixpoint
 * iteration (passes read feeds_seq[s] instead of rescanning all
 * sequential users per gate); collect_barrier_roots lets DCE and O2
 * reachability share one root rule (topology itself lives in topo.hpp
 * over the EdgeFilter seam; this header only classifies barrier gates);
 * wire_fresh_with_polarity is fresh-build wiring with Q/NQ accounting
 * matching wire_pins; DedupKey / MergeTied is the explicit typed dedup
 * contract.
 *
 * Dependency rules: header-only, allocation-free, exception-free;
 * depends on eval.hpp, netlist.hpp, seq_state.hpp, and types.hpp. Logic
 * is a standalone domain: this header must never depend on CLI types.
 * Consumers: optimize_passes.hpp, the O2 build/dedup/min headers, and
 * the test_logic* suite.
 */

#include "wengine/logic/eval.hpp"
#include "wengine/logic/netlist.hpp"
#include "wengine/logic/seq_state.hpp"
#include "wengine/logic/types.hpp"

#include <cstddef>
#include <cstdint>

namespace wengine::logic::detail {

/**
 * @brief A sequential gate is frozen: passes never retype, fold, drop,
 *        bypass, fuse, dedup, or cancel through it.
 *
 * @param[in] k Kind to test.
 * @return True for DFF/DLATCH/JK/T/SRLATCH.
 */
[[nodiscard]] constexpr bool is_frozen_kind(GateKind k) noexcept { return is_sequential_kind(k); }

/**
 * @brief True when slot s is a live frozen (sequential) gate.
 *
 * @param[in] nl Netlist holding the gate.
 * @param[in] s Slot to test.
 * @return True for live sequential gates; false otherwise.
 */
[[nodiscard]] inline bool is_barrier_gate(const Netlist &nl, std::uint16_t s) noexcept {
  return s < kMaxGates && nl.gates[s].live && is_frozen_kind(nl.gates[s].kind);
}

/**
 * @brief True when slot s drives any shared-marked input (011): a primary or extra attachment into a marked input.
 *
 * Driver-side freeze predicate (the feeds_seq pattern): such a gate is
 * never removed, retagged, or rewired-through — rewire_users only
 * rewrites primaries, so touching a shared feeder would silently shrink spans.
 *
 * @param[in] nl Netlist holding drivers and extras.
 * @param[in] s Slot to test.
 * @return True when s feeds a shared input (any attachment).
 */
[[nodiscard]] inline bool drives_shared(const Netlist &nl, std::uint16_t s) noexcept {
  if (s >= kMaxGates) {
    return false;
  }
  for (std::uint16_t u = 0; u < kMaxGates; ++u) {
    const Gate &g = nl.gates[u];
    if (!g.live) {
      continue;
    }
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      const std::size_t flat = tie_flat(u, i);
      if (!is_shared_flat(nl, flat)) {
        continue;
      }
      if (g.drivers[i] == s) {
        return true;
      }
      const std::uint8_t extra_n = shared_extra_flat(nl, flat);
      for (std::uint8_t j = 0; j < extra_n; ++j) {
        if (nl.extra_drivers[flat * kMaxSharedExtra + j] == s) {
          return true;
        }
      }
    }
  }
  return false;
}

/**
 * @brief True when slot s has any shared-marked input (011).
 *
 * Sink-side skip predicate: no pass folds, drops, or retags through a
 * marked input (index-shifting transforms would desynchronize the
 * flat-keyed extras).
 *
 * @param[in] nl Netlist holding the mark table.
 * @param[in] s Slot to test.
 * @return True when any configured input of s carries the mark.
 */
[[nodiscard]] inline bool has_marked_input(const Netlist &nl, std::uint16_t s) noexcept {
  if (s >= kMaxGates || !nl.gates[s].live) {
    return false;
  }
  const Gate &g = nl.gates[s];
  for (std::uint8_t i = 0; i < g.input_count; ++i) {
    if (is_shared_flat(nl, tie_flat(s, i))) {
      return true;
    }
  }
  return false;
}

/**
 * @brief True when slot s has any pulled input (012).
 *
 * Sink-side skip predicate (the has_marked_input pattern): no pass
 * folds, drops, or retags through a pulled input (index-shifting
 * transforms would desynchronize the flat-keyed pull_cfg — the exact
 * 011 extras rationale). Pulls compose with marks; the predicates are
 * independent and both consulted at every skip site.
 *
 * @param[in] nl Netlist holding the pull table.
 * @param[in] s Slot to test.
 * @return True when any configured input of s carries a pull.
 */
[[nodiscard]] inline bool has_pulled_input(const Netlist &nl, std::uint16_t s) noexcept {
  if (s >= kMaxGates || !nl.gates[s].live) {
    return false;
  }
  const Gate &g = nl.gates[s];
  for (std::uint8_t i = 0; i < g.input_count; ++i) {
    if (is_pulled_flat(nl, tie_flat(s, i))) {
      return true;
    }
  }
  return false;
}

/**
 * @brief DCE / reachability root rule: original sinks plus every live
 *        sequential gate (an unobserved flop stays observable through
 *        state readback), plus every shared feeder (011: driver-side
 *        freeze — such gates are never removed).
 *
 * @param[in] nl Netlist holding the gate.
 * @param[in] s Slot to test.
 * @return True for live output-fanout-0 gates, live sequential gates, and live shared feeders.
 */
[[nodiscard]] inline bool is_barrier_root(const Netlist &nl, std::uint16_t s) noexcept {
  if (s >= kMaxGates || !nl.gates[s].live) {
    return false;
  }
  return nl.gates[s].output_fanout == 0 || is_frozen_kind(nl.gates[s].kind) || drives_shared(nl, s);
}

/**
 * @brief Collect the DCE/O2 root set: one bit per slot from
 *        is_barrier_root.
 *
 * @param[in] nl Netlist to scan.
 * @param[out] roots Per-slot root flags.
 */
inline void collect_barrier_roots(const Netlist &nl, bool *roots) noexcept {
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    roots[s] = is_barrier_root(nl, s);
  }
}

/**
 * @brief Generic reverse driver-cone DFS: marks every live gate
 *        reachable from any seeded gate by following drivers. Shared by
 *        DCE keep-sets and O2 reachability so the traversal lives once.
 *
 * @param[in] nl Netlist holding the driver graph.
 * @param[in] seeds Seed flags (roots to start from).
 * @param[out] keep Per-slot reachability flags.
 */
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
    // 011: extras are driver-graph edges — a pass that dropped an extra
    // would silently shrink a span (correctness, not policy).
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      const std::size_t flat = tie_flat(cur, i);
      const std::uint8_t extra_n = shared_extra_flat(nl, flat);
      for (std::uint8_t j = 0; j < extra_n; ++j) {
        const std::uint16_t d = nl.extra_drivers[flat * kMaxSharedExtra + j];
        if (d < kMaxGates && nl.gates[d].live && !keep[d]) {
          keep[d] = true;
          stack[top++] = d;
        }
      }
    }
  }
}

/**
 * @brief Reachable set for the O2 fresh build: reverse DFS from every
 *        barrier root (original sinks plus every live sequential gate,
 *        so closed state clusters with no block-output path still carry
 *        through; driver cones of kept flops follow via the DFS).
 *
 * @param[in] nl Netlist to scan.
 * @param[out] reach Per-slot reachability flags.
 */
inline void collect_reachable(const Netlist &nl, bool *reach) noexcept {
  bool seeds[kMaxGates] = {};
  collect_barrier_roots(nl, seeds);
  follow_drivers(nl, seeds, reach);
}

/**
 * @brief Single reverse scan: feeds_seq[d] is true when any live
 *        sequential gate reads gate d. Passes consult this bit instead
 *        of scanning all users.
 *
 * @param[in] nl Netlist to scan.
 * @param[out] feeds_seq Per-slot sequential-user flags.
 */
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
      // 011: extras feed the same way primaries do.
      const std::size_t flat = tie_flat(u, i);
      const std::uint8_t extra_n = shared_extra_flat(nl, flat);
      for (std::uint8_t j = 0; j < extra_n; ++j) {
        const std::uint16_t e = nl.extra_drivers[flat * kMaxSharedExtra + j];
        if (e < kMaxGates && nl.gates[e].live) {
          feeds_seq[e] = true;
        }
      }
    }
  }
}

/**
 * @brief Fresh-build wiring with polarity accounting (mirrors wire_pins
 *        without the cycle check: the O2 creation pass defers wiring
 *        until the driver map is complete, so order-dependent cycle
 *        checks would spuriously fail).
 *
 * Q edges bump output_fanout; NQ edges bump output_fanout and nq_fanout
 * to preserve the historical recount semantics DCE roots rely on.
 *
 * @param[out] out Fresh netlist receiving the wire.
 * @param[in] fresh Fresh sink slot.
 * @param[in] input 0-based sink input index.
 * @param[in] drv Fresh driver slot (kNoDriver when dangling).
 * @param[in] inv Q/NQ polarity of the sink pin (non-zero reads negated).
 */
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

/**
 * @brief Explicit dedup contract: untied dangling pins never merge (two
 *        [NoDriver,NoDriver] gates may be distinct variables); tied
 *        dangling pins merge only under kMergeTied, which the O2
 *        structural build guarantees by tying same-source duplicates at
 *        creation (sequential nets only, so pure-combinational output
 *        stays bit-identical). kStrict means dangling pins never merge
 *        (distinct variables); kMergeTied means same-source tied
 *        dangling pins merge by tie leader.
 */
enum class MergeTied : std::uint8_t { kStrict = 0, kMergeTied = 1 };

/**
 * @brief Dedup identity for one gate input: wired drivers key by driver
 *        slot, tied dangling pins by tie leader (under kMergeTied),
 *        untied dangling pins by unique (slot, input) so they never
 *        merge.
 */
struct DedupKey {
  /** @brief Key value: 0 means dead/never-match. */
  std::uint32_t v = 0; // 0 == dead/never-match
  /**
   * @brief True when the key can match (non-zero).
   *
   * @return True when v is non-zero.
   */
  [[nodiscard]] constexpr bool matchable() const noexcept { return v != 0; }
  /**
   * @brief Dead key: never matches.
   *
   * @return Zero key.
   */
  [[nodiscard]] static constexpr DedupKey dead() noexcept { return DedupKey{0}; }
  /**
   * @brief Wired-input key from the driver slot.
   *
   * @param[in] d Driver slot.
   * @return Key for a wired input.
   */
  [[nodiscard]] static constexpr DedupKey wired(std::uint16_t d) noexcept {
    return DedupKey{static_cast<std::uint32_t>(d) + 1u};
  }
  /**
   * @brief Tied-input key from the tie leader flat.
   *
   * @param[in] leader Tie leader flat.
   * @return Key for a tied dangling input.
   */
  [[nodiscard]] static constexpr DedupKey tied(std::uint32_t leader) noexcept {
    return DedupKey{0x10000u + leader};
  }
  /**
   * @brief Unique key for one (slot, input): never merges.
   *
   * @param[in] s Gate slot.
   * @param[in] i 0-based input index.
   * @return Key unique to this pin.
   */
  [[nodiscard]] static DedupKey unique(std::uint16_t s, std::uint8_t i) noexcept {
    return DedupKey{0x20000u + static_cast<std::uint32_t>(tie_flat(s, i))};
  }
};

/**
 * @brief Dedup key for one gate input under the merge contract.
 *
 * @param[in] nl Netlist holding drivers and tie classes.
 * @param[in] s Gate slot.
 * @param[in] i 0-based input index.
 * @param[in] merge kStrict (dangling pins never merge) or kMergeTied
 *            (same-source tied dangling pins merge).
 * @return Key for this input.
 */
[[nodiscard]] inline DedupKey dedup_key_for(const Netlist &nl, std::uint16_t s, std::uint8_t i, MergeTied merge) noexcept {
  // 011: inputs of a marked sink never merge — the span (primary AND
  // extras) is the identity, and the key only sees the primary.
  if (has_marked_input(nl, s)) {
    return DedupKey::unique(s, i);
  }
  // 012: inputs of a pulled sink never merge — the key ignores the
  // pull, so merging would silently drop or share a pull level. Unique
  // keys keep each pulled input's identity (O2-only path today, refused
  // on pulls at entry — defense in depth for future callers).
  if (has_pulled_input(nl, s)) {
    return DedupKey::unique(s, i);
  }
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
