#pragma once

/**
 * @file
 * TieSet seam (see GLOSSARY.md) of the logic pipeline: input-input shorts as first-class
 * nets. Only this header may read/write Netlist::tie_rep directly;
 * everyone else (netlist mutation, optimizer passes, eval, stepper,
 * CLI) calls the functions below. Star encoding: tie_rep[flat] == 0
 * means singleton (untied); otherwise it holds leader_flat + 1 where
 * leader_flat is the class canonical rep (lowest slot/input, slot-order
 * enumeration). Zero-initialization is the all-singleton state, so
 * Netlist{} starts untied and reset_netlist clears to zero. One block
 * input per class (leaders only) flows into the derived interface
 * (block_interface.hpp); scalar and bit-parallel eval feed/share one
 * value per class.
 *
 * Capacity (015): dynamic pool; every bound below is slot_count(nl),
 * never kMaxGates (the loud max cap only bounds growth via ensure_slots).
 * Scratch-taking cores (untie_pin, detach_slot_ties, reindex_kept_ties,
 * ignore_tie_class) take caller-owned survivor/member spans sized
 * slot_count(nl) * kMaxInputs.
 *
 * Dependency rules (docs/logic.md section 4): header-only,
 * allocation-free, exception-free; depends only on types.hpp plus
 * shared_net.hpp (mark/tie disjointness) plus core/status (never
 * netlist.hpp: PinRef lives in types.hpp precisely to keep this
 * include one-directional). Logic is a standalone domain:
 * this header must never depend on CLI types. Consumers: netlist.hpp
 * mutation, the O0/O1/O2 optimizer passes, eval.hpp, seq_step.hpp, the
 * CLI tie/untie verbs, and the test_logic* suite.
 */

#include "wengine/core/status.hpp"
#include "wengine/logic/shared_net.hpp"
#include "wengine/logic/types.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace wengine::logic {

/**
 * @brief Whole-table tie snapshot: tie_pins can merge two classes, so
 *        a pin-level untie cannot revert a merge — callers snapshot and
 *        restore wholesale instead. Dynamic pool: vector<Slot> sized
 *        slot_count(nl) * kMaxInputs.
 */
using TieSnapshot = std::vector<Slot>;

/**
 * @brief Copy the whole tie table for a later wholesale restore.
 *
 * @param[in] nl Netlist to snapshot.
 * @return Copy of nl.tie_rep.
 */
[[nodiscard]] inline TieSnapshot snapshot_ties(const Netlist &nl) noexcept {
  return nl.tie_rep;
}

inline void tie_chains_rebuild(Netlist &nl);

/**
 * @brief Restore a whole-table snapshot taken by snapshot_ties.
 *
 * Atomic: installs the star table and rebuilds the member chains
 * internally, so readers never observe a star/chains split-brain.
 * (No noexcept: the chain rebuild may allocate on first growth;
 * steady-state callers find everything sized.)
 *
 * @param[out] nl Netlist receiving the snapshot.
 * @param[in] snap Snapshot to install.
 */
inline void restore_ties(Netlist &nl, const TieSnapshot &snap) {
  nl.tie_rep = snap;
  tie_chains_rebuild(nl);
}

/**
 * @brief Structural equality of two tie tables (save/load round-trip).
 *
 * @param[in] a First netlist.
 * @param[in] b Second netlist.
 * @return True when every tie_rep entry agrees.
 */
[[nodiscard]] inline bool ties_equal(const Netlist &a, const Netlist &b) noexcept {
  return a.tie_rep == b.tie_rep;
}

/**
 * @brief Canonical leader flat of the class holding flat (flat itself
 *        for singletons).
 *
 * @param[in] nl Netlist holding the tie table.
 * @param[in] flat Flat pin index from tie_flat.
 * @return Leader flat of flat's class.
 */
[[nodiscard]] inline std::size_t tie_leader_flat(const Netlist &nl, std::size_t flat) noexcept {
  const Slot rep = nl.tie_rep[flat];
  if (rep == 0) {
    return flat;
  }
  return static_cast<std::size_t>(rep - 1u);
}

/**
 * @brief True when (slot, input) belongs to a tie class (non-singleton).
 *
 * @param[in] nl Netlist holding the tie table.
 * @param[in] slot Gate slot.
 * @param[in] input 0-based input index.
 * @return True when tied; false for singletons and out-of-range pins.
 */
[[nodiscard]] inline bool is_tied(const Netlist &nl, Slot slot, std::uint8_t input) noexcept {
  if (slot >= slot_count(nl) || input >= kMaxInputs) {
    return false;
  }
  return nl.tie_rep[tie_flat(slot, input)] != 0;
}

/**
 * @brief True when (slot, input) is its class canonical leader.
 *
 * @param[in] nl Netlist holding the tie table.
 * @param[in] slot Gate slot.
 * @param[in] input 0-based input index.
 * @return True when the pin leaders itself (singletons included).
 */
[[nodiscard]] inline bool is_tie_leader(const Netlist &nl, Slot slot, std::uint8_t input) noexcept {
  const std::size_t flat = tie_flat(slot, input);
  return tie_leader_flat(nl, flat) == flat;
}

/**
 * @brief Slot-order member enumeration for one class leader.
 *
 * Reads the maintained chain (016): ascending member flats, head at
 * the class minimum. Singletons enumerate themselves when the pin is
 * live, dangling, and non-ignored (the pre-chain rule, preserved
 * exactly); non-leader flats enumerate nothing.
 *
 * @param[in] nl Netlist holding the tie table.
 * @param[in] leader Class leader flat to enumerate.
 * @param[out] out Caller buffer receiving member flats.
 * @param[in] cap Capacity of out.
 * @return Total member count (may exceed cap; only the first cap
 *         entries are stored). Pure readout.
 */
[[nodiscard]] inline std::size_t tie_class_members(const Netlist &nl, std::size_t leader, std::size_t *out, std::size_t cap) noexcept {
  if (leader >= nl.tie_head.size()) {
    return 0;
  }
  if (nl.tie_head[leader] != kNoDriver) {
    std::size_t n = 0;
    for (Slot e = nl.tie_head[leader]; e != kNoDriver; e = nl.tie_next[e]) {
      if (n < cap) {
        out[n] = static_cast<std::size_t>(e);
      }
      ++n;
    }
    return n;
  }
  // Singleton or non-leader: exactly the pre-chain rule.
  if (nl.tie_rep[leader] != 0) {
    return 0; // non-leader member: enumerates nothing
  }
  const auto slot = static_cast<Slot>(leader / kMaxInputs);
  const auto input = static_cast<std::uint8_t>(leader % kMaxInputs);
  if (slot >= slot_count(nl) || !nl.slots[slot].live || input >= nl.slots[slot].input_count) {
    return 0;
  }
  const Gate &g = nl.slots[slot];
  if (g.drivers[input] != kNoDriver || (g.ignored_mask & static_cast<std::uint8_t>(1u << input)) != 0) {
    return 0;
  }
  if (cap > 0) {
    out[0] = leader;
  }
  return 1;
}

namespace detail {
/**
 * @brief Re-star one survivor set around its minimum; a lone survivor
 *        dissolves back to singleton. Shared by untie/detach paths.
 *
 * @param[out] nl Netlist receiving the rebuilt star.
 * @param[in] survivors Survivor flats to re-star.
 * @param[in] n_surv Number of survivors.
 */
inline void tie_reelect(Netlist &nl, const std::size_t *survivors, std::size_t n_surv) noexcept {
  if (n_surv > 1) {
    std::size_t leader = survivors[0];
    for (std::size_t k = 1; k < n_surv; ++k) {
      if (survivors[k] < leader) {
        leader = survivors[k];
      }
    }
    for (std::size_t k = 0; k < n_surv; ++k) {
      nl.tie_rep[survivors[k]] = static_cast<Slot>(leader + 1u);
    }
  } else if (n_surv == 1) {
    nl.tie_rep[survivors[0]] = 0;
  }
}
} // namespace detail

namespace tie_detail {
// --- Per-leader member chains (016, US3) -------------------------------------
// head[leader_flat] is the first member flat (ascending); heads hold
// kNoDriver for singletons. tie_next[member_flat] links the rest.
// Only this seam writes tie_head/tie_next; everyone else calls the
// functions below. Chains mirror tie_rep exactly: every non-singleton
// class enumerates slot-order ascending, head == class minimum.

/**
 * @brief Insert one flat into the leader's chain, ascending.
 *
 * Flats are unique per class. No-op positions are impossible by
 * construction (callers unlink before moving).
 *
 * @param[out] nl Netlist holding the chains.
 * @param[in] leader Class leader flat.
 * @param[in] flat Member flat to insert.
 */
inline void chain_insert_ordered(Netlist &nl, std::size_t leader, std::size_t flat) noexcept {
  Slot *link = &nl.tie_head[leader];
  while (*link != kNoDriver && static_cast<std::size_t>(*link) < flat) {
    link = &nl.tie_next[*link];
  }
  nl.tie_next[flat] = *link;
  *link = static_cast<Slot>(flat);
}

// Unlink flat from leader's chain; true when present.
[[nodiscard]] inline bool chain_unlink(Netlist &nl, std::size_t leader, std::size_t flat) noexcept {
  if (leader >= nl.tie_head.size() || flat >= nl.tie_next.size()) {
    return false;
  }
  Slot *link = &nl.tie_head[leader];
  while (*link != kNoDriver) {
    if (static_cast<std::size_t>(*link) == flat) {
      *link = nl.tie_next[flat];
      nl.tie_next[flat] = kNoDriver;
      return true;
    }
    link = &nl.tie_next[*link];
  }
  return false;
}

// Merge two ascending chains (la, lb) into one ascending chain headed
// at keep. Singleton classes contribute their lone leader flat.
inline void chain_merge(Netlist &nl, std::size_t keep, std::size_t la, std::size_t lb) noexcept {
  std::size_t a = (nl.tie_head[la] != kNoDriver) ? static_cast<std::size_t>(nl.tie_head[la]) : la;
  bool a_single = (nl.tie_head[la] == kNoDriver);
  std::size_t b = (nl.tie_head[lb] != kNoDriver) ? static_cast<std::size_t>(nl.tie_head[lb]) : lb;
  bool b_single = (nl.tie_head[lb] == kNoDriver);
  const std::size_t kNone = static_cast<std::size_t>(kNoDriver);
  std::size_t tail = kNone;
  auto emit = [&](std::size_t f) {
    if (tail == kNone) {
      nl.tie_head[keep] = static_cast<Slot>(f);
    } else {
      nl.tie_next[tail] = static_cast<Slot>(f);
    }
    tail = f;
  };
  auto step_a = [&]() {
    const std::size_t f = a;
    if (a_single) {
      a = kNone;
    } else {
      a = static_cast<std::size_t>(nl.tie_next[a]);
    }
    emit(f);
  };
  auto step_b = [&]() {
    const std::size_t f = b;
    if (b_single) {
      b = kNone;
    } else {
      b = static_cast<std::size_t>(nl.tie_next[b]);
    }
    emit(f);
  };
  while (a != kNone || b != kNone) {
    if (b == kNone || (a != kNone && a < b)) {
      step_a();
    } else {
      step_b();
    }
  }
  nl.tie_next[tail] = kNoDriver;
  if (keep != la) {
    nl.tie_head[la] = kNoDriver;
  }
  if (keep != lb) {
    nl.tie_head[lb] = kNoDriver;
  }
}
// Reconcile one class chain after its reps were re-starred: link the
// ascending survivor list under the new minimum (dissolve when the
// class holds one or zero members). The old key always clears, so a
// migrated minimum cannot leave a stale keyed chain behind.
inline void chain_reconcile(Netlist &nl, std::size_t old_key, const std::size_t *members, std::size_t n) noexcept {
  if (old_key < nl.tie_head.size()) {
    nl.tie_head[old_key] = kNoDriver;
  }
  if (n <= 1) {
    return; // empty or singleton: no chain
  }
  const Slot first = static_cast<Slot>(members[0]);
  nl.tie_head[first] = first;
  for (std::size_t k = 1; k < n; ++k) {
    nl.tie_next[members[k - 1]] = static_cast<Slot>(members[k]);
  }
  nl.tie_next[members[n - 1]] = kNoDriver;
}
} // namespace tie_detail

/**
 * @brief Rebuild every tie-class chain from the star table (bulk-load
 *        path): single ascending scan, O(pins). Cold paths only.
 *
 * (No noexcept: the tail map allocates.)
 *
 * @param[out] nl Netlist whose chains rebuild.
 */
inline void tie_chains_rebuild(Netlist &nl) {
  const std::size_t nflat = slot_count(nl) * kMaxInputs;
  for (std::size_t f = 0; f < nl.tie_head.size(); ++f) {
    nl.tie_head[f] = kNoDriver;
  }
  std::vector<Slot> tail(nflat, kNoDriver);
  for (std::size_t f = 0; f < nflat; ++f) {
    if (nl.tie_rep[f] == 0) {
      continue; // singleton: no chain
    }
    const std::size_t leader = static_cast<std::size_t>(nl.tie_rep[f] - 1u);
    if (leader >= nflat) {
      continue; // corrupt rep: loaders validate before installing
    }
    if (tail[leader] == kNoDriver) {
      nl.tie_head[leader] = static_cast<Slot>(f);
    } else {
      nl.tie_next[tail[leader]] = static_cast<Slot>(f);
    }
    nl.tie_next[f] = kNoDriver;
    tail[leader] = static_cast<Slot>(f);
  }
}

/**
 * @brief Number of tied peers in the class (1 for a singleton or a
 *        leader alone).
 *
 * Reads the maintained chain (016) for live in-range pins; the
 * degenerate out-of-range/dead cases keep the exact pre-chain scan.
 *
 * @param[in] nl Netlist holding the tie table.
 * @param[in] slot Gate slot.
 * @param[in] input 0-based input index.
 * @return Member count of the pin's class.
 */
[[nodiscard]] inline std::size_t tie_group_size(const Netlist &nl, Slot slot, std::uint8_t input) noexcept {
  if (slot >= slot_count(nl) || !nl.slots[slot].live || input >= nl.slots[slot].input_count) {
    std::size_t n = 0;
    const std::size_t flat = tie_flat(slot, input);
    const std::size_t leader = tie_leader_flat(nl, flat);
    for (Slot s = 0; s < slot_count(nl); ++s) {
      const Gate &g = nl.slots[s];
      if (!g.live) {
        continue;
      }
      for (std::uint8_t i = 0; i < g.input_count; ++i) {
        if (tie_leader_flat(nl, tie_flat(s, i)) == leader) {
          ++n;
        }
      }
    }
    return n;
  }
  const std::size_t leader = tie_leader_flat(nl, tie_flat(slot, input));
  if (leader >= nl.tie_head.size() || nl.tie_head[leader] == kNoDriver) {
    return 1; // singleton class (wired pins included, as before)
  }
  std::size_t n = 0;
  for (Slot e = nl.tie_head[leader]; e != kNoDriver; e = nl.tie_next[e]) {
    ++n;
  }
  return n;
}

/**
 * @brief Tie two dangling input pins to the same net.
 *
 * Merging two classes elects the lowest (slot, input) as canonical
 * leader and re-stars every member. Tying is idempotent within a
 * class.
 *
 * @param[out] nl Netlist receiving the tie.
 * @param[in] a First input pin (must dangle, live, non-ignored).
 * @param[in] b Second input pin (must dangle, live, non-ignored).
 * @return kOk on success (including idempotent re-tie);
 *         kInvalidArgument for output pins, dead/out-of-range pins,
 *         wired, ignored, or pulled pins, or tying a pin to itself.
 */
[[nodiscard]] inline core::Status tie_pins(Netlist &nl, PinRef a, PinRef b) noexcept {
  if (a.is_output || b.is_output) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  if (a.slot >= slot_count(nl) || b.slot >= slot_count(nl)) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  const Gate &ga = nl.slots[a.slot];
  const Gate &gb = nl.slots[b.slot];
  if (!ga.live || !gb.live) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  if (a.input >= ga.input_count || b.input >= gb.input_count) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  if (a.slot == b.slot && a.input == b.input) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  if (ga.drivers[a.input] != kNoDriver || gb.drivers[b.input] != kNoDriver) {
    return core::Status::failure(core::StatusCode::kInvalidArgument); // wired
  }
  if (((ga.ignored_mask & (1u << a.input)) != 0) || ((gb.ignored_mask & (1u << b.input)) != 0)) {
    return core::Status::failure(core::StatusCode::kInvalidArgument); // ignored
  }
  const std::size_t fa = tie_flat(a.slot, a.input);
  const std::size_t fb = tie_flat(b.slot, b.input);
  if (is_shared_flat(nl, fa) || is_shared_flat(nl, fb)) {
    return core::Status::failure(core::StatusCode::kInvalidArgument); // shared: never tied
  }
  if (is_pulled_flat(nl, fa) || is_pulled_flat(nl, fb)) {
    return core::Status::failure(core::StatusCode::kInvalidArgument); // pulled: unpull first
  }
  const std::size_t la = tie_leader_flat(nl, fa);
  const std::size_t lb = tie_leader_flat(nl, fb);
  if (la == lb) {
    return core::Status::success(); // idempotent
  }
  const std::size_t new_leader = (la < lb) ? la : lb;
  // Re-star every member of both classes around the new leader, walking
  // the maintained chains (016: class-proportional, never the pool).
  tie_detail::chain_merge(nl, new_leader, la, lb);
  for (Slot e = nl.tie_head[new_leader]; e != kNoDriver; e = nl.tie_next[e]) {
    nl.tie_rep[e] = static_cast<Slot>(new_leader + 1u);
  }
  return core::Status::success();
}

/**
 * @brief Untie one input pin from its class.
 *
 * Removing the leader re-elects the lowest surviving member; removing
 * a non-leader leaves the class otherwise intact. A two-member class
 * dissolves back to two singletons.
 *
 * @param[out] nl Netlist receiving the untie.
 * @param[in] pin Input pin to remove from its class.
 * @param[in] survivors Caller-owned scratch (`slot_count(nl) * kMaxInputs` flats)
 *            collecting surviving members.
 * @return kOk on success; kInvalidArgument for output pins,
 *         dead/out-of-range pins, wired or ignored pins, pins that
 *         are not tied, or an undersized scratch buffer.
 */
[[nodiscard]] inline core::Status untie_pin(Netlist &nl, PinRef pin, std::span<std::size_t> survivors) noexcept {
  if (pin.is_output) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  if (pin.slot >= slot_count(nl) || !nl.slots[pin.slot].live) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  const Gate &g = nl.slots[pin.slot];
  if (pin.input >= g.input_count) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  if (g.drivers[pin.input] != kNoDriver) {
    return core::Status::failure(core::StatusCode::kInvalidArgument); // wired
  }
  if ((g.ignored_mask & (1u << pin.input)) != 0) {
    return core::Status::failure(core::StatusCode::kInvalidArgument); // ignored
  }
  const std::size_t flat = tie_flat(pin.slot, pin.input);
  if (nl.tie_rep[flat] == 0) {
    return core::Status::failure(core::StatusCode::kInvalidArgument); // not tied
  }
  if (survivors.size() < slot_count(nl) * kMaxInputs) {
    return core::Status::failure(core::StatusCode::kInvalidArgument); // scratch too small
  }
  const std::size_t leader = tie_leader_flat(nl, flat);
  // Collect survivors by walking the class chain (016), skipping the
  // removed pin; slot order is the chain order. Scratch capacity is
  // guaranteed by the size check above.
  std::size_t n_surv = 0;
  for (Slot e = nl.tie_head[leader]; e != kNoDriver; e = nl.tie_next[e]) {
    if (static_cast<std::size_t>(e) == flat) {
      continue;
    }
    survivors[n_surv++] = static_cast<std::size_t>(e);
  }
  nl.tie_rep[flat] = 0;
  // Unlink the pin, re-star the survivors, and reconcile the chain
  // (dissolve clears it; a migrated minimum re-keys it).
  (void)tie_detail::chain_unlink(nl, leader, flat);
  detail::tie_reelect(nl, survivors.data(), n_surv);
  tie_detail::chain_reconcile(nl, leader, survivors.data(), n_surv);
  return core::Status::success();
}

/**
 * @brief Untie one input pin from its class.
 *
 * Workspace-backed entry: grows the survivor scratch to the pool size,
 * then runs the core above. (No noexcept: workspace growth may
 * allocate.)
 *
 * @param[out] nl Netlist receiving the untie.
 * @param[in] pin Input pin to remove from its class.
 * @return kOk on success; kInvalidArgument for output pins,
 *         dead/out-of-range pins, wired or ignored pins, or pins that
 *         are not tied.
 */
[[nodiscard]] inline core::Status untie_pin(Netlist &nl, PinRef pin) {
  const std::size_t n = slot_count(nl);
  ensure_workspace(nl, n);
  return untie_pin(nl, pin, std::span<std::size_t>(nl.ws_survivors.data(), n * kMaxInputs));
}

/**
 * @brief Detach every tied pin of slot s from its class (used when a
 *        gate is removed or collapsed to a constant): outside survivors
 *        re-elect, and slot s keeps no tie entries.
 *
 * @param[out] nl Netlist receiving the detach.
 * @param[in] s Slot to detach.
 * @param[in] survivors Caller-owned scratch (`slot_count(nl) * kMaxInputs` flats)
 *            collecting surviving members.
 */
inline void detach_slot_ties(Netlist &nl, Slot s, std::span<std::size_t> survivors) noexcept {
  if (s >= slot_count(nl) || !nl.slots[s].live || survivors.size() < slot_count(nl) * kMaxInputs) {
    return;
  }
  const std::uint8_t n = nl.slots[s].input_count;
  // Group-wise: one rebuild per distinct original leader.
  std::size_t leaders[kMaxInputs];
  std::size_t n_leaders = 0;
  {
    std::size_t seen[kMaxInputs];
    std::size_t n_seen = 0;
    for (std::uint8_t i = 0; i < n; ++i) {
      const std::size_t flat = tie_flat(s, i);
      if (nl.tie_rep[flat] == 0) {
        continue;
      }
      const std::size_t l = tie_leader_flat(nl, flat);
      bool have = false;
      for (std::size_t k = 0; k < n_seen; ++k) {
        if (seen[k] == l) {
          have = true;
          break;
        }
      }
      if (!have) {
        seen[n_seen++] = l;
        leaders[n_leaders++] = l;
      }
    }
  }
  for (std::size_t li = 0; li < n_leaders; ++li) {
    const std::size_t l = leaders[li];
    // Unlink this slot's flats from the class chain (016) and clear
    // only this slot's star entries.
    for (std::uint8_t i = 0; i < n; ++i) {
      const std::size_t f = tie_flat(s, i);
      if (tie_leader_flat(nl, f) == l) {
        (void)tie_detail::chain_unlink(nl, l, f);
        nl.tie_rep[f] = 0;
      }
    }
    // Collect survivors in chain order, re-star, and reconcile the
    // chain (dissolve clears it; a migrated minimum re-keys it).
    std::size_t n_surv = 0;
    for (Slot e = nl.tie_head[l]; e != kNoDriver; e = nl.tie_next[e]) {
      survivors[n_surv++] = static_cast<std::size_t>(e);
    }
    detail::tie_reelect(nl, survivors.data(), n_surv);
    tie_detail::chain_reconcile(nl, l, survivors.data(), n_surv);
  }
  for (std::size_t i = 0; i < kMaxInputs; ++i) {
    nl.tie_rep[tie_flat(s, static_cast<std::uint8_t>(i))] = 0;
  }
}

/**
 * @brief Rebuild tie classes after an input-drop compaction on slot s.
 *
 * Kept pins shift to indices 0..n-1, each carrying its original class
 * leader in orig_leader[k] (orig_tied[k] says whether it was tied).
 * Each original class re-stars around its new minimum (kept new flats
 * plus outside survivors still pointing at the old leader); singletons
 * stay singleton. Call with the gate's new input_count already in
 * place.
 *
 * @param[out] nl Netlist receiving the rebuilt classes.
 * @param[in] s Compacted slot.
 * @param[in] orig_leader Original leader flat per kept pin.
 * @param[in] orig_tied Whether each kept pin was tied.
 * @param[in] n Kept pin count (the new input_count).
 * @param[in] survivors Caller-owned scratch (`slot_count(nl) * kMaxInputs` flats).
 */
inline void reindex_kept_ties(Netlist &nl, Slot s, const std::size_t *orig_leader, const bool *orig_tied,
                               std::uint8_t n, std::span<std::size_t> survivors) noexcept {
  if (s >= slot_count(nl) || survivors.size() < slot_count(nl) * kMaxInputs) {
    return;
  }
  for (std::size_t i = 0; i < kMaxInputs; ++i) {
    nl.tie_rep[tie_flat(s, static_cast<std::uint8_t>(i))] = 0;
  }
  for (std::uint8_t k = 0; k < n; ++k) {
    if (!orig_tied[k]) {
      continue;
    }
    const std::size_t orig = orig_leader[k];
    // Skip if this class was already rebuilt (another kept pin shared it).
    bool done = false;
    for (std::uint8_t j = 0; j < k; ++j) {
      if (orig_tied[j] && orig_leader[j] == orig) {
        done = true;
        break;
      }
    }
    if (done) {
      continue;
    }
    // Unlink this slot's old flats from the class chain (016): any
    // chain member on slot s is a kept pin moving to a new flat.
    for (Slot e = nl.tie_head[orig]; e != kNoDriver;) {
      const Slot cur = e;
      e = nl.tie_next[e];
      if (static_cast<std::size_t>(cur) / kMaxInputs == s) {
        (void)tie_detail::chain_unlink(nl, orig, static_cast<std::size_t>(cur));
      }
    }
    // Clear this slot's whole tie row (mirrors the old wholesale
    // clear: dropped positions must read singleton).
    for (std::size_t i = 0; i < kMaxInputs; ++i) {
      nl.tie_rep[tie_flat(s, static_cast<std::uint8_t>(i))] = 0;
    }
    // Re-insert kept pins at their new flats, ascending; outside
    // survivors keep their positions.
    for (std::uint8_t j = 0; j < n; ++j) {
      if (orig_tied[j] && orig_leader[j] == orig) {
        tie_detail::chain_insert_ordered(nl, orig, tie_flat(s, j));
      }
    }
    // Re-star the rebuilt class around its new minimum (identical to
    // the old collect-and-reelect: same member set, same minimum),
    // then reconcile the chain (dissolve clears it; a migrated
    // minimum re-keys it).
    std::size_t n_surv = 0;
    for (Slot e = nl.tie_head[orig]; e != kNoDriver; e = nl.tie_next[e]) {
      survivors[n_surv++] = static_cast<std::size_t>(e);
    }
    detail::tie_reelect(nl, survivors.data(), n_surv);
    tie_detail::chain_reconcile(nl, orig, survivors.data(), n_surv);
  }
}

/**
 * @brief Dissolve one tie class into ignored pins: every member unties
 *        and marks ignored (ties off to kLow at eval).
 *
 * Used by exact dead-input elimination, where dead inputs never affect
 * any output so the function is preserved. No-op when leader is not a
 * live class leader.
 *
 * @param[out] nl Netlist receiving the dissolve.
 * @param[in] leader Class leader flat to dissolve.
 * @param[in] members Caller-owned scratch (`slot_count(nl) * kMaxInputs` flats).
 */
inline void ignore_tie_class(Netlist &nl, std::size_t leader, std::span<std::size_t> members) noexcept {
  if (members.size() < slot_count(nl) * kMaxInputs) {
    return;
  }
  const std::size_t n = tie_class_members(nl, leader, members.data(), members.size());
  for (std::size_t k = 0; k < n; ++k) {
    const auto slot = static_cast<Slot>(members[k] / kMaxInputs);
    const auto input = static_cast<std::uint8_t>(members[k] % kMaxInputs);
    if (slot >= slot_count(nl) || !nl.slots[slot].live || input >= nl.slots[slot].input_count) {
      continue;
    }
    nl.tie_rep[members[k]] = 0;
    nl.slots[slot].ignored_mask =
        static_cast<std::uint8_t>(nl.slots[slot].ignored_mask | static_cast<std::uint8_t>(1u << input));
  }
  // The class dissolves into ignored pins: clear its chain head.
  if (leader < nl.tie_head.size()) {
    nl.tie_head[leader] = kNoDriver;
  }
}

/**
 * @brief Byte-level read of one raw tie_rep entry for checkpoint
 *        serialization (world format v6/v7).
 *
 * @param[in] nl Netlist holding the tie table.
 * @param[in] flat Flat pin index from tie_flat.
 * @return tie_rep[flat], or 0 when flat is out of range.
 */
[[nodiscard]] inline Slot tie_rep_entry(const Netlist &nl, std::size_t flat) noexcept {
  return (flat < slot_count(nl) * kMaxInputs) ? nl.tie_rep[flat] : 0;
}

/**
 * @brief Byte-level write of one raw tie_rep entry for the checkpoint
 *        loader.
 *
 * @param[out] nl Netlist receiving the entry.
 * @param[in] flat Flat pin index from tie_flat.
 * @param[in] rep Raw star value to store.
 */
inline void set_tie_rep_entry(Netlist &nl, std::size_t flat, Slot rep) noexcept {
  if (flat < slot_count(nl) * kMaxInputs) {
    nl.tie_rep[flat] = rep;
  }
}

/**
 * @brief Star-consistency validation for a loaded netlist: tied pins
 *        must be live, unwired, non-ignored; every rep must name an
 *        in-range leader that agrees (tie_rep[leader] == rep) and is
 *        itself a live dangling non-ignored pin. Pure readout, used by
 *        the checkpoint loader.
 *
 * @param[in] nl Netlist to validate.
 * @return True when every tie entry is consistent.
 */
[[nodiscard]] inline bool validate_ties(const Netlist &nl) noexcept {
  for (Slot slot = 0; slot < slot_count(nl); ++slot) {
    const Gate &g = nl.slots[slot];
    if (!g.live) {
      continue;
    }
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      const std::size_t flat = tie_flat(slot, i);
      const Slot rep = nl.tie_rep[flat];
      if (rep == 0) {
        continue;
      }
      if (g.drivers[i] != kNoDriver || (g.ignored_mask & static_cast<std::uint8_t>(1u << i)) != 0) {
        return false;
      }
      const std::size_t leader = static_cast<std::size_t>(rep - 1u);
      if (leader >= slot_count(nl) * kMaxInputs || nl.tie_rep[leader] != rep) {
        return false;
      }
      const auto ls = static_cast<Slot>(leader / kMaxInputs);
      const auto li = static_cast<std::uint8_t>(leader % kMaxInputs);
      const Gate &lg = nl.slots[ls];
      if (!lg.live || li >= lg.input_count || lg.drivers[li] != kNoDriver ||
          (lg.ignored_mask & static_cast<std::uint8_t>(1u << li)) != 0) {
        return false;
      }
    }
  }
  return true;
}

/**
 * @brief Count of tie groups with at least 2 members.
 *
 * @param[in] nl Netlist holding the tie table.
 * @return Number of live classes with 2 or more members.
 */
[[nodiscard]] inline std::size_t tie_group_count(const Netlist &nl) noexcept {
  std::size_t n = 0;
  for (Slot s = 0; s < slot_count(nl); ++s) {
    const Gate &g = nl.slots[s];
    if (!g.live) {
      continue;
    }
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      if (g.drivers[i] != kNoDriver) {
        continue;
      }
      if ((g.ignored_mask & (1u << i)) != 0) {
        continue;
      }
      const std::size_t flat = tie_flat(s, i);
      if (tie_leader_flat(nl, flat) != flat) {
        continue; // non-leaders don't start groups
      }
      if (nl.tie_rep[flat] == 0) {
        continue; // singleton
      }
      ++n;
    }
  }
  return n;
}

/**
 * @brief Clear the whole tie pool (reset_netlist / fresh Netlist
 *        invariant). Construction helper: the only direct writer beside
 *        the tie/untie/detach paths above.
 *
 * @param[out] nl Netlist whose ties are cleared.
 */
inline void reset_all_ties(Netlist &nl) noexcept {
  for (std::size_t i = 0; i < slot_count(nl) * kMaxInputs; ++i) {
    nl.tie_rep[i] = 0;
  }
  // Bulk clear: every class dissolves, so every chain head clears.
  for (std::size_t i = 0; i < nl.tie_head.size(); ++i) {
    nl.tie_head[i] = kNoDriver;
  }
}

/**
 * @brief Total tied pairs for canonical YAML: one entry per non-leader
 *        tied member.
 *
 * @param[in] nl Netlist holding the tie table.
 * @return Number of tied non-leader dangling pins.
 */
[[nodiscard]] inline std::size_t tie_pair_count(const Netlist &nl) noexcept {
  std::size_t n = 0;
  for (Slot s = 0; s < slot_count(nl); ++s) {
    const Gate &g = nl.slots[s];
    if (!g.live) {
      continue;
    }
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      if (g.drivers[i] != kNoDriver) {
        continue;
      }
      if ((g.ignored_mask & (1u << i)) != 0) {
        continue;
      }
      const std::size_t flat = tie_flat(s, i);
      if (nl.tie_rep[flat] != 0 && tie_leader_flat(nl, flat) != flat) {
        ++n;
      }
    }
  }
  return n;
}

} // namespace wengine::logic
