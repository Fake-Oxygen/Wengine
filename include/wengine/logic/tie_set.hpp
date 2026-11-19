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
 * Dependency rules (docs/logic.md section 4): header-only,
 * allocation-free, exception-free; depends only on types.hpp plus
 * core/status (never netlist.hpp: PinRef lives in types.hpp precisely
 * to keep this include one-directional). Logic is a standalone domain:
 * this header must never depend on CLI types. Consumers: netlist.hpp
 * mutation, the O0/O1/O2 optimizer passes, eval.hpp, seq_step.hpp, the
 * CLI tie/untie verbs, and the test_logic* suite.
 */

#include "wengine/core/status.hpp"
#include "wengine/logic/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace wengine::logic {

/**
 * @brief Whole-table tie snapshot: tie_pins can merge two classes, so
 *        a pin-level untie cannot revert a merge — callers snapshot and
 *        restore wholesale instead.
 */
using TieSnapshot = std::array<std::uint16_t, kMaxGates * kMaxInputs>;

/**
 * @brief Copy the whole tie table for a later wholesale restore.
 *
 * @param[in] nl Netlist to snapshot.
 * @return Copy of nl.tie_rep.
 */
[[nodiscard]] inline TieSnapshot snapshot_ties(const Netlist &nl) noexcept { return nl.tie_rep; }

/**
 * @brief Restore a whole-table snapshot taken by snapshot_ties.
 *
 * @param[out] nl Netlist receiving the snapshot.
 * @param[in] snap Snapshot to install.
 */
inline void restore_ties(Netlist &nl, const TieSnapshot &snap) noexcept { nl.tie_rep = snap; }

/**
 * @brief Structural equality of two tie tables (save/load round-trip).
 *
 * @param[in] a First netlist.
 * @param[in] b Second netlist.
 * @return True when every tie_rep entry agrees.
 */
[[nodiscard]] inline bool ties_equal(const Netlist &a, const Netlist &b) noexcept {
  for (std::size_t f = 0; f < kMaxGates * kMaxInputs; ++f) {
    if (a.tie_rep[f] != b.tie_rep[f]) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Flat tie-table index for (slot, input): slot * kMaxInputs + input.
 *
 * @param[in] slot Gate slot.
 * @param[in] input 0-based input index.
 * @return Flat index into Netlist::tie_rep.
 */
[[nodiscard]] constexpr std::size_t tie_flat(std::uint16_t slot, std::uint8_t input) noexcept {
  return static_cast<std::size_t>(slot) * kMaxInputs + static_cast<std::size_t>(input);
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
  const std::uint16_t rep = nl.tie_rep[flat];
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
[[nodiscard]] inline bool is_tied(const Netlist &nl, std::uint16_t slot, std::uint8_t input) noexcept {
  if (slot >= kMaxGates || input >= kMaxInputs) {
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
[[nodiscard]] inline bool is_tie_leader(const Netlist &nl, std::uint16_t slot, std::uint8_t input) noexcept {
  const std::size_t flat = tie_flat(slot, input);
  return tie_leader_flat(nl, flat) == flat;
}

/**
 * @brief Slot-order member enumeration for one class leader.
 *
 * @param[in] nl Netlist holding the tie table.
 * @param[in] leader Class leader flat to enumerate.
 * @param[out] out Caller buffer receiving member flats.
 * @param[in] cap Capacity of out.
 * @return Total member count (may exceed cap; only the first cap
 *         entries are stored). Pure readout.
 */
[[nodiscard]] inline std::size_t tie_class_members(const Netlist &nl, std::size_t leader, std::size_t *out, std::size_t cap) noexcept {
  std::size_t n = 0;
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    const Gate &g = nl.gates[s];
    if (!g.live) {
      continue;
    }
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      if (g.drivers[i] != kNoDriver) {
        continue;
      }
      if ((g.ignored_mask & static_cast<std::uint8_t>(1u << i)) != 0) {
        continue;
      }
      const std::size_t f = tie_flat(s, i);
      if (tie_leader_flat(nl, f) != leader) {
        continue;
      }
      if (n < cap) {
        out[n] = f;
      }
      ++n;
    }
  }
  return n;
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
      nl.tie_rep[survivors[k]] = static_cast<std::uint16_t>(leader + 1u);
    }
  } else if (n_surv == 1) {
    nl.tie_rep[survivors[0]] = 0;
  }
}
} // namespace detail

/**
 * @brief Number of tied peers in the class (1 for a singleton or a
 *        leader alone).
 *
 * @param[in] nl Netlist holding the tie table.
 * @param[in] slot Gate slot.
 * @param[in] input 0-based input index.
 * @return Member count of the pin's class.
 */
[[nodiscard]] inline std::size_t tie_group_size(const Netlist &nl, std::uint16_t slot, std::uint8_t input) noexcept {
  const std::size_t flat = tie_flat(slot, input);
  const std::size_t leader = tie_leader_flat(nl, flat);
  std::size_t n = 0;
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    const Gate &g = nl.gates[s];
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
 *         wired or ignored pins, or tying a pin to itself.
 */
[[nodiscard]] inline core::Status tie_pins(Netlist &nl, PinRef a, PinRef b) noexcept {
  if (a.is_output || b.is_output) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  if (a.slot >= kMaxGates || b.slot >= kMaxGates) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  const Gate &ga = nl.gates[a.slot];
  const Gate &gb = nl.gates[b.slot];
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
  const std::size_t la = tie_leader_flat(nl, fa);
  const std::size_t lb = tie_leader_flat(nl, fb);
  if (la == lb) {
    return core::Status::success(); // idempotent
  }
  const std::size_t new_leader = (la < lb) ? la : lb;
  // Re-star every member of both classes around the new leader.
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    const Gate &g = nl.gates[s];
    if (!g.live) {
      continue;
    }
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      const std::size_t f = tie_flat(s, i);
      const std::size_t l = tie_leader_flat(nl, f);
      if (l == la || l == lb) {
        nl.tie_rep[f] = static_cast<std::uint16_t>(new_leader + 1u);
      }
    }
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
 * @return kOk on success; kInvalidArgument for output pins,
 *         dead/out-of-range pins, wired or ignored pins, or pins that
 *         are not tied.
 */
[[nodiscard]] inline core::Status untie_pin(Netlist &nl, PinRef pin) noexcept {
  if (pin.is_output) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  if (pin.slot >= kMaxGates || !nl.gates[pin.slot].live) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  const Gate &g = nl.gates[pin.slot];
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
  const std::size_t leader = tie_leader_flat(nl, flat);
  std::size_t survivors[kMaxGates * kMaxInputs];
  std::size_t n_surv = 0;
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    const Gate &o = nl.gates[s];
    if (!o.live) {
      continue;
    }
    for (std::uint8_t i = 0; i < o.input_count; ++i) {
      const std::size_t f = tie_flat(s, i);
      if (f == flat) {
        continue;
      }
      if (tie_leader_flat(nl, f) == leader) {
        survivors[n_surv++] = f;
      }
    }
  }
  nl.tie_rep[flat] = 0;
  detail::tie_reelect(nl, survivors, n_surv);
  return core::Status::success();
}

/**
 * @brief Detach every tied pin of slot s from its class (used when a
 *        gate is removed or collapsed to a constant): outside survivors
 *        re-elect, and slot s keeps no tie entries.
 *
 * @param[out] nl Netlist receiving the detach.
 * @param[in] s Slot to detach.
 */
inline void detach_slot_ties(Netlist &nl, std::uint16_t s) noexcept {
  if (s >= kMaxGates || !nl.gates[s].live) {
    return;
  }
  const std::uint8_t n = nl.gates[s].input_count;
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
    std::size_t survivors[kMaxGates * kMaxInputs];
    std::size_t n_surv = 0;
    for (std::uint16_t t = 0; t < kMaxGates; ++t) {
      if (t == s) {
        continue;
      }
      const Gate &o = nl.gates[t];
      if (!o.live) {
        continue;
      }
      for (std::uint8_t j = 0; j < o.input_count; ++j) {
        const std::size_t f = tie_flat(t, j);
        if (tie_leader_flat(nl, f) == l) {
          survivors[n_surv++] = f;
        }
      }
    }
    // Clear the whole class (this slot + survivors), then re-star survivors.
    for (std::uint8_t i = 0; i < n; ++i) {
      nl.tie_rep[tie_flat(s, i)] = 0;
    }
    for (std::size_t k = 0; k < n_surv; ++k) {
      nl.tie_rep[survivors[k]] = 0;
    }
    detail::tie_reelect(nl, survivors, n_surv);
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
 */
inline void reindex_kept_ties(Netlist &nl, std::uint16_t s, const std::size_t *orig_leader, const bool *orig_tied, std::uint8_t n) noexcept {
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
    std::size_t survivors[kMaxGates * kMaxInputs];
    std::size_t n_surv = 0;
    for (std::uint8_t j = 0; j < n; ++j) {
      if (orig_tied[j] && orig_leader[j] == orig) {
        survivors[n_surv++] = tie_flat(s, j);
      }
    }
    for (std::uint16_t t = 0; t < kMaxGates; ++t) {
      if (t == s) {
        continue;
      }
      const Gate &o = nl.gates[t];
      if (!o.live) {
        continue;
      }
      for (std::uint8_t j = 0; j < o.input_count; ++j) {
        const std::size_t f = tie_flat(t, j);
        if (tie_leader_flat(nl, f) == orig) {
          survivors[n_surv++] = f;
        }
      }
    }
    detail::tie_reelect(nl, survivors, n_surv);
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
 */
inline void ignore_tie_class(Netlist &nl, std::size_t leader) noexcept {
  std::size_t members[kMaxGates * kMaxInputs];
  const std::size_t n = tie_class_members(nl, leader, members, kMaxGates * kMaxInputs);
  for (std::size_t k = 0; k < n; ++k) {
    const auto slot = static_cast<std::uint16_t>(members[k] / kMaxInputs);
    const auto input = static_cast<std::uint8_t>(members[k] % kMaxInputs);
    if (slot >= kMaxGates || !nl.gates[slot].live || input >= nl.gates[slot].input_count) {
      continue;
    }
    nl.tie_rep[members[k]] = 0;
    nl.gates[slot].ignored_mask =
        static_cast<std::uint8_t>(nl.gates[slot].ignored_mask | static_cast<std::uint8_t>(1u << input));
  }
}

/**
 * @brief Byte-level read of one raw tie_rep entry for checkpoint
 *        serialization (world format v6).
 *
 * @param[in] nl Netlist holding the tie table.
 * @param[in] flat Flat pin index from tie_flat.
 * @return tie_rep[flat], or 0 when flat is out of range.
 */
[[nodiscard]] inline std::uint16_t tie_rep_entry(const Netlist &nl, std::size_t flat) noexcept {
  return (flat < kMaxGates * kMaxInputs) ? nl.tie_rep[flat] : 0;
}

/**
 * @brief Byte-level write of one raw tie_rep entry for the checkpoint
 *        loader.
 *
 * @param[out] nl Netlist receiving the entry.
 * @param[in] flat Flat pin index from tie_flat.
 * @param[in] rep Raw star value to store.
 */
inline void set_tie_rep_entry(Netlist &nl, std::size_t flat, std::uint16_t rep) noexcept {
  if (flat < kMaxGates * kMaxInputs) {
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
  for (std::uint16_t slot = 0; slot < kMaxGates; ++slot) {
    const Gate &g = nl.gates[slot];
    if (!g.live) {
      continue;
    }
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      const std::size_t flat = tie_flat(slot, i);
      const std::uint16_t rep = nl.tie_rep[flat];
      if (rep == 0) {
        continue;
      }
      if (g.drivers[i] != kNoDriver || (g.ignored_mask & static_cast<std::uint8_t>(1u << i)) != 0) {
        return false;
      }
      const std::size_t leader = static_cast<std::size_t>(rep - 1u);
      if (leader >= kMaxGates * kMaxInputs || nl.tie_rep[leader] != rep) {
        return false;
      }
      const auto ls = static_cast<std::uint16_t>(leader / kMaxInputs);
      const auto li = static_cast<std::uint8_t>(leader % kMaxInputs);
      const Gate &lg = nl.gates[ls];
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
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    const Gate &g = nl.gates[s];
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
  for (std::size_t i = 0; i < kMaxGates * kMaxInputs; ++i) {
    nl.tie_rep[i] = 0;
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
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    const Gate &g = nl.gates[s];
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
