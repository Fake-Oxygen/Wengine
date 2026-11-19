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
 * (fixed stack arrays, no allocation).
 *
 * Dependency rules: header-only, allocation-free, exception-free,
 * noexcept throughout. Lives in the wengine::logic::detail seam —
 * internal to the optimizer family; depends on eval/netlist/types
 * plus the barrier and exact seams. Never depends on CLI types.
 * Consumers: optimize.hpp (fixpoint driver) and optimize_o2_*.hpp
 * (classify/recount reuse).
 */

#include "wengine/core/status.hpp"
#include "wengine/logic/eval.hpp"
#include "wengine/logic/netlist.hpp"
#include "wengine/logic/optimize_types.hpp"
#include "wengine/logic/optimize_barrier.hpp"
#include "wengine/logic/optimize_exact.hpp"
#include "wengine/logic/types.hpp"

#include <cstddef>
#include <cstdint>

namespace wengine::logic {

namespace detail {

/**
 * @brief Recompute output_fanout for every gate from the wire list
 *        (deterministic; avoids drift after direct driver surgery).
 *
 * @param[in,out] nl Netlist whose fanout counters are rebuilt in place.
 */
inline void recount_fanout(Netlist &nl) noexcept {
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    nl.gates[s].output_fanout = 0;
  }
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    const Gate &g = nl.gates[s];
    if (!g.live) {
      continue;
    }
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      const std::uint16_t d = g.drivers[i];
      if (d != kNoDriver && d < kMaxGates && nl.gates[d].live) {
        ++nl.gates[d].output_fanout;
      }
    }
    // 011: every extra edge counts exactly like a primary (same
    // recount semantics the barrier roots rely on).
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      const std::size_t flat = tie_flat(s, i);
      const std::uint8_t extra_n = shared_extra_flat(nl, flat);
      for (std::uint8_t j = 0; j < extra_n; ++j) {
        const std::uint16_t d = nl.extra_drivers[flat * kMaxSharedExtra + j];
        if (d < kMaxGates && nl.gates[d].live) {
          ++nl.gates[d].output_fanout;
        }
      }
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
inline void retag(Netlist &nl, std::uint16_t slot, GateKind kind) noexcept {
  const std::uint8_t ord = kind_ordinal(kind);
  Gate &g = nl.gates[slot];
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
 * @param[out] label Per-slot Constness labels.
 */
inline void classify(const Netlist &nl, Constness *label) noexcept {
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    label[s] = Constness::kV;
  }
  for (std::uint16_t oi = 0; oi < nl.topo_size; ++oi) {
    const std::uint16_t s = nl.topo_order[oi];
    const Gate &g = nl.gates[s];
    bool any_c0 = false;
    bool any_c1 = false;
    bool any_v = false;
    std::uint8_t c1_count = 0;
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      Constness c = Constness::kV;
      const std::uint16_t d = g.drivers[i];
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
 * @return True when any gate was removed.
 */
inline bool dce_pass(Netlist &nl, const bool *is_root) noexcept {
  bool keep[kMaxGates] = {};
  follow_drivers(nl, is_root, keep);
  bool changed = false;
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    if (nl.gates[s].live && !keep[s]) {
      (void)remove_gate(nl, s);
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
inline void rewire_users(Netlist &nl, std::uint16_t from_slot, std::uint16_t target) noexcept {
  for (std::uint16_t u = 0; u < kMaxGates; ++u) {
    Gate &user = nl.gates[u];
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

/**
 * @brief True when any live sequential gate reads gate s.
 *
 * Prefer the precomputed feeds_seq bit (see optimize_barrier.hpp)
 * inside passes; this helper stays for single-shot queries.
 *
 * @param[in] nl Netlist to scan.
 * @param[in] s Slot to test.
 * @return True when a live sequential gate is driven by s.
 */
inline bool has_live_seq_user(const Netlist &nl, std::uint16_t s) noexcept {
  bool feeds[kMaxGates] = {};
  compute_feeds_seq(nl, feeds);
  return s < kMaxGates && feeds[s];
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
 * @return True when any gate was bypassed.
 */
inline bool bypass_pass(Netlist &nl, const bool *feeds_seq = nullptr) noexcept {
  recount_fanout(nl);
  bool changed = false;
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    Gate &g = nl.gates[s];
    if (!g.live || g.output_fanout == 0) {
      continue;
    }
    if (feeds_seq != nullptr && feeds_seq[s]) {
      continue; // opaque barrier: never rewire a sequential-gate input pin
    }
    if (drives_shared(nl, s)) {
      continue; // 011 driver-side freeze: never rewire a shared-net span
    }
    std::uint16_t target = kNoDriver;
    bool fire = false;
    if (g.kind == GateKind::kBuf) {
      target = g.drivers[0];
      fire = true;
    } else if (g.kind == GateKind::kNot) {
      const std::uint16_t h = g.drivers[0];
      if (h != kNoDriver && h != s && nl.gates[h].live && nl.gates[h].kind == GateKind::kNot) {
        target = nl.gates[h].drivers[0];
        fire = true;
      }
    }
    if (!fire || target == s) {
      continue;
    }
    rewire_users(nl, s, target);
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
 * @return True when inputs were dropped.
 * @note Opaque barrier: sequential gates are never re-typed (returns
 *       false at once).
 */
inline bool drop_inputs(Netlist &nl, std::uint16_t s, Constness *label, const Constness *ic, bool drop_any, Constness drop_c, bool flip) noexcept {
  Gate &g = nl.gates[s];
  if (is_frozen_kind(g.kind)) {
    return false; // opaque barrier: sequential gates are never re-typed
  }
  std::uint16_t kept_drv[kMaxInputs];
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
  // Rebuild ties for kept pins (indices may have shifted) via the TieSet
  // seam — index-shift re-election lives in tie_set.hpp, not here.
  reindex_kept_ties(nl, s, orig_leader, orig_tied, n);
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
 * @return True when the net changed.
 * @note Opaque barrier: frozen (sequential) gates are never folded or
 *       dropped; constant roots collapse to canonical AND/NAND form.
 */
inline bool const_fold_pass(Netlist &nl, const bool *is_root) noexcept {
  Constness label[kMaxGates];
  classify(nl, label);
  recount_fanout(nl);
  bool changed = false;
  for (std::uint16_t oi = 0; oi < nl.topo_size; ++oi) {
    const std::uint16_t s = nl.topo_order[oi];
    Gate &g = nl.gates[s];
    if (!g.live) {
      continue;
    }
    Constness ic[kMaxInputs];
    bool has_c0 = false;
    bool has_c1 = false;
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      Constness c = Constness::kV;
      const std::uint16_t d = g.drivers[i];
      if (d != kNoDriver) {
        c = label[d];
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
    switch (g.kind) {
    case GateKind::kAnd:
      if (!has_c0) {
        changed |= drop_inputs(nl, s, label, ic, false, Constness::kC1, false);
      }
      break;
    case GateKind::kOr:
      if (!has_c1) {
        changed |= drop_inputs(nl, s, label, ic, false, Constness::kC0, false);
      }
      break;
    case GateKind::kNand:
      if (!has_c0) {
        changed |= drop_inputs(nl, s, label, ic, false, Constness::kC1, false);
      }
      break;
    case GateKind::kNor:
      if (!has_c1) {
        changed |= drop_inputs(nl, s, label, ic, false, Constness::kC0, false);
      }
      break;
    case GateKind::kXor:
    case GateKind::kXnor:
      changed |= drop_inputs(nl, s, label, ic, true, Constness::kC0, true);
      break;
    default:
      break; // label-only; users/DCE/collapse handle the rest
    }
  }
  // Collapse constant roots in place (same slot: outputs stay positional).
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    if (!is_root[s]) {
      continue;
    }
    Gate &g = nl.gates[s];
    if (!g.live) {
      continue;
    }
    if (is_frozen_kind(g.kind)) {
      continue; // opaque barrier: labels stay kV, but never collapse state
    }
    if (has_marked_input(nl, s)) {
      continue; // 011 sink-side skip: collapsing would destroy the span
    }
    if (label[s] == Constness::kC0) {
      const bool canonical = g.kind == GateKind::kAnd && g.input_count == 2 && g.ignored_mask == 0x03u &&
                             g.drivers[0] == kNoDriver && g.drivers[1] == kNoDriver;
      if (!canonical) {
        detach_slot_ties(nl, s);
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
    } else if (label[s] == Constness::kC1) {
      const bool canonical = g.kind == GateKind::kNand && g.input_count == 2 &&
                             g.ignored_mask == 0x03u && g.drivers[0] == kNoDriver &&
                             g.drivers[1] == kNoDriver;
      if (!canonical) {
        detach_slot_ties(nl, s);
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
    nl.topo_size = 0;
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
 * @return True when any fusion fired.
 * @note Opaque barrier: never fuses through a state-element driver and
 *       never rewires a sequential-gate input pin. Tie classes of fused
 *       pins follow their source counterparts.
 */
inline bool fusion_sweep(Netlist &nl, const bool *feeds_seq = nullptr) noexcept {
  recount_fanout(nl);
  bool changed = false;
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    Gate &g = nl.gates[s];
    if (!g.live || g.kind != GateKind::kNot) {
      continue;
    }
    const std::uint16_t h = g.drivers[0];
    if (h == kNoDriver || h == s || !nl.gates[h].live) {
      continue;
    }
    // 011: never fuse through a shared span — driver-side (rewiring s's
    // users would miss extras) or sink-side (the driver-array copy
    // below only moves primaries and would corrupt the span).
    if (drives_shared(nl, s) || has_marked_input(nl, s) || has_marked_input(nl, h)) {
      continue;
    }
    const GateKind hk = nl.gates[h].kind;
    if (hk == GateKind::kNot) {
      if (g.output_fanout == 0) {
        continue; // output double-negation preserved
      }
      if (feeds_seq != nullptr && feeds_seq[s]) {
        continue; // opaque barrier: never rewire a sequential-gate input pin
      }
      const std::uint16_t t = nl.gates[h].drivers[0];
      if (t == s) {
        continue; // defensive: never rewire a gate into itself
      }
      rewire_users(nl, s, t);
      (void)remove_gate(nl, s);
      changed = true;
    } else if (hk == GateKind::kBuf) {
      g.drivers[0] = nl.gates[h].drivers[0];
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
      const Gate &src = nl.gates[h];
      g.input_count = src.input_count;
      g.ignored_mask = src.ignored_mask;
      for (std::size_t i = 0; i < kMaxInputs; ++i) {
        g.drivers[i] = src.drivers[i];
      }
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
    nl.topo_size = 0;
  }
  return changed;
}


} // namespace detail

} // namespace wengine::logic
