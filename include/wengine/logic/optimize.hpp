#pragma once

// Combinational optimizer (LOGIC-4): pure `optimize(netlist, opts)` sells
// the simplification story in three explicit levels. The input is never
// touched (const ref in, new Netlist out); overflow past kMaxGates is a
// loud Result failure, never a silent drop.
//
// Levels (docs/logic.md section 6):
//   O0: constant folding / annihilator + identity with tied inputs,
//       double-negation + BUF bypass, dead-gate elimination. Never grows
//       the net (gates_after <= gates_before).
//   O1: O0 + De Morgan bubble-push canonical form: every NOT driven by an
//       AND/OR/NAND/NOR/XOR/XNOR is fused into the complementary kind in
//       place (NOT(AND) -> NAND, NOT(OR) -> NOR, ...). Still mixed gates.
//       Invariant afterwards: no live NOT is driven by a fusable kind.
//       Output double-negations (NOT driven by NOT at a block output) are
//       deliberately preserved: deleting the outer gate would move the
//       output to another slot, and interface stability wins over fusion.
//   O2: O1 + full conversion to NAND-only (--to nand) or NOR-only
//       (--to nor) via De Morgan + duality. Gate count may grow; the
//       caller reports before/after counts so the trade is visible.
//       Built fresh (slot-order creation, deferred wiring) so block
//       input/output order is preserved positionally.
//
// Determinism / interface rules (all levels):
// - Slot-order processing everywhere; new gates take the lowest free
//   slot (O0/O1) or compact slot order (O2); per-kind counters are never
//   reused (in-place kind changes take a fresh counter, so display names
//   stay unique for resolve_gate).
// - Block outputs keep their slots (O0/O1 convert in place) or their
//   relative order (O2 emits output networks in original slot order), so
//   truth-equivalence checks can map outputs positionally.
// - DCE roots are the ORIGINAL sinks (live fanout-0 gates captured before
//   any pass, ignored outputs included). Optimization orphans (bypassed
//   inners, fused-away drivers, dropped constant gates) are removed, but
//   user logic feeding an ignored output is never deleted: ignore means
//   "hidden", not "dead", and the optimizer must not destroy circuitry
//   the user only hid.
// - Ignored dangling inputs tie to kLow during eval (netlist.hpp rule).
//   O0 reasons about them as constant-0; O2 materializes a shared
//   target-kind constant pool for them. O2 resets propagation_delay_ps
//   (the network is reshaped; O0/O1 preserve delays in place).
//
// Header-only, allocation-free, exception-free like the rest of
// wengine/logic. No iostream (report printing belongs to the CLI verb in
// LOGIC-8; the engine exposes before/after counts via live_count).

#include "wengine/core/status.hpp"
#include "wengine/logic/eval.hpp"
#include "wengine/logic/netlist.hpp"
#include "wengine/logic/types.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace wengine::logic {

enum class OptLevel : std::uint8_t {
  kO0 = 0,
  kO1 = 1,
  kO2 = 2,
};

enum class OptTarget : std::uint8_t {
  kNand = 0,
  kNor = 1,
};

struct OptimizeOptions {
  OptLevel level = OptLevel::kO0;
  OptTarget target = OptTarget::kNand; // read only at O2
};

// Case-insensitive "O0|O1|O2" (exactly two characters).
[[nodiscard]] inline core::Result<OptLevel> parse_opt_level(std::string_view text) noexcept {
  if (text.size() == 2 && (text[0] == 'O' || text[0] == 'o') && text[1] >= '0' && text[1] <= '2') {
    const auto v = static_cast<std::uint8_t>(text[1] - '0');
    return core::Result<OptLevel>::success(static_cast<OptLevel>(v));
  }
  return core::Result<OptLevel>::failure("bad opt level: want O0|O1|O2");
}

// Case-insensitive "nand"|"nor".
[[nodiscard]] inline core::Result<OptTarget> parse_opt_target(std::string_view text) noexcept {
  if (text.size() == 4) {
    const bool nand = (text[0] == 'N' || text[0] == 'n') && (text[1] == 'A' || text[1] == 'a') &&
                      (text[2] == 'N' || text[2] == 'n') && (text[3] == 'D' || text[3] == 'd');
    if (nand) {
      return core::Result<OptTarget>::success(OptTarget::kNand);
    }
  } else if (text.size() == 3) {
    const bool nor = (text[0] == 'N' || text[0] == 'n') && (text[1] == 'O' || text[1] == 'o') && (text[2] == 'R' || text[2] == 'r');
    if (nor) {
      return core::Result<OptTarget>::success(OptTarget::kNor);
    }
  }
  return core::Result<OptTarget>::failure("bad opt target: want nand|nor");
}

namespace detail {

enum class Constness : std::uint8_t {
  kC0 = 0,
  kC1 = 1,
  kV = 2, // varies with block inputs
};

// Recompute output_fanout from scratch (deterministic; avoids drift after
// direct driver surgery).
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
    case GateKind::kDff:
    case GateKind::kDlatch:
      break; // SEQ11 owns optimizer preservation; classify stays kV until then.
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

// BUF(x) -> x and NOT(NOT(x)) -> x for internal gates (fanout > 0; block
// outputs are preserved for interface stability). Single ascending sweep
// collapses chains regardless of slot order.
inline bool bypass_pass(Netlist &nl) noexcept {
  recount_fanout(nl);
  bool changed = false;
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    Gate &g = nl.gates[s];
    if (!g.live || g.output_fanout == 0) {
      continue;
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
    case GateKind::kNot:
    case GateKind::kBuf:
      want = base;
      break;
    case GateKind::kDff:
    case GateKind::kDlatch:
      want = base; // SEQ11 owns optimizer preservation; no reduction until then.
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
    case GateKind::kNot:
    case GateKind::kBuf:
      break; // label-only; users/DCE/collapse handle the rest
    case GateKind::kDff:
    case GateKind::kDlatch:
      break; // SEQ11 owns optimizer preservation; no folding until then.
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
inline bool fusion_sweep(Netlist &nl) noexcept {
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
      case GateKind::kNot:
      case GateKind::kBuf:
        break; // handled above; unreachable here
      case GateKind::kDff:
      case GateKind::kDlatch:
        break; // SEQ11 owns optimizer preservation; no bubble-push until then.
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

// --- O2 fresh-build conversion -------------------------------------------
// Deferred wiring: creation runs in original slot order (fresh slots follow
// it, so block input/output order is preserved positionally); wiring
// applies afterwards from the complete driver map.
struct PendingWire {
  std::uint16_t fresh = kNoDriver;
  std::uint16_t orig = kNoDriver; // original driver slot (kNoDriver = dangling)
  std::uint8_t input = 0;
  bool use_pool = false; // ignored original input: wire the shared C0 pool gate
};

struct ConvertCtx {
  Netlist *out = nullptr;
  OptTarget target = OptTarget::kNand;
  std::uint16_t *emit_map = nullptr; // orig slot -> network output fresh slot
  PendingWire *pend = nullptr;
  std::size_t *npend = nullptr;
  std::size_t pend_cap = 0;
  std::uint16_t pool_c0 = kNoDriver;
  bool failed = false;
};

inline std::uint16_t fresh_gate(ConvertCtx &ctx, GateKind kind, std::uint8_t n) noexcept {
  if (ctx.failed) {
    return kNoDriver;
  }
  auto r = add_gate(*ctx.out, kind, n);
  if (!r.ok()) {
    ctx.failed = true;
    return kNoDriver;
  }
  return r.value;
}

// Record one network input: original input `orig_i` of `orig_s` feeding
// `fresh` at `input_idx`. Ignored pins route to the shared C0 pool;
// dangling block inputs pass through as dangling (mask stays clear).
inline void record_input(ConvertCtx &ctx, const Netlist &src, std::uint16_t orig_s, std::uint8_t orig_i,
                         std::uint16_t fresh, std::uint8_t input_idx) noexcept {
  if (ctx.failed) {
    return;
  }
  if (*ctx.npend >= ctx.pend_cap) {
    ctx.failed = true; // defensive: bounded by reachable*8 by construction
    return;
  }
  const Gate &g = src.gates[orig_s];
  PendingWire w{};
  w.fresh = fresh;
  w.input = input_idx;
  if ((g.ignored_mask & static_cast<std::uint8_t>(1u << orig_i)) != 0) {
    w.use_pool = true;
    w.orig = ctx.pool_c0;
  } else {
    w.use_pool = false;
    w.orig = g.drivers[orig_i];
  }
  ctx.pend[*ctx.npend] = w;
  ++(*ctx.npend);
}

inline GateKind universal_kind(OptTarget t) noexcept {
  return (t == OptTarget::kNand) ? GateKind::kNand : GateKind::kNor;
}

// NOT(a) in the target vocabulary: one universal gate over (a, a).
inline std::uint16_t emit_not(ConvertCtx &ctx, const Netlist &src, std::uint16_t orig_s, std::uint8_t orig_i) noexcept {
  const GateKind u = universal_kind(ctx.target);
  const std::uint16_t t = fresh_gate(ctx, u, 2);
  if (t == kNoDriver) {
    return kNoDriver;
  }
  record_input(ctx, src, orig_s, orig_i, t, 0);
  // Both inputs read the same source: record twice (duplicate drivers are
  // legal; fanout counts each edge, block-output detection only needs != 0).
  record_input(ctx, src, orig_s, orig_i, t, 1);
  return t;
}

// NOT over an already-emitted fresh driver (internal edge, wired at once).
inline std::uint16_t emit_not_fresh(ConvertCtx &ctx, std::uint16_t a) noexcept {
  const GateKind u = universal_kind(ctx.target);
  const std::uint16_t o = fresh_gate(ctx, u, 2);
  if (o == kNoDriver) {
    return kNoDriver;
  }
  ctx.out->gates[o].drivers[0] = a;
  ctx.out->gates[o].drivers[1] = a;
  return o;
}

// Convert one reachable source gate into fresh target-kind gates. Returns
// the network output fresh slot (kNoDriver on budget exhaustion).
inline std::uint16_t emit_gate(ConvertCtx &ctx, const Netlist &src, std::uint16_t s) noexcept {
  const Gate &g = src.gates[s];
  const GateKind u = universal_kind(ctx.target);
  const std::uint8_t n = g.input_count;
  const bool is_nand_target = (ctx.target == OptTarget::kNand);
  auto single = [&]() noexcept -> std::uint16_t {
    const std::uint16_t o = fresh_gate(ctx, u, n);
    if (o == kNoDriver) {
      return kNoDriver;
    }
    for (std::uint8_t i = 0; i < n; ++i) {
      record_input(ctx, src, s, i, o, i);
    }
    return o;
  };
  switch (g.kind) {
  case GateKind::kNot: {
    // NOT(a) = U(a, a) regardless of target.
    const std::uint16_t o = fresh_gate(ctx, u, 2);
    if (o == kNoDriver) {
      return kNoDriver;
    }
    record_input(ctx, src, s, 0, o, 0);
    record_input(ctx, src, s, 0, o, 1);
    return o;
  }
  case GateKind::kBuf: {
    // BUF(a) = NOT(NOT(a)).
    const std::uint16_t t = emit_not(ctx, src, s, 0);
    if (t == kNoDriver) {
      return kNoDriver;
    }
    return emit_not_fresh(ctx, t);
  }
  case GateKind::kAnd:
  case GateKind::kOr:
  case GateKind::kNand:
  case GateKind::kNor: {
    // Native singles: the kind already is the universal gate.
    if ((g.kind == GateKind::kNand && is_nand_target) || (g.kind == GateKind::kNor && !is_nand_target)) {
      return single();
    }
    // Same-shape + NOT: AND under NAND
    // (AND = NOT(NAND)) and OR under NOR (OR = NOT(NOR)). Dual shape via
    // De Morgan: OR under NAND is NAND(~a..), AND under NOR is NOR(~a..);
    // the complemented kinds (NOR under NAND, NAND under NOR) add one
    // output inverter.
    const bool single_then_not = (g.kind == GateKind::kAnd && is_nand_target) || (g.kind == GateKind::kOr && !is_nand_target);
    if (single_then_not) {
      const std::uint16_t inner = single();
      if (inner == kNoDriver) {
        return kNoDriver;
      }
      return emit_not_fresh(ctx, inner);
    }
    const bool complemented = (g.kind == GateKind::kNor && is_nand_target) || (g.kind == GateKind::kNand && !is_nand_target);
    {
      const std::uint16_t mid = fresh_gate(ctx, u, n);
      if (mid == kNoDriver) {
        return kNoDriver;
      }
      for (std::uint8_t i = 0; i < n; ++i) {
        const std::uint16_t v = emit_not(ctx, src, s, i);
        if (v == kNoDriver) {
          return kNoDriver;
        }
        ctx.out->gates[mid].drivers[i] = v;
      }
      if (!complemented) {
        return mid;
      }
      const std::uint16_t o = fresh_gate(ctx, u, 2);
      if (o == kNoDriver) {
        return kNoDriver;
      }
      ctx.out->gates[o].drivers[0] = mid;
      ctx.out->gates[o].drivers[1] = mid;
      return o;
    }
  }
  case GateKind::kXor:
  case GateKind::kXnor: {
    // Odd-parity cascade of 2-input XOR blocks, then NOT for XNOR.
    // NAND block (4 gates): t1=N(a,b); t2=N(a,t1); t3=N(b,t1); o=N(t2,t3).
    // NOR block (5 gates): t1=NOR(a,b); na=~a; nb=~b; t4=NOR(na,nb)=a&b;
    // o=NOR(t1,t4) = (a|b)&~(a&b). Each block input is either an existing
    // fresh driver (wired at once) or an original source input (recorded
    // for the deferred wiring pass).
    struct Feed {
      bool is_fresh = false;
      std::uint16_t fresh_slot = kNoDriver;
      std::uint8_t orig_idx = 0;
    };
    auto drive = [&](std::uint16_t gate, std::uint8_t idx, const Feed &f) noexcept {
      if (f.is_fresh) {
        ctx.out->gates[gate].drivers[idx] = f.fresh_slot;
      } else {
        record_input(ctx, src, s, f.orig_idx, gate, idx);
      }
    };
    auto xor2 = [&](const Feed &a, const Feed &b) noexcept -> std::uint16_t {
      if (is_nand_target) {
        const std::uint16_t t1 = fresh_gate(ctx, u, 2);
        const std::uint16_t t2 = fresh_gate(ctx, u, 2);
        const std::uint16_t t3 = fresh_gate(ctx, u, 2);
        const std::uint16_t o = fresh_gate(ctx, u, 2);
        if (t1 == kNoDriver || t2 == kNoDriver || t3 == kNoDriver || o == kNoDriver) {
          return kNoDriver;
        }
        drive(t1, 0, a);
        drive(t1, 1, b);
        drive(t2, 0, a);
        ctx.out->gates[t2].drivers[1] = t1;
        drive(t3, 0, b);
        ctx.out->gates[t3].drivers[1] = t1;
        ctx.out->gates[o].drivers[0] = t2;
        ctx.out->gates[o].drivers[1] = t3;
        return o;
      }
      const std::uint16_t t1 = fresh_gate(ctx, u, 2);
      const std::uint16_t na = fresh_gate(ctx, u, 2);
      const std::uint16_t nb = fresh_gate(ctx, u, 2);
      const std::uint16_t t4 = fresh_gate(ctx, u, 2);
      const std::uint16_t o = fresh_gate(ctx, u, 2);
      if (t1 == kNoDriver || na == kNoDriver || nb == kNoDriver || t4 == kNoDriver || o == kNoDriver) {
        return kNoDriver;
      }
      drive(t1, 0, a);
      drive(t1, 1, b);
      drive(na, 0, a);
      drive(na, 1, a);
      drive(nb, 0, b);
      drive(nb, 1, b);
      ctx.out->gates[t4].drivers[0] = na;
      ctx.out->gates[t4].drivers[1] = nb;
      ctx.out->gates[o].drivers[0] = t1;
      ctx.out->gates[o].drivers[1] = t4;
      return o;
    };
    const Feed r0{false, kNoDriver, 0};
    const Feed r1{false, kNoDriver, 1};
    std::uint16_t acc = xor2(r0, r1);
    if (acc == kNoDriver || ctx.failed) {
      return kNoDriver;
    }
    for (std::uint8_t k = 2; k < n; ++k) {
      const Feed fa{true, acc, 0};
      const Feed fb{false, kNoDriver, k};
      acc = xor2(fa, fb);
      if (acc == kNoDriver || ctx.failed) {
        return kNoDriver;
      }
    }
    if (g.kind == GateKind::kXor) {
      return acc;
    }
    return emit_not_fresh(ctx, acc);
  }
  case GateKind::kDff:
  case GateKind::kDlatch:
    break; // SEQ11 owns optimizer preservation; O2 conversion refuses loudly.
  }
  return kNoDriver; // unreachable (all kinds handled); defensive
}

} // namespace detail

// Pure optimizer: `in` is never modified (all surgery happens on the copy).
[[nodiscard]] inline core::Result<Netlist> optimize(const Netlist &in, OptimizeOptions opts) noexcept {
  Netlist nl = in;
  nl.dirty = true;
  nl.topo_size = 0;
  detail::recount_fanout(nl);

  bool roots[kMaxGates] = {};
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    if (nl.gates[s].live && nl.gates[s].output_fanout == 0) {
      roots[s] = true;
    }
  }

  const bool want_fuse = (opts.level == OptLevel::kO1) || (opts.level == OptLevel::kO2);
  constexpr std::size_t kMaxIters = 2 * kMaxGates + 16;
  std::size_t iter = 0;
  while (true) {
    if (iter >= kMaxIters) {
      return core::Result<Netlist>::failure("optimize: internal iteration bound", core::StatusCode::kInternal);
    }
    ++iter;
    if (!ensure_topology(nl).ok()) {
      return core::Result<Netlist>::failure("optimize: topology error", core::StatusCode::kInternal);
    }
    bool changed = false;
    if (detail::bypass_pass(nl)) {
      changed = true;
    }
    if (!ensure_topology(nl).ok()) {
      return core::Result<Netlist>::failure("optimize: topology error", core::StatusCode::kInternal);
    }
    if (want_fuse && detail::fusion_sweep(nl)) {
      changed = true;
    }
    if (!ensure_topology(nl).ok()) {
      return core::Result<Netlist>::failure("optimize: topology error", core::StatusCode::kInternal);
    }
    if (detail::const_fold_pass(nl, roots)) {
      changed = true;
    }
    if (!ensure_topology(nl).ok()) {
      return core::Result<Netlist>::failure("optimize: topology error", core::StatusCode::kInternal);
    }
    if (detail::dce_pass(nl, roots)) {
      changed = true;
    }
    if (!changed) {
      break;
    }
  }

  if (opts.level != OptLevel::kO2) {
    if (!ensure_topology(nl).ok()) {
      return core::Result<Netlist>::failure("optimize: topology error", core::StatusCode::kInternal);
    }
    return core::Result<Netlist>::success(nl);
  }

  // --- O2: fresh universal build -----------------------------------------
  // Reachable set (reverse DFS from every current sink, ignored or not).
  bool reach[kMaxGates] = {};
  {
    std::uint16_t stack[kMaxGates];
    std::size_t top = 0;
    for (std::uint16_t s = 0; s < kMaxGates; ++s) {
      if (nl.gates[s].live && nl.gates[s].output_fanout == 0) {
        reach[s] = true;
        stack[top++] = s;
      }
    }
    while (top > 0) {
      const std::uint16_t cur = stack[--top];
      const Gate &g = nl.gates[cur];
      for (std::uint8_t i = 0; i < g.input_count; ++i) {
        const std::uint16_t d = g.drivers[i];
        if (d != kNoDriver && d < kMaxGates && nl.gates[d].live && !reach[d]) {
          reach[d] = true;
          stack[top++] = d;
        }
      }
    }
  }

  Netlist out{};
  std::uint16_t emit_map[kMaxGates];
  for (std::size_t s = 0; s < kMaxGates; ++s) {
    emit_map[s] = kNoDriver;
  }
  detail::PendingWire pend[kMaxGates * kMaxInputs];
  std::size_t npend = 0;
  detail::ConvertCtx ctx{};
  ctx.out = &out;
  ctx.target = opts.target;
  ctx.emit_map = emit_map;
  ctx.pend = pend;
  ctx.npend = &npend;
  ctx.pend_cap = kMaxGates * kMaxInputs;

  // Shared C0 pool (only when some reachable gate has an ignored input).
  bool need_pool = false;
  for (std::uint16_t s = 0; s < kMaxGates && !need_pool; ++s) {
    if (!nl.gates[s].live || !reach[s]) {
      continue;
    }
    if (nl.gates[s].ignored_mask != 0) {
      need_pool = true;
    }
  }
  if (need_pool) {
    const GateKind u = detail::universal_kind(opts.target);
    auto rc1 = add_gate(out, u, 2);
    if (!rc1.ok()) {
      return core::Result<Netlist>::failure("optimize: gate budget exceeded (kMaxGates)", core::StatusCode::kInternal);
    }
    Gate &c1 = out.gates[rc1.value];
    c1.ignored_mask = 0x03u; // U(0,0) = 1 in both vocabularies
    auto rc0 = add_gate(out, u, 2);
    if (!rc0.ok()) {
      return core::Result<Netlist>::failure("optimize: gate budget exceeded (kMaxGates)", core::StatusCode::kInternal);
    }
    out.gates[rc0.value].drivers[0] = rc1.value;
    out.gates[rc0.value].drivers[1] = rc1.value;
    ctx.pool_c0 = rc0.value;
  }

  // Creation pass in original slot order (fresh slots follow it).
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    if (!nl.gates[s].live || !reach[s]) {
      continue;
    }
    const std::uint16_t o = detail::emit_gate(ctx, nl, s);
    if (o == kNoDriver || ctx.failed) {
      return core::Result<Netlist>::failure("optimize: gate budget exceeded (kMaxGates)", core::StatusCode::kInternal);
    }
    emit_map[s] = o;
  }

  // Wiring pass from the complete map.
  for (std::size_t p = 0; p < npend; ++p) {
    const detail::PendingWire &w = pend[p];
    std::uint16_t drv = kNoDriver;
    if (w.use_pool) {
      drv = ctx.pool_c0;
    } else if (w.orig != kNoDriver) {
      if (w.orig >= kMaxGates || emit_map[w.orig] == kNoDriver) {
        return core::Result<Netlist>::failure("optimize: internal wiring error", core::StatusCode::kInternal);
      }
      drv = emit_map[w.orig];
    }
    if (w.fresh >= kMaxGates || w.input >= kMaxInputs) {
      return core::Result<Netlist>::failure("optimize: internal wiring error", core::StatusCode::kInternal);
    }
    out.gates[w.fresh].drivers[w.input] = drv;
  }
  // Restore ignored marks on fresh dangling pins that mirror ignored
  // original pins (they stay excluded from the block interface).
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    if (!nl.gates[s].live || !reach[s]) {
      continue;
    }
    // Only single-gate networks mirror pin positions 1:1; multi-gate
    // networks route ignored originals through the pool (wired, no mark).
    const Gate &g = nl.gates[s];
    const std::uint16_t o = emit_map[s];
    const bool single_gate_network = (g.kind == GateKind::kNand && opts.target == OptTarget::kNand) ||
        (g.kind == GateKind::kNor && opts.target == OptTarget::kNor);
    if (single_gate_network) {
      out.gates[o].ignored_mask = g.ignored_mask;
    }
  }
  // Preserve ignored-output flags on the fresh output gates.
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    if (!nl.gates[s].live || !reach[s]) {
      continue;
    }
    if (nl.gates[s].ignore_output) {
      out.gates[emit_map[s]].ignore_output = true;
    }
  }
  detail::recount_fanout(out);
  out.dirty = true;
  out.topo_size = 0;
  if (!ensure_topology(out).ok()) {
    return core::Result<Netlist>::failure("optimize: topology error", core::StatusCode::kInternal);
  }
  return core::Result<Netlist>::success(out);
}

} // namespace wengine::logic
