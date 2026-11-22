#pragma once

/**
 * @file
 * Pre-change scan oracle for the 016 cached-adjacency feature.
 *
 * Every function here is a verbatim copy of the scan implementation it
 * names, taken before any chain/index consumer replaced it (the
 * function bodies are identical to the pre-016 sources; only the
 * names carry the `ref_` prefix and the namespace is `scan_ref`).
 * The copies call the LIVE table vocabulary (`tie_flat`,
 * `tie_leader_flat`, `for_each_extra`, `dedup_key_for`, ...) and the
 * LIVE seam for table writes — those are table truth, never replaced
 * by this feature — but every READ loop the feature replaces is
 * frozen here.
 *
 * Layering notes (contracts/parity-harness.md §1):
 * - `ref_dedup_universal_pass_impl` calls `ref_rewire_users`,
 *   `ref_remove_gate`, and `ref_drives_shared` (all frozen here) plus
 *   the live `recount_fanout` (never replaced) — its merge decisions
 *   are independent of every phase.
 * - `ref_remove_gate` calls the live `detach_slot_ties`/`unwire`/
 *   `unwire_driver`: those keep their semantics in every phase (pinned
 *   by their own parity batteries); only the sink-sweep loop is
 *   frozen here.
 * - `ref_add_gate` is a standalone copy for the slot-assignment
 *   replay check (FR-017).
 *
 * Test-only: included by `tests/test_logic_adjacency.cpp` alone, never
 * by shipped headers.
 */

#include "wengine/core/status.hpp"
#include "wengine/logic/netlist.hpp"
#include "wengine/logic/optimize_barrier.hpp"
#include "wengine/logic/optimize_passes.hpp"
#include "wengine/logic/pins.hpp"
#include "wengine/logic/shared_net.hpp"
#include "wengine/logic/tie_set.hpp"
#include "wengine/logic/topo.hpp"
#include "wengine/logic/types.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace wengine::logic::scan_ref {

// --- topo.hpp: detail::drain_successors (pre-T009) ---------------------------

inline void ref_drain_successors(const Netlist &nl, EdgeFilter filter, Slot cur, std::uint8_t *indeg, Slot *queue,
                                 std::size_t &tail) noexcept {
  const bool skip_seq = (filter == EdgeFilter::kCombOnly);
  for (Slot s = 0; s < slot_count(nl); ++s) {
    const Gate &g = nl.slots[s];
    if (!g.live || indeg[s] == 0) {
      continue;
    }
    if (skip_seq && is_sequential_kind(g.kind)) {
      continue;
    }
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      if (g.drivers[i] == cur) {
        --indeg[s];
        if (indeg[s] == 0) {
          queue[tail++] = s;
          break;
        }
      }
    }
    if (indeg[s] != 0) {
      bool drained = false;
      for (std::uint8_t i = 0; i < g.input_count; ++i) {
        for_each_extra(nl, tie_flat(s, i), [&](std::uint8_t, Slot edrv, std::uint8_t) {
          if (edrv == cur) {
            --indeg[s];
            drained = true;
          }
        });
      }
      if (drained && indeg[s] == 0) {
        queue[tail++] = s;
      }
    }
  }
}

// --- topo.hpp: topo::reaches core (pre-T012) ----------------------------------

[[nodiscard]] inline bool ref_reaches(const Netlist &nl, Slot from, Slot to, EdgeFilter filter, std::span<std::uint8_t> seen,
                                      std::span<Slot> stack) noexcept {
  if (from == to) {
    return true;
  }
  if (seen.size() < slot_count(nl) || stack.size() < slot_count(nl)) {
    return false;
  }
  for (std::size_t i = 0; i < slot_count(nl); ++i) {
    seen[i] = false;
  }
  const bool skip_seq = (filter == EdgeFilter::kCombOnly);
  std::size_t top = 0;
  stack[top++] = from;
  seen[from] = true;
  while (top > 0) {
    const Slot cur = stack[--top];
    for (Slot s = 0; s < slot_count(nl); ++s) {
      const Gate &g = nl.slots[s];
      if (!g.live) {
        continue;
      }
      if (skip_seq && is_sequential_kind(g.kind)) {
        continue;
      }
      bool uses_cur = false;
      for (std::uint8_t i = 0; i < g.input_count; ++i) {
        if (g.drivers[i] == cur) {
          uses_cur = true;
          break;
        }
      }
      if (!uses_cur) {
        for (std::uint8_t i = 0; i < g.input_count && !uses_cur; ++i) {
          if (shared_uses_driver_flat(nl, tie_flat(s, i), cur)) {
            uses_cur = true;
          }
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

// --- tie_set.hpp readers (pre-T020) ------------------------------------------

[[nodiscard]] inline std::size_t ref_tie_class_members(const Netlist &nl, std::size_t leader, std::size_t *out,
                                                       std::size_t cap) noexcept {
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

[[nodiscard]] inline std::size_t ref_tie_group_size(const Netlist &nl, Slot slot, std::uint8_t input) noexcept {
  const std::size_t flat = tie_flat(slot, input);
  const std::size_t leader = tie_leader_flat(nl, flat);
  std::size_t n = 0;
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

[[nodiscard]] inline std::size_t ref_tie_group_count(const Netlist &nl) noexcept {
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
        continue;
      }
      if (nl.tie_rep[flat] == 0) {
        continue;
      }
      ++n;
    }
  }
  return n;
}

[[nodiscard]] inline std::size_t ref_tie_pair_count(const Netlist &nl) noexcept {
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

inline void ref_tie_reelect(Netlist &nl, const std::size_t *survivors, std::size_t n_surv) noexcept {
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

[[nodiscard]] inline core::Status ref_tie_pins(Netlist &nl, PinRef a, PinRef b) noexcept {
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
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  if (((ga.ignored_mask & (1u << a.input)) != 0) || ((gb.ignored_mask & (1u << b.input)) != 0)) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  const std::size_t fa = tie_flat(a.slot, a.input);
  const std::size_t fb = tie_flat(b.slot, b.input);
  if (is_shared_flat(nl, fa) || is_shared_flat(nl, fb)) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  if (is_pulled_flat(nl, fa) || is_pulled_flat(nl, fb)) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  const std::size_t la = tie_leader_flat(nl, fa);
  const std::size_t lb = tie_leader_flat(nl, fb);
  if (la == lb) {
    return core::Status::success();
  }
  const std::size_t new_leader = (la < lb) ? la : lb;
  for (Slot s = 0; s < slot_count(nl); ++s) {
    const Gate &g = nl.slots[s];
    if (!g.live) {
      continue;
    }
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      const std::size_t f = tie_flat(s, i);
      const std::size_t l = tie_leader_flat(nl, f);
      if (l == la || l == lb) {
        nl.tie_rep[f] = static_cast<Slot>(new_leader + 1u);
      }
    }
  }
  return core::Status::success();
}

[[nodiscard]] inline core::Status ref_untie_pin(Netlist &nl, PinRef pin, std::span<std::size_t> survivors) noexcept {
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
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  if ((g.ignored_mask & (1u << pin.input)) != 0) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  const std::size_t flat = tie_flat(pin.slot, pin.input);
  if (nl.tie_rep[flat] == 0) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  if (survivors.size() < slot_count(nl) * kMaxInputs) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  const std::size_t leader = tie_leader_flat(nl, flat);
  std::size_t n_surv = 0;
  for (Slot s = 0; s < slot_count(nl); ++s) {
    const Gate &o = nl.slots[s];
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
  ref_tie_reelect(nl, survivors.data(), n_surv);
  return core::Status::success();
}

inline void ref_detach_slot_ties(Netlist &nl, Slot s, std::span<std::size_t> survivors) noexcept {
  if (s >= slot_count(nl) || !nl.slots[s].live || survivors.size() < slot_count(nl) * kMaxInputs) {
    return;
  }
  const std::uint8_t n = nl.slots[s].input_count;
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
    std::size_t n_surv = 0;
    for (Slot t = 0; t < slot_count(nl); ++t) {
      if (t == s) {
        continue;
      }
      const Gate &o = nl.slots[t];
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
    for (std::uint8_t i = 0; i < n; ++i) {
      nl.tie_rep[tie_flat(s, i)] = 0;
    }
    for (std::size_t k = 0; k < n_surv; ++k) {
      nl.tie_rep[survivors[k]] = 0;
    }
    ref_tie_reelect(nl, survivors.data(), n_surv);
  }
  for (std::size_t i = 0; i < kMaxInputs; ++i) {
    nl.tie_rep[tie_flat(s, static_cast<std::uint8_t>(i))] = 0;
  }
}

inline void ref_reindex_kept_ties(Netlist &nl, Slot s, const std::size_t *orig_leader, const bool *orig_tied, std::uint8_t n,
                                  std::span<std::size_t> survivors) noexcept {
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
    std::size_t n_surv = 0;
    for (std::uint8_t j = 0; j < n; ++j) {
      if (orig_tied[j] && orig_leader[j] == orig) {
        survivors[n_surv++] = tie_flat(s, j);
      }
    }
    for (Slot t = 0; t < slot_count(nl); ++t) {
      if (t == s) {
        continue;
      }
      const Gate &o = nl.slots[t];
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
    ref_tie_reelect(nl, survivors.data(), n_surv);
  }
}

// --- optimize_barrier.hpp: drives_shared (pre-T023) ---------------------------

[[nodiscard]] inline bool ref_drives_shared(const Netlist &nl, Slot s) noexcept {
  if (s >= slot_count(nl)) {
    return false;
  }
  for (Slot u = 0; u < slot_count(nl); ++u) {
    const Gate &g = nl.slots[u];
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
      if (shared_uses_driver_flat(nl, flat, s)) {
        return true;
      }
    }
  }
  return false;
}

// --- optimize_passes.hpp: rewire_users (pre-T024) -----------------------------

inline void ref_rewire_users(Netlist &nl, Slot from_slot, Slot target) noexcept {
  for (Slot u = 0; u < slot_count(nl); ++u) {
    Gate &user = nl.slots[u];
    if (!user.live) {
      continue;
    }
    for (std::uint8_t i = 0; i < user.input_count; ++i) {
      if (user.drivers[i] == from_slot) {
        user.drivers[i] = target;
        if (target == kNoDriver) {
          user.ignored_mask =
              static_cast<std::uint8_t>(user.ignored_mask & static_cast<std::uint8_t>(~(1u << i)));
        }
      }
    }
  }
}

// --- netlist.hpp: remove_gate core (pre-T016; sink-sweep loop frozen) ---------
// Calls the live detach/unwire seams (semantics pinned by their own
// batteries); only the full-pool sink discovery is frozen here.

[[nodiscard]] inline core::Status ref_remove_gate(Netlist &nl, Slot slot, std::span<std::size_t> survivors) noexcept {
  if (slot >= slot_count(nl)) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  Gate &g = nl.slots[slot];
  if (!g.live) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  if (survivors.size() < slot_count(nl) * kMaxInputs) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  detach_slot_ties(nl, slot, survivors);
  for (std::uint8_t i = 0; i < g.input_count; ++i) {
    const Slot drv = g.drivers[i];
    if (drv != kNoDriver && drv < slot_count(nl) && nl.slots[drv].live) {
      const std::size_t flat = tie_flat(slot, i);
      if (nl.driver_inv[flat] != 0) {
        nl.driver_inv[flat] = 0;
        if (nl.slots[drv].nq_fanout > 0) {
          --nl.slots[drv].nq_fanout;
        }
      } else if (nl.slots[drv].output_fanout > 0) {
        --nl.slots[drv].output_fanout;
      }
    }
    g.drivers[i] = kNoDriver;
    const std::size_t flat = tie_flat(slot, i);
    for_each_extra(nl, flat, [&](std::uint8_t, Slot edrv, std::uint8_t einv) {
      if (edrv < slot_count(nl) && nl.slots[edrv].live) {
        if (einv != 0) {
          if (nl.slots[edrv].nq_fanout > 0) {
            --nl.slots[edrv].nq_fanout;
          }
        } else if (nl.slots[edrv].output_fanout > 0) {
          --nl.slots[edrv].output_fanout;
        }
      }
    });
    shared_clear_flat(nl, flat);
  }
  for (Slot s = 0; s < slot_count(nl); ++s) {
    Gate &o = nl.slots[s];
    if (!o.live) {
      continue;
    }
    for (std::uint8_t i = 0; i < o.input_count; ++i) {
      if (o.drivers[i] == slot) {
        (void)unwire(nl, s, i);
      } else {
        const std::size_t flat = tie_flat(s, i);
        if (shared_uses_driver_flat(nl, flat, slot)) {
          (void)unwire_driver(nl, slot, s, i);
        }
      }
    }
  }
  g.live = false;
  g.ignored_mask = 0;
  g.ignore_output = false;
  g.ignore_nq_output = true;
  g.output_fanout = 0;
  g.nq_fanout = 0;
  g.input_count = default_fanin(g.kind);
  for (std::size_t i = 0; i < kMaxInputs; ++i) {
    g.drivers[i] = kNoDriver;
    nl.driver_inv[tie_flat(slot, static_cast<std::uint8_t>(i))] = 0;
    nl.pull_cfg[tie_flat(slot, static_cast<std::uint8_t>(i))] = 0;
  }
  if (nl.live_count > 0) {
    --nl.live_count;
  }
  nl.dirty = true;
  return core::Status::success();
}

// --- netlist.hpp: add_gate core (pre-T015; lowest-free scan frozen) -----------
// Standalone copy for the FR-017 slot-assignment replay check.

[[nodiscard]] inline core::Result<Slot> ref_add_gate(Netlist &nl, GateKind kind, std::uint8_t inputs) {
  if (!valid_fanin(kind, inputs)) {
    return core::Result<Slot>::failure("bad fan-in for kind");
  }
  const std::uint8_t ord = kind_ordinal(kind);
  if (ord > 8 && !is_sequential_kind(kind)) {
    return core::Result<Slot>::failure("bad gate kind");
  }
  if (nl.next_counter[ord] == 0) {
    nl.next_counter[ord] = 1;
  }
  Slot slot = kNoDriver;
  for (Slot s = 0; s < slot_count(nl); ++s) {
    if (!nl.slots[s].live) {
      slot = s;
      break;
    }
  }
  if (slot == kNoDriver) {
    const std::size_t old = slot_count(nl);
    if (!ensure_slots(nl, old + 1)) {
      return core::Result<Slot>::failure("netlist full");
    }
    slot = static_cast<Slot>(old);
  }
  Gate &gate = nl.slots[slot];
  gate.live = true;
  gate.kind = kind;
  gate.counter = nl.next_counter[ord]++;
  if (nl.next_counter[ord] == 0) {
    nl.next_counter[ord] = 1;
  }
  gate.input_count = inputs;
  gate.ignored_mask = 0;
  gate.ignore_output = false;
  gate.ignore_nq_output = is_sequential_kind(kind);
  gate.output_fanout = 0;
  gate.nq_fanout = 0;
  gate.propagation_delay_ps = 0;
  for (std::size_t i = 0; i < kMaxInputs; ++i) {
    gate.drivers[i] = kNoDriver;
    nl.driver_inv[tie_flat(slot, static_cast<std::uint8_t>(i))] = 0;
  }
  ++nl.live_count;
  nl.dirty = true;
  return core::Result<Slot>::success(slot);
}

// --- optimize_o2_dedup.hpp: dedup double loop (pre-T025) -----------------------
// Merge decisions frozen here; edge moves go through ref_rewire_users /
// ref_remove_gate (frozen above) and the live recount (never replaced).

inline bool ref_dedup_universal_pass_impl(Netlist &nl, GateKind u, detail::MergeTied merge, const bool *feeds_seq,
                                          std::span<std::size_t> tie_survivors) noexcept {
  detail::recount_fanout(nl);
  bool changed = false;
  for (Slot s = 0; s < slot_count(nl); ++s) {
    Gate &a = nl.slots[s];
    if (!a.live || a.kind != u || a.output_fanout == 0) {
      continue;
    }
    if (a.ignored_mask != 0) {
      continue;
    }
    std::uint32_t a_sorted[kMaxInputs];
    bool a_matchable = true;
    for (std::uint8_t i = 0; i < a.input_count; ++i) {
      const detail::DedupKey k = detail::dedup_key_for(nl, s, i, merge);
      a_sorted[i] = k.v;
      if (!k.matchable()) {
        a_matchable = false;
        break;
      }
    }
    if (!a_matchable) {
      continue;
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
    for (Slot t = static_cast<Slot>(s + 1); t < slot_count(nl); ++t) {
      Gate &b = nl.slots[t];
      if (!b.live || b.kind != u || b.output_fanout == 0) {
        continue;
      }
      if (b.input_count != a.input_count || b.ignored_mask != 0) {
        continue;
      }
      std::uint32_t b_sorted[kMaxInputs];
      bool b_matchable = true;
      for (std::uint8_t i = 0; i < b.input_count; ++i) {
        const detail::DedupKey k = detail::dedup_key_for(nl, t, i, merge);
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
      if (feeds_seq != nullptr && feeds_seq[t]) {
        continue;
      }
      if (ref_drives_shared(nl, t) || ref_drives_shared(nl, s)) {
        continue;
      }
      ref_rewire_users(nl, t, s);
      (void)ref_remove_gate(nl, t, tie_survivors);
      changed = true;
    }
  }
  if (changed) {
    detail::recount_fanout(nl);
    nl.dirty = true;
    nl.topo_order.clear();
  }
  return changed;
}

} // namespace wengine::logic::scan_ref
