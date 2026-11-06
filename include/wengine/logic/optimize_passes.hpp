#pragma once

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
  }
}

// Sequential-aware recompilation: canonical barrier rebuild (see
// optimize_barrier.hpp). Kept as a thin forwarder so existing call sites
// and the O0/O1 fixpoint read unchanged.
inline bool recompile(Netlist &nl) noexcept { return rebuild_seq_topology(nl); }

// Detach every tied pin of slot `s` from its class (used when a gate is
// collapsed to a constant: the varying tied pins disappear, outside
// survivors re-elect). Leaves slot `s` with no tie entries.
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
      // Clear first (in case survivors shared the class with this slot,
      // their reps still point at the old leader which may live in s).
      nl.tie_rep[survivors[k]] = 0;
    }
    if (n_surv > 1) {
      std::size_t nl_leader = survivors[0];
      for (std::size_t k = 1; k < n_surv; ++k) {
        if (survivors[k] < nl_leader) {
          nl_leader = survivors[k];
        }
      }
      for (std::size_t k = 0; k < n_surv; ++k) {
        nl.tie_rep[survivors[k]] = static_cast<std::uint16_t>(nl_leader + 1u);
      }
    }
  }
  for (std::uint8_t i = 0; i < kMaxInputs; ++i) {
    nl.tie_rep[tie_flat(s, i)] = 0;
  }
}

// Fresh per-kind counter after an in-place kind change: display names stay
// unique (resolve_gate returns the first slot match, so duplicates would be
// ambiguous). Deterministic: sweep order is fixed.
inline void retag(Netlist &nl, std::uint16_t slot, GateKind kind) noexcept {
  const std::uint8_t ord = kind_ordinal(kind);
  Gate &g = nl.gates[slot];
  g.kind = kind;
  g.counter = nl.next_counter[ord]++;
  if (nl.next_counter[ord] == 0) { // u32 wrap: 0 is never assigned
    nl.next_counter[ord] = 1;
  }
}

// Classify every live gate output in topo order. Ignored dangling inputs
// read as C0 (they tie low in eval); wired inputs inherit the driver's
// label; dangling block inputs are V.
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

// Delete every live gate that cannot reach a root sink. Roots are the
// original sinks (see header note): optimization orphans vanish, user
// logic (including ignored-output cones) is never deleted.
inline bool dce_pass(Netlist &nl, const bool *is_root) noexcept {
  bool keep[kMaxGates] = {};
  std::uint16_t stack[kMaxGates];
  std::size_t top = 0;
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    if (is_root[s] && nl.gates[s].live) {
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
  }
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

// Rewire every input currently driven by `from_slot` to `target`. When the
// target dangles, the new block input must be visible: clear its ignored
// bit (the mark was inert while wired; keeping it would silently tie the
// pin low instead of exposing the real dependency).
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

// True when any live sequential gate reads gate `s`. Prefer the
// precomputed feeds_seq bit (see optimize_barrier.hpp) inside passes;
// this helper stays for single-shot queries.
inline bool has_live_seq_user(const Netlist &nl, std::uint16_t s) noexcept {
  bool feeds[kMaxGates] = {};
  compute_feeds_seq(nl, feeds);
  return s < kMaxGates && feeds[s];
}

// BUF(x) -> x and NOT(NOT(x)) -> x for internal gates (fanout > 0; block
// outputs are preserved for interface stability). Single ascending sweep
// collapses chains regardless of slot order. Gates feeding a sequential
// gate are never removed (opaque barrier: flop/latch input pins freeze).
// feeds_seq is the precomputed barrier bit (nullptr == no sequential gates,
// skip the check).
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

// Drop every input of gate `s` whose class matches (drop_any drops all C0/C1
// inputs; otherwise only drop_c). Odd C1 count flips XOR<->XNOR when asked.
// Single remainders become BUF (AND/OR/XOR) or NOT (NAND/NOR/XNOR). An
// all-constant gate is left alone (label-only; the caller collapses roots
// and DCE removes the rest). Remaining inputs are all varying, so the gate
// relabels to V. Tied pins are always varying (dangling, non-ignored), so
// dropped pins are untied — but kept tied pins shift indices, so their
// classes re-elect around the new flats (outside members follow).
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
  const std::uint8_t old_n = g.input_count;
  for (std::uint8_t k = 0; k < n; ++k) {
    const std::size_t of = tie_flat(s, kept_orig[k]);
    orig_leader[k] = tie_leader_flat(nl, of);
    orig_tied[k] = (nl.tie_rep[of] != 0);
  }
  (void)old_n;
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
  // Rebuild ties for kept pins (indices may have shifted): clear this
  // gate's tie entries, then re-star each original class around its new
  // minimum (kept new flats + outside survivors still pointing at the old
  // leader). Singletons stay singleton.
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
    if (n_surv > 1) {
      std::size_t nl_leader = survivors[0];
      for (std::size_t q = 1; q < n_surv; ++q) {
        if (survivors[q] < nl_leader) {
          nl_leader = survivors[q];
        }
      }
      for (std::size_t q = 0; q < n_surv; ++q) {
        nl.tie_rep[survivors[q]] = static_cast<std::uint16_t>(nl_leader + 1u);
      }
    }
    // n_surv == 1 stays singleton (already 0).
  }
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

// Constant folding + annihilator/identity sweep (forward topo order, labels
// updated in place so one sweep propagates whole chains). Assumes a fresh
// topology on entry; leaves dirty when structural.
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
    if (label[s] == Constness::kC0) {
      const bool canonical = g.kind == GateKind::kAnd && g.input_count == 2 && g.ignored_mask == 0x03u &&
                             g.drivers[0] == kNoDriver && g.drivers[1] == kNoDriver;
      if (!canonical) {
        detail::detach_slot_ties(nl, s);
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
        detail::detach_slot_ties(nl, s);
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

// De Morgan bubble-push (O1): fuse every NOT driven by a live gate into the
// complementary kind in place (same slot, fresh counter). NOT(BUF) rewires
// through; output double-negations are preserved (interface stability).
// feeds_seq is the precomputed barrier bit (nullptr == no sequential gates).
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
        const std::size_t src_flat = tie_flat(h, i);
        if (nl.tie_rep[src_flat] == 0) {
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
