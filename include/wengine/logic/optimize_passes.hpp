#pragma once

/**
 * @file
 * Structural optimization passes (O0/O1) plus the shared
 * classification machinery of the optimize stage of the logic pipeline
 * (netlist parse -> state -> eval -> layout/topo -> optimize):
 * recount/retag/classify bookkeeping, dead-gate elimination, BUF/
 * double-NOT bypass, constant folding, and the De Morgan fusion
 * sweep. optimize.hpp only orchestrates these into the O0/O1
 * fixpoint; the O2 fresh build lives in optimize_o2_*.hpp and the
 * barrier vocabulary in optimize_barrier.hpp.
 *
 * What each pass preserves: function (truth table) exactly; the net
 * never grows (O0 passes); sequential gates are opaque barriers — no
 * pass retypes, folds, drops, bypasses, fuses, or rewires through a
 * state element or its input pins (feeds_seq guards every rewire).
 * Cost class: linear sweeps over slots/inputs per fixpoint iteration
 * (caller-owned buffers, no allocation).
 *
 * Capacity (dynamic pool): every pass works over Netlist. Passes with
 * pool-sized scratch take caller-owned spans: dce_pass (keep + DFS
 * stack), const_fold_pass (labels), and drop_inputs (tie survivors via
 * reindex_kept_ties). All other locals are capacity-free
 * (kMaxInputs-sized or bounded by the n<=10/m<=16 exact caps).
 * Internal cross-calls are bare (no capacity parameter).
 *
 * Dependency rules: header-only, allocation-free, exception-free,
 * noexcept throughout. Lives in the wengine::logic::detail seam —
 * internal to the optimizer family; depends on eval/netlist/types
 * plus the barrier and exact seams. Never depends on CLI types.
 * Consumers: optimize.hpp (fixpoint driver) and optimize_o2_*.hpp
 * (classify/recount reuse).
 */

#include "wengine/core/status.hpp"
#include "wengine/logic/adjacency.hpp"
#include "wengine/logic/eval.hpp"
#include "wengine/logic/netlist.hpp"
#include "wengine/logic/optimize_types.hpp"
#include "wengine/logic/optimize_barrier.hpp"
#include "wengine/logic/optimize_exact.hpp"
#include "wengine/logic/shared_net.hpp"
#include "wengine/logic/types.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace wengine::logic {

namespace detail {

/**
 * @brief Recompute output_fanout for every gate from the wire list
 *        (deterministic; avoids drift after direct driver surgery).
 *
 * @param[in,out] nl Netlist whose fanout counters are rebuilt in place.
 */
 inline void recount_fanout(Netlist &nl) noexcept {
  for (Slot s = 0; s < slot_count(nl); ++s) {
    nl.slots[s].output_fanout = 0;
  }
  for (Slot s = 0; s < slot_count(nl); ++s) {
    const Gate &g = nl.slots[s];
    if (!g.live) {
      continue;
    }
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      const Slot d = g.drivers[i];
      if (d != kNoDriver && d < slot_count(nl) && nl.slots[d].live) {
        ++nl.slots[d].output_fanout;
      }
    }
    // 011: every extra edge counts exactly like a primary (same
    // recount semantics the barrier roots rely on).
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      for_each_extra(nl, tie_flat(s, i), [&](std::uint8_t, Slot d, std::uint8_t) {
        if (d < slot_count(nl) && nl.slots[d].live) {
          ++nl.slots[d].output_fanout;
        }
      });
    }
  }
}

// detach_slot_ties lives in tie_set.hpp (the TieSet seam owns all tie_rep
// writes); this file calls it for constant-collapse paths.

/**
 * @brief Fresh per-kind counter after an in-place kind change: display
 *        names stay unique (resolve_gate returns the first slot match,
 *        so duplicates would be ambiguous). Deterministic: sweep order
 *        is fixed.
 *
 * @param[out] nl Netlist holding the gate and counters.
 * @param[in] slot Slot to retag.
 * @param[in] kind New kind.
 */
 inline void retag(Netlist &nl, Slot slot, GateKind kind) noexcept {
  const std::uint8_t ord = kind_ordinal(kind);
  Gate &g = nl.slots[slot];
  g.kind = kind;
  g.counter = nl.next_counter[ord]++;
  if (nl.next_counter[ord] == 0) { // u32 wrap: 0 is never assigned
    nl.next_counter[ord] = 1;
  }
}

/**
 * @brief Classify every live gate output in topo order: ignored
 *        dangling inputs read as C0 (they tie low in eval); wired inputs
 *        inherit the driver's label; dangling block inputs are V.
 *        Sequential outputs always classify as V (opaque barrier).
 *
 * @param[in] nl Netlist to classify (topology must be compiled).
 * @param[out] label Per-slot Constness labels (caller-owned).
 */
 inline void classify(const Netlist &nl, Constness *label) noexcept {
  for (Slot s = 0; s < slot_count(nl); ++s) {
    label[s] = Constness::kV;
  }
  for (Slot oi = 0; oi < nl.topo_order.size(); ++oi) {
    const Slot s = nl.topo_order[oi];
    const Gate &g = nl.slots[s];
    bool any_c0 = false;
    bool any_c1 = false;
    bool any_v = false;
    std::uint8_t c1_count = 0;
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      Constness c = Constness::kV;
      const Slot d = g.drivers[i];
      if (d != kNoDriver) {
        c = label[d];
      } else if ((g.ignored_mask & static_cast<std::uint8_t>(1u << i)) != 0) {
        c = Constness::kC0;
      }
      if (c == Constness::kC0) {
        any_c0 = true;
      } else if (c == Constness::kC1) {
        any_c1 = true;
        ++c1_count;
      } else {
        any_v = true;
      }
    }
    Constness out = Constness::kV;
    if (is_frozen_kind(g.kind)) {
      // Opaque barrier: sequential outputs always classify as kV.
      label[s] = Constness::kV;
      continue;
    }
    if (has_marked_input(nl, s)) {
      // 011: a marked input can resolve X (contention) regardless of
      // its drivers' labels — shared sinks always classify as kV so no
      // downstream fold can mask a loud conflict.
      label[s] = Constness::kV;
      continue;
    }
    // 012: kTbuf intentionally has no switch arm below — an enable-gated
    // buffer resolves Z (released) or its data level, never a constant,
    // so it holds kV through the default arm and no fold can lift it
    // (buffer-gate freeze, research D7).
    switch (g.kind) {
    case GateKind::kAnd:
      out = any_c0 ? Constness::kC0 : (any_v ? Constness::kV : Constness::kC1);
      break;
    case GateKind::kNand:
      out = any_c0 ? Constness::kC1 : (any_v ? Constness::kV : Constness::kC0);
      break;
    case GateKind::kOr:
      out = any_c1 ? Constness::kC1 : (any_v ? Constness::kV : Constness::kC0);
      break;
    case GateKind::kNor:
      out = any_c1 ? Constness::kC0 : (any_v ? Constness::kV : Constness::kC1);
      break;
    case GateKind::kXor:
      out = any_v ? Constness::kV : (((c1_count % 2u) != 0u) ? Constness::kC1 : Constness::kC0);
      break;
    case GateKind::kXnor:
      out = any_v ? Constness::kV : (((c1_count % 2u) != 0u) ? Constness::kC0 : Constness::kC1);
      break;
    case GateKind::kNot:
      out = any_c0 ? Constness::kC1 : (any_c1 ? Constness::kC0 : Constness::kV);
      break;
    case GateKind::kBuf:
      out = any_c0 ? Constness::kC0 : (any_c1 ? Constness::kC1 : Constness::kV);
      break;
    default:
      break; // frozen kinds handled above; unknown kinds stay varying
    }
    label[s] = out;
  }
}

/**
 * @brief Delete every live gate that cannot reach a root sink.
 *
 * Roots are the original sinks (see optimize.hpp): optimization orphans
 * vanish, user logic (including ignored-output cones) is never deleted.
 *
 * @param[out] nl Netlist to prune.
 * @param[in] is_root Per-slot root flags (barrier roots: original sinks
 *            plus every live sequential gate).
 * @param[in] keep Caller-owned scratch (pool size reachability flags).
 * @param[in] stack Caller-owned DFS stack (pool size slots; see
 *            follow_drivers).
 * @param[in] tie_survivors Caller-owned scratch (`slot_count(nl) * kMaxInputs`
 *            flats) for remove_gate's tie-detach sweep.
 * @return True when any gate was removed.
 */

inline bool dce_pass(Netlist &nl, const bool *is_root, std::span<bool> keep, std::span<Slot> stack, std::span<std::size_t> tie_survivors) noexcept {
  if (keep.size() < slot_count(nl)) {
    return false;
  }
  follow_drivers(nl, is_root, keep.data(), stack);
  bool changed = false;
  for (Slot s = 0; s < slot_count(nl); ++s) {
    if (nl.slots[s].live && !keep[s]) {
      (void)remove_gate(nl, s, tie_survivors);
      changed = true;
    }
  }
  if (changed) {
    recount_fanout(nl);
  }
  return changed;
}

/**
 * @brief Rewire every input currently driven by from_slot to target.
 *
 * When the target dangles, the new block input must be visible: clear
 * its ignored bit (the mark was inert while wired; keeping it would
 * silently tie the pin low instead of exposing the real dependency).
 *
 * @param[out] nl Netlist receiving the rewiring.
 * @param[in] from_slot Driver slot being replaced.
 * @param[in] target Replacement driver (kNoDriver exposes a dangling
 *            block input).
 */

inline void rewire_users(Netlist &nl, Slot from_slot, Slot target) noexcept {
  if (target == from_slot) {
    return; // defensive: callers always rewire onto a distinct driver
  }
  // Walk the replaced driver's recorded primary edges (016): extras are
  // frozen spans (011 driver-side freeze) and never move. The head is
  // re-read per edge and the driver's chain strictly shrinks, so this
  // terminates with exactly the old scan's rewrites at
  // fanout-proportional cost.
  while (from_slot < nl.succ_head.size() && nl.succ_head[from_slot] != kNoDriver) {
    Slot e = nl.succ_head[from_slot];
    while (e != kNoDriver && nl.succ_extra[e] != 0) {
      e = nl.succ_next[e];
    }
    if (e == kNoDriver) {
      break; // only frozen extras remain
    }
    const Slot u = nl.succ_sink[e];
    const std::uint8_t i = nl.succ_input[e];
    Gate &user = nl.slots[u];
    if (user.drivers[i] != from_slot) {
      (void)adjacency::adj_unlink_edge(nl, from_slot, u, i, 0); // phantom: drop, never hang
      continue;
    }
    user.drivers[i] = target;
    // Adjacency (016): the edge moves drivers. Unlink-then-insert
    // reuses the freed slot.
    (void)adjacency::adj_unlink_edge(nl, from_slot, u, i, 0);
    if (target != kNoDriver) {
      (void)adjacency::adj_insert_edge(nl, target, u, i, 0);
    }
    // Feeder counters (016): the edge left/joined a marked input.
    const std::size_t flat = tie_flat(u, i);
    if (is_shared_flat(nl, flat)) {
      if (nl.feeds_shared[from_slot] > 0) {
        --nl.feeds_shared[from_slot];
      }
      if (target != kNoDriver && target < slot_count(nl) && nl.slots[target].live) {
        ++nl.feeds_shared[target];
      }
    }
    if (target == kNoDriver) {
      user.ignored_mask = static_cast<std::uint8_t>(user.ignored_mask & static_cast<std::uint8_t>(~(1u << i)));
    }
  }
}

/**
 * @brief True when any live sequential gate reads gate s.
 *
 * Prefer the precomputed feeds_seq bit (see optimize_barrier.hpp)
 * inside passes; this helper stays for single-shot queries.
 *
 * @param[in] nl Netlist to scan.
 * @param[in] s Slot to test.
 * @param[in] feeds Caller-owned scratch (pool size flags).
 * @return True when a live sequential gate is driven by s.
 */

inline bool has_live_seq_user(const Netlist &nl, Slot s, std::span<bool> feeds) noexcept {
  if (feeds.size() < slot_count(nl)) {
    return false;
  }
  compute_feeds_seq(nl, feeds.data());
  return s < slot_count(nl) && feeds[s];
}

/**
 * @brief BUF(x) to x and NOT(NOT(x)) to x for internal gates (fanout
 *        greater than 0; block outputs are preserved for interface
 *        stability).
 *
 * Single ascending sweep collapses chains regardless of slot order.
 * Gates feeding a sequential gate are never removed (opaque barrier:
 * flop/latch input pins freeze). feeds_seq is the precomputed barrier
 * bit (nullptr means no sequential gates: skip the check).
 *
 * @param[out] nl Netlist to simplify.
 * @param[in] feeds_seq Per-slot sequential-user flags, or nullptr.
 * @param[in] tie_survivors Caller-owned scratch (`slot_count(nl) * kMaxInputs`
 *            flats) for remove_gate's tie-detach sweep.
 * @return True when any gate was bypassed.
 * @note Opaque barrier: never rewire a sequential-gate input pin and
 *       never remove a gate feeding one.
 */

inline bool bypass_pass(Netlist &nl, const bool *feeds_seq, std::span<std::size_t> tie_survivors) noexcept {
  recount_fanout(nl);
  bool changed = false;
  for (Slot s = 0; s < slot_count(nl); ++s) {
    Gate &g = nl.slots[s];
    if (!g.live || g.output_fanout == 0) {
      continue;
    }
    if (feeds_seq != nullptr && feeds_seq[s]) {
      continue; // opaque barrier: never rewire a sequential-gate input pin
    }
    if (drives_shared(nl, s)) {
      continue; // 011 driver-side freeze: never rewire a shared-net span
    }
    if (has_pulled_input(nl, s)) {
      continue; // 012: never bypass a pulled-input gate — its resolved
      // output carries the idle mapping, which the raw driver target
      // below would bypass (value + interface change)
    }
    Slot target = kNoDriver;
    bool fire = false;
    if (g.kind == GateKind::kBuf) {
      target = g.drivers[0];
      fire = true;
    } else if (g.kind == GateKind::kNot) {
      const Slot h = g.drivers[0];
      if (h != kNoDriver && h != s && nl.slots[h].live && nl.slots[h].kind == GateKind::kNot &&
          !has_pulled_input(nl, h)) {
        // 012: never rewire through a pulled-input NOT — its resolved
        // output carries the idle mapping, which the raw target below
        // would bypass (value + interface change)
        target = nl.slots[h].drivers[0];
        fire = true;
      }
    }
    if (!fire || target == s) {
      continue;
    }
    rewire_users(nl, s, target);
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

/**
 * @brief Drop constant inputs of gate s (drop_any drops all C0/C1
 *        inputs; otherwise only drop_c). Odd C1 count flips XOR to XNOR
 *        when asked.
 *
 * Single remainders become BUF (AND/OR/XOR) or NOT (NAND/NOR/XNOR). An
 * all-constant gate is left alone (label-only; the caller collapses
 * roots and DCE removes the rest). Remaining inputs are all varying, so
 * the gate relabels to V. Tied pins are always varying (dangling,
 * non-ignored), so dropped pins are untied — but kept tied pins shift
 * indices, so their classes re-elect around the new flats (outside
 * members follow).
 *
 * @param[out] nl Netlist holding the gate.
 * @param[in] s Slot to compact.
 * @param[out] label Per-slot Constness labels (s relabels to V).
 * @param[in] ic Per-input Constness of s.
 * @param[in] drop_any True drops every constant input.
 * @param[in] drop_c Constant class to drop when drop_any is false.
 * @param[in] flip True flips XOR/XNOR on an odd dropped-C1 count.
 * @param[in] tie_survivors Caller-owned scratch (`slot_count(nl) * kMaxInputs`
 *            flats) for kept-tie re-election.
 * @return True when inputs were dropped.
 * @note Opaque barrier: sequential gates are never re-typed (returns
 *       false at once).
 */

inline bool drop_inputs(Netlist &nl, Slot s, Constness *label, const Constness *ic, bool drop_any,
                         Constness drop_c, bool flip, std::span<std::size_t> tie_survivors) noexcept {
  Gate &g = nl.slots[s];
  if (is_frozen_kind(g.kind)) {
    return false; // opaque barrier: sequential gates are never re-typed
  }
  Slot kept_drv[kMaxInputs];
  std::uint8_t kept_ign[kMaxInputs];
  std::uint8_t kept_orig[kMaxInputs];
  std::uint8_t n = 0;
  std::uint8_t c1_dropped = 0;
  for (std::uint8_t i = 0; i < g.input_count; ++i) {
    const bool is_c = drop_any ? (ic[i] != Constness::kV) : (ic[i] == drop_c);
    if (is_c) {
      if (ic[i] == Constness::kC1) {
        ++c1_dropped;
      }
      continue;
    }
    kept_drv[n] = g.drivers[i];
    kept_ign[n] = ((g.ignored_mask & static_cast<std::uint8_t>(1u << i)) != 0) ? 1u : 0u;
    kept_orig[n] = i;
    ++n;
  }
  if (n == 0 || n == g.input_count) {
    return false;
  }
  // Capture original tie leaders for kept pins before mutating indices.
  std::size_t orig_leader[kMaxInputs];
  bool orig_tied[kMaxInputs];
  for (std::uint8_t k = 0; k < n; ++k) {
    const std::size_t of = tie_flat(s, kept_orig[k]);
    orig_leader[k] = tie_leader_flat(nl, of);
    orig_tied[k] = is_tied(nl, s, kept_orig[k]);
  }
  // Adjacency (016): detach this sink's edges before the index shift,
  // re-insert the kept row after (tables are truth at each call).
  // Feeder counters ride along (marked sinks skip the pass, so the
  // pair is a no-op there — kept for unconditional correctness).
  adjacency::adj_detach_sink(nl, s);
  detail::feeds_release_sink(nl, s);
  for (std::uint8_t i = 0; i < n; ++i) {
    g.drivers[i] = kept_drv[i];
  }
  for (std::size_t i = n; i < kMaxInputs; ++i) {
    g.drivers[i] = kNoDriver;
  }
  std::uint8_t mask = 0;
  for (std::uint8_t i = 0; i < n; ++i) {
    if (kept_ign[i] != 0u) {
      mask = static_cast<std::uint8_t>(mask | static_cast<std::uint8_t>(1u << i));
    }
  }
  g.ignored_mask = mask;
  g.input_count = n;
  adjacency::adj_insert_sink(nl, s);
  detail::feeds_acquire_sink(nl, s);
  // Rebuild ties for kept pins (indices may have shifted) via the TieSet
  // seam — index-shift re-election lives in tie_set.hpp, not here.
  reindex_kept_ties(nl, s, orig_leader, orig_tied, n, tie_survivors);
  GateKind base = g.kind;
  if (flip && ((c1_dropped % 2u) != 0u)) {
    base = (base == GateKind::kXor) ? GateKind::kXnor : GateKind::kXor;
  }
  GateKind want = base;
  if (n == 1) {
    switch (base) {
    case GateKind::kAnd:
    case GateKind::kOr:
    case GateKind::kXor:
      want = GateKind::kBuf;
      break;
    case GateKind::kNand:
    case GateKind::kNor:
    case GateKind::kXnor:
      want = GateKind::kNot;
      break;
    default:
      want = base; // frozen kinds return above; NOT/BUF keep shape
      break;
    }
  }
  if (want != g.kind) {
    retag(nl, s, want);
  }
  label[s] = Constness::kV;
  return true;
}

/**
 * @brief Constant folding plus annihilator/identity sweep (forward topo
 *        order, labels updated in place so one sweep propagates whole
 *        chains). Assumes a fresh topology on entry; leaves dirty when
 *        structural.
 *
 * @param[out] nl Netlist to fold.
 * @param[in] is_root Per-slot root flags (constant roots collapse in
 *            place so outputs stay positional).
 * @param[in] labels Caller-owned scratch (pool size Constness labels).
 * @param[in] tie_survivors Caller-owned scratch (`slot_count(nl) * kMaxInputs`
 *            flats) for kept-tie re-election.
 * @return True when the net changed.
 * @note Opaque barrier: frozen (sequential) gates are never folded or
 *       dropped; constant roots collapse to canonical AND/NAND form.
 */

inline bool const_fold_pass(Netlist &nl, const bool *is_root, std::span<Constness> labels, std::span<std::size_t> tie_survivors) noexcept {
  if (labels.size() < slot_count(nl)) {
    return false;
  }
  classify(nl, labels.data());
  recount_fanout(nl);
  bool changed = false;
  for (Slot oi = 0; oi < nl.topo_order.size(); ++oi) {
    const Slot s = nl.topo_order[oi];
    Gate &g = nl.slots[s];
    if (!g.live) {
      continue;
    }
    Constness ic[kMaxInputs];
    bool has_c0 = false;
    bool has_c1 = false;
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      Constness c = Constness::kV;
      const Slot d = g.drivers[i];
      if (d != kNoDriver) {
        c = labels[d];
      } else if ((g.ignored_mask & static_cast<std::uint8_t>(1u << i)) != 0) {
        c = Constness::kC0;
      }
      ic[i] = c;
      if (c == Constness::kC0) {
        has_c0 = true;
      } else if (c == Constness::kC1) {
        has_c1 = true;
      }
    }
    if (!has_c0 && !has_c1) {
      continue;
    }
    if (is_frozen_kind(g.kind)) {
      continue; // opaque barrier: flop/latch inputs are never folded or dropped
    }
    if (has_marked_input(nl, s)) {
      continue; // 011 sink-side skip: never fold, drop, or retag through
      // a marked input (index shifts would desynchronize flat-keyed extras)
    }
    if (has_pulled_input(nl, s) || g.kind == GateKind::kTbuf) {
      continue; // 012 sink-side skip: never fold, drop, or retag through
      // a pulled input (index shifts would desynchronize flat-keyed
      // pull_cfg), and never touch a buffer gate (its 2-input shape is
      // load-bearing — dropping to n=1 would retag a fan-in-invalid
      // 1-input buffer)
    }
    switch (g.kind) {
    case GateKind::kAnd:
      if (!has_c0) {
        changed |= drop_inputs(nl, s, labels.data(), ic, false, Constness::kC1, false, tie_survivors);
      }
      break;
    case GateKind::kOr:
      if (!has_c1) {
        changed |= drop_inputs(nl, s, labels.data(), ic, false, Constness::kC0, false, tie_survivors);
      }
      break;
    case GateKind::kNand:
      if (!has_c0) {
        changed |= drop_inputs(nl, s, labels.data(), ic, false, Constness::kC1, false, tie_survivors);
      }
      break;
    case GateKind::kNor:
      if (!has_c1) {
        changed |= drop_inputs(nl, s, labels.data(), ic, false, Constness::kC0, false, tie_survivors);
      }
      break;
    case GateKind::kXor:
    case GateKind::kXnor:
      changed |= drop_inputs(nl, s, labels.data(), ic, true, Constness::kC0, true, tie_survivors);
      break;
    default:
      break; // label-only; users/DCE/collapse handle the rest
    }
  }
  // Collapse constant roots in place (same slot: outputs stay positional).
  for (Slot s = 0; s < slot_count(nl); ++s) {
    if (!is_root[s]) {
      continue;
    }
    Gate &g = nl.slots[s];
    if (!g.live) {
      continue;
    }
    if (is_frozen_kind(g.kind)) {
      continue; // opaque barrier: labels stay kV, but never collapse state
    }
    if (has_marked_input(nl, s)) {
      continue; // 011 sink-side skip: collapsing would destroy the span
    }
    if (has_pulled_input(nl, s) || g.kind == GateKind::kTbuf) {
      continue; // 012 sink-side skip: collapsing would destroy the pull
      // mapping or the buffer's load-bearing shape (see const_fold skip)
    }
    if (labels[s] == Constness::kC0) {
      const bool canonical = g.kind == GateKind::kAnd && g.input_count == 2 && g.ignored_mask == 0x03u && g.drivers[0] == kNoDriver && g.drivers[1] == kNoDriver;
      if (!canonical) {
        detach_slot_ties(nl, s, tie_survivors);
        // Adjacency (016): every driver below clears — detach first.
        adjacency::adj_detach_sink(nl, s);
        if (g.kind != GateKind::kAnd) {
          retag(nl, s, GateKind::kAnd);
        }
        g.input_count = 2;
        g.ignored_mask = 0x03u;
        g.drivers[0] = kNoDriver;
        g.drivers[1] = kNoDriver;
        for (std::size_t i = 2; i < kMaxInputs; ++i) {
          g.drivers[i] = kNoDriver;
        }
        changed = true;
      }
    } else if (labels[s] == Constness::kC1) {
      const bool canonical = g.kind == GateKind::kNand && g.input_count == 2 &&
                             g.ignored_mask == 0x03u && g.drivers[0] == kNoDriver &&
                             g.drivers[1] == kNoDriver;
      if (!canonical) {
        detach_slot_ties(nl, s, tie_survivors);
        // Adjacency (016): every driver below clears — detach first.
        adjacency::adj_detach_sink(nl, s);
        if (g.kind != GateKind::kNand) {
          retag(nl, s, GateKind::kNand);
        }
        g.input_count = 2;
        g.ignored_mask = 0x03u;
        g.drivers[0] = kNoDriver;
        g.drivers[1] = kNoDriver;
        for (std::size_t i = 2; i < kMaxInputs; ++i) {
          g.drivers[i] = kNoDriver;
        }
        changed = true;
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
 * @brief De Morgan bubble-push (O1): fuse every NOT driven by a live
 *        gate into the complementary kind in place (same slot, fresh
 *        counter).
 *
 * NOT(BUF) rewires through; output double-negations are preserved
 * (interface stability). feeds_seq is the precomputed barrier bit
 * (nullptr means no sequential gates).
 *
 * @param[out] nl Netlist to fuse.
 * @param[in] feeds_seq Per-slot sequential-user flags, or nullptr.
 * @param[in] tie_survivors Caller-owned scratch (`slot_count(nl) * kMaxInputs`
 *            flats) for remove_gate's tie-detach sweep.
 * @return True when any fusion fired.
 * @note Opaque barrier: never fuses through a state-element driver and
 *       never rewires a sequential-gate input pin. Tie classes of fused
 *       pins follow their source counterparts.
 */

inline bool fusion_sweep(Netlist &nl, const bool *feeds_seq, std::span<std::size_t> tie_survivors) noexcept {
  recount_fanout(nl);
  bool changed = false;
  for (Slot s = 0; s < slot_count(nl); ++s) {
    Gate &g = nl.slots[s];
    if (!g.live || g.kind != GateKind::kNot) {
      continue;
    }
    const Slot h = g.drivers[0];
    if (h == kNoDriver || h == s || !nl.slots[h].live) {
      continue;
    }
    // 011: never fuse through a shared span — driver-side (rewiring s's
    // users would miss extras) or sink-side (the driver-array copy
    // below only moves primaries and would corrupt the span).
    if (drives_shared(nl, s) || has_marked_input(nl, s) || has_marked_input(nl, h)) {
      continue;
    }
    // 012: never fuse through a pulled sink or a pulled driver — the
    // driver-array copy below moves primaries without their flat-keyed
    // pulls (the victim gate in test_optimizer_skips_pulled_sink fuses
    // NAND-ward and goes wrong), mirroring the 011 driver/sink pair.
    // (s is always NOT in this sweep, so no buffer-gate check is needed
    // here; the general fuse above carries the kTbuf freeze).
    if (has_pulled_input(nl, s) || has_pulled_input(nl, h)) {
      continue;
    }
    const GateKind hk = nl.slots[h].kind;
    if (hk == GateKind::kNot) {
      if (g.output_fanout == 0) {
        continue; // output double-negation preserved
      }
      if (feeds_seq != nullptr && feeds_seq[s]) {
        continue; // opaque barrier: never rewire a sequential-gate input pin
      }
      const Slot t = nl.slots[h].drivers[0];
      if (t == s) {
        continue; // defensive: never rewire a gate into itself
      }
      rewire_users(nl, s, t);
      (void)remove_gate(nl, s, tie_survivors);
      changed = true;
    } else if (hk == GateKind::kBuf) {
      // Adjacency (016): single edge move (h->s,0 becomes t->s,0).
      const Slot t = nl.slots[h].drivers[0];
      (void)adjacency::adj_unlink_edge(nl, h, s, 0, 0);
      g.drivers[0] = t;
      if (t != kNoDriver) {
        (void)adjacency::adj_insert_edge(nl, t, s, 0, 0);
      }
      changed = true;
    } else {
      if (is_frozen_kind(hk)) {
        continue; // opaque barrier: never fuse through a state-element driver
      }
      GateKind nk = GateKind::kNot;
      switch (hk) {
      case GateKind::kAnd:
        nk = GateKind::kNand;
        break;
      case GateKind::kNand:
        nk = GateKind::kAnd;
        break;
      case GateKind::kOr:
        nk = GateKind::kNor;
        break;
      case GateKind::kNor:
        nk = GateKind::kOr;
        break;
      case GateKind::kXor:
        nk = GateKind::kXnor;
        break;
      case GateKind::kXnor:
        nk = GateKind::kXor;
        break;
      default:
        break; // NOT/BUF handled above; unknown kinds never fuse
      }
      if (nk == GateKind::kNot) {
        continue;
      }
      const Gate &src = nl.slots[h];
      // Adjacency (016): full row copy — detach this sink's edges, copy,
      // re-insert (extras cannot be present: marked sinks skip above).
      adjacency::adj_detach_sink(nl, s);
      g.input_count = src.input_count;
      g.ignored_mask = src.ignored_mask;
      for (std::size_t i = 0; i < kMaxInputs; ++i) {
        g.drivers[i] = src.drivers[i];
      }
      adjacency::adj_insert_sink(nl, s);
      // Preserve input-input ties: each dangling dest pin joins its src
      // counterpart's class (internal + outside ties follow; the src gate
      // goes away via later DCE, leaving the fused pins tied).
      for (std::uint8_t i = 0; i < src.input_count; ++i) {
        if (src.drivers[i] != kNoDriver) {
          continue;
        }
        if ((src.ignored_mask & static_cast<std::uint8_t>(1u << i)) != 0) {
          continue;
        }
        if (!is_tied(nl, h, i)) {
          continue;
        }
        PinRef dst_pin{.slot = s, .is_output = false, .input = i};
        PinRef src_pin{.slot = h, .is_output = false, .input = i};
        (void)tie_pins(nl, dst_pin, src_pin);
      }
      retag(nl, s, nk);
      changed = true;
    }
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
