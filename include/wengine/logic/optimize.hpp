#pragma once

// Combinational optimizer (LOGIC-4): pure `optimize(netlist, opts)` sells
// the simplification story in three explicit levels. The input is never
// touched (const ref in, new Netlist out); overflow past kMaxGates is a
// loud Result failure, never a silent drop.
//
// Levels (docs/logic.md section 6):
//   O0: constant folding / annihilator + identity with tied inputs,
//       double-negation + BUF bypass, dead-gate elimination + exact
//       truth-table simplification for small blocks (n<=10,m<=16: semantic
//       constants like x&~x==0 + dead inputs). Never grows the net
//       (gates_after <= gates_before).
//   O1: O0 + De Morgan bubble-push canonical form: every NOT driven by an
//       AND/OR/NAND/NOR/XOR/XNOR is fused into the complementary kind in
//       place (NOT(AND) -> NAND, NOT(OR) -> NOR, ...). Still mixed gates.
//       Invariant afterwards: no live NOT is driven by a fusable kind.
//       Output double-negations (NOT driven by NOT at a block output) are
//       deliberately preserved: deleting the outer gate would move the
//       output to another slot, and interface stability wins over fusion.
//   O2: O1 + full conversion to NAND-only (--to nand) or NOR-only
//       (--to nor). Small blocks (n<=8,m<=8) try minimized two-level
//       resynthesis (QM + greedy, SOP/POS, shared inverters, tied inputs)
//       and pick smaller vs structural De Morgan (which is optimal for
//       XOR-like parity); large blocks use structural + vocabulary-preserving
//       cleanup (dedup + double-inverter cancellation + DCE). Constants reuse
//       a shared pool (1-2 gates, not 4+). Gate count may still grow for
//       large nets; the caller reports before/after counts so the trade is
//       visible. Minimized may permute inputs (tied, one per variable);
//       structural preserves positional order (duplicating pins for inversion).
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

// Forward declarations for exact pass (defined below).
inline void recount_fanout(Netlist &nl) noexcept;
inline void detach_slot_ties(Netlist &nl, std::uint16_t s) noexcept;
inline void retag(Netlist &nl, std::uint16_t slot, GateKind kind) noexcept;

// Exact truth-table simplification (semantic, not just structural):
// for small blocks (n<=10, m<=16, no hidden outputs) detect constant
// outputs (contradictions like x & ~x == 0 that structural folding
// misses) and dead inputs (inputs that never affect any output), then
// collapse constants to canonical form and tie dead inputs off to 0
// (preserving function, since dead). Runs at all levels (O0/O1/O2
// pre-pass); never grows the net. Returns true when changed.
// Deterministic (slot order everywhere), allocation-free (fixed stack
// arrays), noexcept.
inline bool exact_simplify_pass(Netlist &nl) noexcept {
  const std::size_t n = block_input_count(nl);
  const std::size_t m = block_output_count(nl);
  if (n == 0 || n > 10 || m == 0 || m > 16) {
    return false;
  }
  // Hidden outputs must be preserved exactly: if the user hid logic via
  // `ignore`, dead-input analysis over block outputs alone could break
  // the hidden cone (an input dead for visible outputs may be live for
  // hidden ones). Skip exact when any hidden sink exists (conservative;
  // structural passes already preserve hidden cones).
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    if (nl.gates[s].live && nl.gates[s].ignore_output) {
      return false;
    }
  }
  BlockPin in_buf[10];
  BlockPin out_buf[16];
  std::size_t in_n = 0;
  std::size_t out_n = 0;
  if (!block_interface(nl, in_buf, 10, &in_n, out_buf, 16, &out_n).ok()) {
    return false;
  }
  if (in_n != n || out_n != m) {
    return false;
  }
  std::uint16_t out_slots[16];
  for (std::size_t o = 0; o < m; ++o) {
    out_slots[o] = out_buf[o].slot;
  }
  if (!ensure_topology(nl).ok()) {
    return false;
  }
  const std::size_t rows = static_cast<std::size_t>(1ULL << n);
  // Truth table: tt[r*m+o] = 0/1. 1024*16 = 16KB stack, allocation-free.
  // Only rows*m entries are used.
  std::uint8_t tt[1024 * 16];
  LogicValue in_vals[10];
  LogicValue out_vals[16];
  for (std::size_t r = 0; r < rows; ++r) {
    for (std::size_t j = 0; j < n; ++j) {
      in_vals[j] = (((r >> j) & 1ULL) != 0ULL) ? LogicValue::kHigh : LogicValue::kLow;
    }
    for (std::size_t o = 0; o < m; ++o) {
      out_vals[o] = LogicValue::kLow;
    }
    if (!solve(nl, std::span<const LogicValue>(in_vals, n), std::span<LogicValue>(out_vals, m)).ok()) {
      return false;
    }
    for (std::size_t o = 0; o < m; ++o) {
      tt[r * m + o] = (out_vals[o] == LogicValue::kHigh) ? 1u : 0u;
    }
  }
  bool is_const[16] = {};
  std::uint8_t const_val[16] = {};
  for (std::size_t o = 0; o < m; ++o) {
    const std::uint8_t first = tt[0 * m + o];
    bool same = true;
    for (std::size_t r = 1; r < rows; ++r) {
      if (tt[r * m + o] != first) {
        same = false;
        break;
      }
    }
    is_const[o] = same;
    const_val[o] = first;
  }
  bool is_dead[10] = {};
  for (std::size_t j = 0; j < n; ++j) {
    bool live = false;
    for (std::size_t r = 0; r < rows; ++r) {
      if (((r >> j) & 1ULL) != 0ULL) {
        continue; // check each pair once (bit 0 -> 1)
      }
      const std::size_t r2 = r | (static_cast<std::size_t>(1ULL << j));
      for (std::size_t o = 0; o < m; ++o) {
        if (tt[r * m + o] != tt[r2 * m + o]) {
          live = true;
          break;
        }
      }
      if (live) {
        break;
      }
    }
    is_dead[j] = !live;
  }
  bool any_const = false;
  for (std::size_t o = 0; o < m; ++o) {
    if (is_const[o]) {
      any_const = true;
      break;
    }
  }
  bool any_dead = false;
  for (std::size_t j = 0; j < n; ++j) {
    if (is_dead[j]) {
      any_dead = true;
      break;
    }
  }
  if (!any_const && !any_dead) {
    return false;
  }
  bool changed = false;
  // Dead inputs first (while tie classes are intact): dissolve each dead
  // class and tie its pins off to 0 (ignored). Preserves function since
  // dead inputs never affect any output.
  for (std::size_t j = 0; j < n; ++j) {
    if (!is_dead[j]) {
      continue;
    }
    const std::size_t leader = tie_flat(in_buf[j].slot, in_buf[j].input);
    // in_buf holds leaders, so leader should equal its own flat; if not
    // (raced by an earlier dead class sharing? classes are disjoint, so
    // no), skip defensively.
    if (tie_leader_flat(nl, leader) != leader) {
      continue;
    }
    for (std::uint16_t s = 0; s < kMaxGates; ++s) {
      Gate &g = nl.gates[s];
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
        const std::size_t flat = tie_flat(s, i);
        if (tie_leader_flat(nl, flat) != leader) {
          continue;
        }
        nl.tie_rep[flat] = 0;
        g.ignored_mask = static_cast<std::uint8_t>(g.ignored_mask | static_cast<std::uint8_t>(1u << i));
        changed = true;
      }
    }
  }
  // Constant outputs: collapse to canonical form (same as structural
  // const-fold roots: C0 = AND(0,0), C1 = NAND(0,0)).
  for (std::size_t o = 0; o < m; ++o) {
    if (!is_const[o]) {
      continue;
    }
    const std::uint16_t s = out_slots[o];
    Gate &g = nl.gates[s];
    if (!g.live) {
      continue;
    }
    if (const_val[o] == 0) {
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
    } else {
      const bool canonical = g.kind == GateKind::kNand && g.input_count == 2 && g.ignored_mask == 0x03u &&
                             g.drivers[0] == kNoDriver && g.drivers[1] == kNoDriver;
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

// --- Quine-McCluskey minimization for O2 (small blocks, n<=8) ----------
// Exact two-level minimization (SOP for NAND, POS via complement for NOR)
// with greedy set cover. Used to resynthesize small blocks from their truth
// table, so repeated O2 toggles (NAND<->NOR) stay near-optimal instead of
// 4x per toggle. Deterministic (sorted minterms, ordered combining, first-
// max greedy), allocation-free (fixed 1024-cube arrays), noexcept. Fails
// loudly (returns false) when the cover would need splitting (>8 products
// or >8 sums) or exceed fixed caps — the caller then falls back to the
// structural De Morgan translation (which is optimal for XOR-like parity).
struct QmCube {
  std::uint16_t mask = 0; // 1 = don't care
  std::uint16_t bits = 0; // values for cared bits (don't-care bits cleared)
};

inline std::uint8_t qm_popcount(std::uint16_t x) noexcept {
  std::uint8_t c = 0;
  while (x != 0) {
    c = static_cast<std::uint8_t>(c + static_cast<std::uint8_t>(x & 1u));
    x = static_cast<std::uint16_t>(x >> 1);
  }
  return c;
}

inline bool qm_is_pow2(std::uint16_t x) noexcept {
  return x != 0 && ((x & static_cast<std::uint16_t>(x - 1u)) == 0u);
}

// Collect prime implicants for `minterms[0..nm)` (sorted ascending, distinct)
// over `n` vars (n<=8). Primes in deterministic creation order. Returns false
// when fixed caps would overflow (caller falls back to structural).
inline bool qm_primes(const std::uint16_t *minterms, std::size_t nm, std::uint8_t n,
                      QmCube *primes, std::size_t *nprimes) noexcept {
  if (nm == 0 || n == 0 || n > 8) {
    return false;
  }
  QmCube cur[1024];
  QmCube nxt[1024];
  bool used[1024] = {};
  if (nm > 1024) {
    return false;
  }
  for (std::size_t i = 0; i < nm; ++i) {
    cur[i].mask = 0;
    cur[i].bits = minterms[i];
    used[i] = false;
  }
  std::size_t ncur = nm;
  std::size_t np = 0;
  for (std::size_t level = 0; level < 9; ++level) {
    for (std::size_t i = 0; i < ncur; ++i) {
      used[i] = false;
    }
    std::size_t nnxt = 0;
    for (std::size_t i = 0; i < ncur; ++i) {
      for (std::size_t j = i + 1; j < ncur; ++j) {
        if (cur[i].mask != cur[j].mask) {
          continue;
        }
        const std::uint16_t diff =
            static_cast<std::uint16_t>(cur[i].bits ^ cur[j].bits);
        if (!qm_is_pow2(diff)) {
          continue;
        }
        if ((diff & cur[i].mask) != 0u) {
          continue; // differing bit already don't-care (shouldn't happen w/ canonical)
        }
        QmCube ncube{};
        ncube.mask = static_cast<std::uint16_t>(cur[i].mask | diff);
        ncube.bits = static_cast<std::uint16_t>(cur[i].bits & static_cast<std::uint16_t>(~diff));
        // Deduplicate (preserve first-occurrence order).
        bool have = false;
        for (std::size_t k = 0; k < nnxt; ++k) {
          if (nxt[k].mask == ncube.mask && nxt[k].bits == ncube.bits) {
            have = true;
            break;
          }
        }
        if (!have) {
          if (nnxt >= 1024) {
            return false;
          }
          nxt[nnxt++] = ncube;
        }
        used[i] = true;
        used[j] = true;
      }
    }
    for (std::size_t i = 0; i < ncur; ++i) {
      if (!used[i]) {
        // Prime: add if not already present (dedup across levels).
        bool have = false;
        for (std::size_t k = 0; k < np; ++k) {
          if (primes[k].mask == cur[i].mask && primes[k].bits == cur[i].bits) {
            have = true;
            break;
          }
        }
        if (!have) {
          if (np >= 1024) {
            return false;
          }
          primes[np++] = cur[i];
        }
      }
    }
    if (nnxt == 0) {
      break;
    }
    for (std::size_t i = 0; i < nnxt; ++i) {
      cur[i] = nxt[i];
    }
    ncur = nnxt;
  }
  *nprimes = np;
  return np > 0;
}

// Greedy set cover of `minterms` by `primes` (both deterministic order).
// Picks the prime covering the most still-uncovered minterms (first-max
// tie-break). Returns false when uncovered remains (shouldn't happen) or
// cover would exceed 8 products (needs splitting; caller falls back).
inline bool qm_greedy_cover(const std::uint16_t *minterms, std::size_t nm,
                           const QmCube *primes, std::size_t np,
                           QmCube *cover, std::size_t *ncover) noexcept {
  if (nm == 0 || np == 0 || nm > 256) {
    return false;
  }
  bool covered[256] = {};
  std::size_t ncov = 0;
  std::size_t nuncovered = nm;
  // Guard against pathological loops (at most nm iterations, one prime per
  // iteration minimum).
  for (std::size_t iter = 0; iter < nm + 1; ++iter) {
    if (nuncovered == 0) {
      break;
    }
    std::size_t best = 0;
    std::size_t best_count = 0;
    bool have_best = false;
    for (std::size_t p = 0; p < np; ++p) {
      std::size_t cnt = 0;
      const std::uint16_t pm = primes[p].mask;
      const std::uint16_t pb = primes[p].bits;
      const std::uint16_t care = static_cast<std::uint16_t>(~pm);
      for (std::size_t i = 0; i < nm; ++i) {
        if (covered[i]) {
          continue;
        }
        if (((minterms[i] ^ pb) & care) == 0u) {
          ++cnt;
        }
      }
      if (!have_best || cnt > best_count) {
        have_best = true;
        best_count = cnt;
        best = p;
      }
    }
    if (!have_best || best_count == 0) {
      return false;
    }
    if (ncov >= 8) {
      return false; // would need splitting; fallback to structural
    }
    cover[ncov++] = primes[best];
    const std::uint16_t pm = primes[best].mask;
    const std::uint16_t pb = primes[best].bits;
    const std::uint16_t care = static_cast<std::uint16_t>(~pm);
    for (std::size_t i = 0; i < nm; ++i) {
      if (covered[i]) {
        continue;
      }
      if (((minterms[i] ^ pb) & care) == 0u) {
        covered[i] = true;
        --nuncovered;
      }
    }
  }
  if (nuncovered != 0) {
    return false;
  }
  *ncover = ncov;
  return true;
}

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
  std::uint16_t pool_c1 = kNoDriver;
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

// --- O2 post-cleanup (vocabulary-preserving) ---------------------------
// Merge identical universal gates (non-sinks, all inputs wired) and cancel
// double inverters U(U(E,E),U(E,E)) == E for internal gates. Both preserve
// the target vocabulary (all gates stay U) and block output count (sinks
// are never merged). Deterministic (slot order), allocation-free, noexcept.
// Dangling inputs are never merged (two [NoDriver,NoDriver] inverters may be
// distinct variables ~a vs ~b; merging would equate them).
inline bool dedup_universal_pass(Netlist &nl, GateKind u) noexcept {
  recount_fanout(nl);
  bool changed = false;
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    Gate &a = nl.gates[s];
    if (!a.live || a.kind != u || a.output_fanout == 0) {
      continue; // sinks preserved (output count stable)
    }
    if (a.ignored_mask != 0) {
      continue;
    }
    bool a_wired = true;
    for (std::uint8_t i = 0; i < a.input_count; ++i) {
      if (a.drivers[i] == kNoDriver) {
        a_wired = false;
        break;
      }
      if (a.drivers[i] >= kMaxGates || !nl.gates[a.drivers[i]].live) {
        a_wired = false;
        break;
      }
    }
    if (!a_wired) {
      continue; // dangling: distinct variables, never merge
    }
    // Sorted driver multiset for order-insensitive (commutative) compare.
    std::uint16_t a_sorted[kMaxInputs];
    for (std::uint8_t i = 0; i < a.input_count; ++i) {
      a_sorted[i] = a.drivers[i];
    }
    for (std::uint8_t i = 1; i < a.input_count; ++i) {
      std::uint16_t key = a_sorted[i];
      std::uint8_t j = i;
      while (j > 0 && a_sorted[j - 1] > key) {
        a_sorted[j] = a_sorted[j - 1];
        --j;
      }
      a_sorted[j] = key;
    }
    for (std::uint16_t t = static_cast<std::uint16_t>(s + 1); t < kMaxGates; ++t) {
      Gate &b = nl.gates[t];
      if (!b.live || b.kind != u || b.output_fanout == 0) {
        continue;
      }
      if (b.input_count != a.input_count || b.ignored_mask != 0) {
        continue;
      }
      bool b_wired = true;
      for (std::uint8_t i = 0; i < b.input_count; ++i) {
        if (b.drivers[i] == kNoDriver || b.drivers[i] >= kMaxGates || !nl.gates[b.drivers[i]].live) {
          b_wired = false;
          break;
        }
      }
      if (!b_wired) {
        continue;
      }
      std::uint16_t b_sorted[kMaxInputs];
      for (std::uint8_t i = 0; i < b.input_count; ++i) {
        b_sorted[i] = b.drivers[i];
      }
      for (std::uint8_t i = 1; i < b.input_count; ++i) {
        std::uint16_t key = b_sorted[i];
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
      // Merge higher (t) into lower (s): rewire users, remove duplicate.
      rewire_users(nl, t, s);
      (void)remove_gate(nl, t);
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

// Cancel internal double inverters: G=U(D,D), D=U(E,E) with D,E wired live
// gates => G==E. Only non-sinks (preserves output slots/count); dangling
// bases (E==NoDriver) are skipped to avoid interface churn.
inline bool double_inv_universal_pass(Netlist &nl, GateKind u) noexcept {
  recount_fanout(nl);
  bool changed = false;
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    Gate &g = nl.gates[s];
    if (!g.live || g.kind != u || g.output_fanout == 0 || g.input_count != 2) {
      continue;
    }
    const std::uint16_t d = g.drivers[0];
    if (d == kNoDriver || g.drivers[1] != d || d == s || d >= kMaxGates) {
      continue;
    }
    const Gate &dg = nl.gates[d];
    if (!dg.live || dg.kind != u || dg.input_count != 2) {
      continue;
    }
    const std::uint16_t e = dg.drivers[0];
    if (e == kNoDriver || dg.drivers[1] != e || e == s || e == d || e >= kMaxGates) {
      continue;
    }
    if (!nl.gates[e].live) {
      continue;
    }
    // G == E: rewire users, remove G (D becomes orphan if it only drove G;
    // later DCE removes it).
    rewire_users(nl, s, e);
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

// --- Minimized two-level resynthesis for O2 (n<=8, m<=8) ---------------
// Truth-table driven NAND-NAND (SOP) / NOR-NOR (POS) synthesis with shared
// input inverters and globally tied dangling pins (one block input per
// variable, so the interface stays stable instead of duplicating pins for
// inversion). Used as an alternative to the structural De Morgan translation;
// the caller picks the smaller (gates, then inputs). Deterministic (slot
// order, j-ascending lits, greedy cover order), allocation-free (fixed
// arrays), noexcept. Returns false on any limit/budget failure (caller falls
// back to structural, which is optimal for XOR-like parity).
struct MiniDang {
  std::uint16_t fresh = kNoDriver;
  std::uint8_t input = 0;
  std::uint8_t orig_j = 0;
};

inline bool mini_add_dang(MiniDang *list, std::size_t *n, std::size_t cap,
                          std::uint16_t fresh, std::uint8_t input, std::uint8_t orig_j) noexcept {
  if (*n >= cap) {
    return false;
  }
  list[*n].fresh = fresh;
  list[*n].input = input;
  list[*n].orig_j = orig_j;
  ++(*n);
  return true;
}

// Build NAND-NAND from SOP covers (one cover per output, greedy order).
// `covers[o][k]` are QmCubes for output o (SOP, lit = x_j if bits=1 else ~x_j).
// Returns false on budget/tie failure.
inline bool build_nand_sop(Netlist &out, std::uint8_t n, std::uint8_t m,
                           const QmCube *covers, const std::size_t *ncover,
                           const bool *is_const, const std::uint8_t *const_val,
                           MiniDang *dangs, std::size_t *ndangs, std::size_t dang_cap) noexcept {
  const GateKind u = GateKind::kNand;
  // Shared C1 for constant-0 sinks (dedicated C0 per sink).
  bool need_c1 = false;
  for (std::uint8_t o = 0; o < m; ++o) {
    if (is_const[o] && const_val[o] == 0) {
      need_c1 = true;
      break;
    }
  }
  std::uint16_t pool_c1 = kNoDriver;
  if (need_c1) {
    auto r = add_gate(out, u, 2);
    if (!r.ok()) {
      return false;
    }
    out.gates[r.value].ignored_mask = 0x03u;
    pool_c1 = r.value;
  }
  // Shared inverters for negative lits (width>1) and single-positive q's.
  bool need_inv[8] = {};
  for (std::uint8_t o = 0; o < m; ++o) {
    if (is_const[o]) {
      continue;
    }
    for (std::size_t k = 0; k < ncover[o]; ++k) {
      const QmCube c = covers[static_cast<std::size_t>(o) * 8 + k];
      const std::uint8_t width = static_cast<std::uint8_t>(n - qm_popcount(c.mask));
      if (width == 0) {
        continue;
      }
      if (width == 1) {
        // Single cared bit.
        std::uint8_t jk = 0;
        for (std::uint8_t j = 0; j < n; ++j) {
          if (((c.mask >> j) & 1u) == 0u) {
            jk = j;
            break;
          }
        }
        const bool positive = (((c.bits >> jk) & 1u) != 0u);
        if (positive) {
          // P=x_j, q=~x_j needs inverter.
          need_inv[jk] = true;
        } else {
          // P=~x_j, q=x_j wire (no inverter for q), but f itself needs
          // inverter only in single-product case (handled with dedicated
          // gate below, no shared needed).
        }
      } else {
        for (std::uint8_t j = 0; j < n; ++j) {
          if (((c.mask >> j) & 1u) != 0u) {
            continue;
          }
          const bool positive = (((c.bits >> j) & 1u) != 0u);
          if (!positive) {
            need_inv[j] = true;
          }
        }
      }
    }
    // Single-product single-negative outputs need a dedicated output
    // inverter (not shared, to keep outputs distinct); no shared needed.
  }
  std::uint16_t inv_slot[8];
  for (std::uint8_t j = 0; j < 8; ++j) {
    inv_slot[j] = kNoDriver;
  }
  for (std::uint8_t j = 0; j < n; ++j) {
    if (!need_inv[j]) {
      continue;
    }
    auto r = add_gate(out, u, 2);
    if (!r.ok()) {
      return false;
    }
    // Both inputs dangling for variable j (tied later).
    if (!mini_add_dang(dangs, ndangs, dang_cap, r.value, 0, j)) {
      return false;
    }
    if (!mini_add_dang(dangs, ndangs, dang_cap, r.value, 1, j)) {
      return false;
    }
    inv_slot[j] = r.value;
  }
  // Per-output networks in original output order.
  for (std::uint8_t o = 0; o < m; ++o) {
    if (is_const[o]) {
      if (const_val[o] == 1) {
        auto r = add_gate(out, u, 2);
        if (!r.ok()) {
          return false;
        }
        out.gates[r.value].ignored_mask = 0x03u;
      } else {
        if (pool_c1 == kNoDriver) {
          return false;
        }
        auto r = add_gate(out, u, 2);
        if (!r.ok()) {
          return false;
        }
        out.gates[r.value].drivers[0] = pool_c1;
        out.gates[r.value].drivers[1] = pool_c1;
      }
      continue;
    }
    const std::size_t p = ncover[o];
    if (p == 0 || p > 8) {
      return false;
    }
    if (p == 1) {
      const QmCube c = covers[static_cast<std::size_t>(o) * 8 + 0];
      const std::uint8_t width = static_cast<std::uint8_t>(n - qm_popcount(c.mask));
      if (width == 1) {
        std::uint8_t jk = 0;
        for (std::uint8_t j = 0; j < n; ++j) {
          if (((c.mask >> j) & 1u) == 0u) {
            jk = j;
            break;
          }
        }
        const bool positive = (((c.bits >> jk) & 1u) != 0u);
        if (positive) {
          // f=x_j: buffer via shared inv + dedicated NAND.
          if (inv_slot[jk] == kNoDriver) {
            return false;
          }
          auto r = add_gate(out, u, 2);
          if (!r.ok()) {
            return false;
          }
          out.gates[r.value].drivers[0] = inv_slot[jk];
          out.gates[r.value].drivers[1] = inv_slot[jk];
        } else {
          // f=~x_j: dedicated output inverter.
          auto r = add_gate(out, u, 2);
          if (!r.ok()) {
            return false;
          }
          if (!mini_add_dang(dangs, ndangs, dang_cap, r.value, 0, jk)) {
            return false;
          }
          if (!mini_add_dang(dangs, ndangs, dang_cap, r.value, 1, jk)) {
            return false;
          }
        }
      } else {
        // f=AND lits: product NAND + output NOT.
        auto rp = add_gate(out, u, width);
        if (!rp.ok()) {
          return false;
        }
        std::uint8_t idx = 0;
        for (std::uint8_t j = 0; j < n; ++j) {
          if (((c.mask >> j) & 1u) != 0u) {
            continue;
          }
          const bool positive = (((c.bits >> j) & 1u) != 0u);
          if (positive) {
            if (!mini_add_dang(dangs, ndangs, dang_cap, rp.value, idx, j)) {
              return false;
            }
          } else {
            if (inv_slot[j] == kNoDriver) {
              return false;
            }
            out.gates[rp.value].drivers[idx] = inv_slot[j];
          }
          ++idx;
        }
        auto ro = add_gate(out, u, 2);
        if (!ro.ok()) {
          return false;
        }
        out.gates[ro.value].drivers[0] = rp.value;
        out.gates[ro.value].drivers[1] = rp.value;
      }
      continue;
    }
    // Multi-product: product gates + final NAND.
    std::uint16_t q_slot[8];
    bool q_wired[8] = {};
    std::uint8_t q_dang_j[8] = {};
    for (std::size_t k = 0; k < p; ++k) {
      const QmCube c = covers[static_cast<std::size_t>(o) * 8 + k];
      const std::uint8_t width = static_cast<std::uint8_t>(n - qm_popcount(c.mask));
      if (width == 1) {
        std::uint8_t jk = 0;
        for (std::uint8_t j = 0; j < n; ++j) {
          if (((c.mask >> j) & 1u) == 0u) {
            jk = j;
            break;
          }
        }
        const bool positive = (((c.bits >> jk) & 1u) != 0u);
        if (positive) {
          if (inv_slot[jk] == kNoDriver) {
            return false;
          }
          q_slot[k] = inv_slot[jk];
          q_wired[k] = true;
        } else {
          q_wired[k] = false;
          q_dang_j[k] = jk;
        }
      } else {
        auto rp = add_gate(out, u, width);
        if (!rp.ok()) {
          return false;
        }
        std::uint8_t idx = 0;
        for (std::uint8_t j = 0; j < n; ++j) {
          if (((c.mask >> j) & 1u) != 0u) {
            continue;
          }
          const bool positive = (((c.bits >> j) & 1u) != 0u);
          if (positive) {
            if (!mini_add_dang(dangs, ndangs, dang_cap, rp.value, idx, j)) {
              return false;
            }
          } else {
            if (inv_slot[j] == kNoDriver) {
              return false;
            }
            out.gates[rp.value].drivers[idx] = inv_slot[j];
          }
          ++idx;
        }
        q_slot[k] = rp.value;
        q_wired[k] = true;
      }
    }
    auto rf = add_gate(out, u, static_cast<std::uint8_t>(p));
    if (!rf.ok()) {
      return false;
    }
    for (std::size_t k = 0; k < p; ++k) {
      if (q_wired[k]) {
        out.gates[rf.value].drivers[k] = q_slot[k];
      } else {
        if (!mini_add_dang(dangs, ndangs, dang_cap, rf.value, static_cast<std::uint8_t>(k), q_dang_j[k])) {
          return false;
        }
      }
    }
  }
  return true;
}

// Build NOR-NOR from POS covers (sums). Dual of NAND-SOP: lit = x_j if
// maxterm bit=0 else ~x_j; sums via NOR, final via NOR.
inline bool build_nor_pos(Netlist &out, std::uint8_t n, std::uint8_t m,
                          const QmCube *covers, const std::size_t *ncover,
                          const bool *is_const, const std::uint8_t *const_val,
                          MiniDang *dangs, std::size_t *ndangs, std::size_t dang_cap) noexcept {
  const GateKind u = GateKind::kNor;
  bool need_c1 = false;
  for (std::uint8_t o = 0; o < m; ++o) {
    if (is_const[o] && const_val[o] == 1) {
      need_c1 = true;
      break;
    }
  }
  // Shared C0 for constant-1 sinks? NOR C1 needs 2 gates (C0 shared +
  // dedicated C1). C0 = NOR(0,0)=1? Wait NOR(0,0)=1 (C1). Let's define:
  // shared C1 = NOR(0,0)=1 (1 gate, ignored). Shared C0? For NOR, C0 needs
  // C1? NOR(C1,C1)=~(1|1)=0 (C0). Good same pattern as NAND (swap roles?
  // Actually same: C1=U(0,0)=1, C0=U(C1,C1)=0 in both vocabularies).
  std::uint16_t pool_c1 = kNoDriver;
  if (need_c1) {
    // Need shared C0? No, C1 sinks need shared C0? C1 sink = NOR(C0_shared,C0_shared)?
    // C0_shared = ??? Let's use same pattern as NAND but swapped:
    // For NOR, const-1 sink needs 2 gates (shared C1? No). Simpler: mirror
    // NAND logic with C0/C1 swapped? Actually U(0,0)=1 in both, U(1,1)=0 in
    // both. So same pool structure works: shared C1 (1 gate) + dedicated
    // C0/C1 sinks as in NAND, but with NOR gates. For const-1 sink in NOR:
    // needs 2 gates (shared C1? No, const-1 = 1 needs... Let's reuse same
    // scheme as NAND: const-1 sink = dedicated U(0,0) (1 gate, no shared);
    // const-0 sink = dedicated U(C1_shared,C1_shared) (needs shared C1).
    // Wait const-0 in NOR: 0 needs... U(0,0)=1 (C1), U(1,1)=0 (C0). So const-0
    // sink needs 2 gates (shared C1 + dedicated C0). Const-1 sink needs 1
    // gate (dedicated C1). Same as NAND! Good, same as NAND (since U(0,0)=1
    // in both). So need shared C1 iff any const-0 output exists (not const-1).
    // Our need_c1 above checks const-1, wrong. Fix: need shared C1 iff any
    // const-0 output exists.
  }
  // Recompute correctly: shared C1 needed for const-0 sinks.
  need_c1 = false;
  for (std::uint8_t o = 0; o < m; ++o) {
    if (is_const[o] && const_val[o] == 0) {
      need_c1 = true;
      break;
    }
  }
  if (need_c1) {
    auto r = add_gate(out, u, 2);
    if (!r.ok()) {
      return false;
    }
    out.gates[r.value].ignored_mask = 0x03u;
    pool_c1 = r.value;
  }
  // Shared inverters for positive lits? For POS sums: sum = OR lits where
  // lit = x_j if maxterm bit=0 (positive) else ~x_j (negative, needs inv).
  // Product (NAND) needed inv for negative lits; sum (NOR) needs inv for
  // positive? Let's derive: sum S = OR lits, r = ~S = NOR(lits). Lits:
  // positive x_j direct wire (dangling), negative ~x_j via inverter.
  // For POS, lit polarity: bit=0 => positive x_j (direct), bit=1 =>
  // negative ~x_j (needs inv). Good (opposite of SOP where bit=1 positive).
  bool need_inv[8] = {};
  for (std::uint8_t o = 0; o < m; ++o) {
    if (is_const[o]) {
      continue;
    }
    for (std::size_t k = 0; k < ncover[o]; ++k) {
      const QmCube c = covers[static_cast<std::size_t>(o) * 8 + k];
      const std::uint8_t width = static_cast<std::uint8_t>(n - qm_popcount(c.mask));
      if (width == 0) {
        continue;
      }
      if (width == 1) {
        std::uint8_t jk = 0;
        for (std::uint8_t j = 0; j < n; ++j) {
          if (((c.mask >> j) & 1u) == 0u) {
            jk = j;
            break;
          }
        }
        const bool positive = (((c.bits >> jk) & 1u) == 0u); // POS: bit 0 => positive
        if (positive) {
          // Single-lit sum S=x_j (positive): r=~x_j needs inverter (shared).
          // Single-negative S=~x_j needs no shared inv (r=x_j wire; f itself
          // uses a dedicated output inverter below).
          need_inv[jk] = true;
        }
      } else {
        for (std::uint8_t j = 0; j < n; ++j) {
          if (((c.mask >> j) & 1u) != 0u) {
            continue;
          }
          const bool positive = (((c.bits >> j) & 1u) == 0u);
          if (!positive) {
            need_inv[j] = true;
          }
        }
      }
    }
  }
  // Note: single-sum single-negative (S=~x_j) needs no shared inv for r
  // (r=x_j wire), but f itself (single sum) needs special handling below
  // with dedicated gates. Good.
  std::uint16_t inv_slot[8];
  for (std::uint8_t j = 0; j < 8; ++j) {
    inv_slot[j] = kNoDriver;
  }
  for (std::uint8_t j = 0; j < n; ++j) {
    if (!need_inv[j]) {
      continue;
    }
    auto r = add_gate(out, u, 2);
    if (!r.ok()) {
      return false;
    }
    if (!mini_add_dang(dangs, ndangs, dang_cap, r.value, 0, j)) {
      return false;
    }
    if (!mini_add_dang(dangs, ndangs, dang_cap, r.value, 1, j)) {
      return false;
    }
    inv_slot[j] = r.value;
  }
  for (std::uint8_t o = 0; o < m; ++o) {
    if (is_const[o]) {
      if (const_val[o] == 1) {
        auto r = add_gate(out, u, 2);
        if (!r.ok()) {
          return false;
        }
        out.gates[r.value].ignored_mask = 0x03u;
      } else {
        if (pool_c1 == kNoDriver) {
          return false;
        }
        auto r = add_gate(out, u, 2);
        if (!r.ok()) {
          return false;
        }
        out.gates[r.value].drivers[0] = pool_c1;
        out.gates[r.value].drivers[1] = pool_c1;
      }
      continue;
    }
    const std::size_t p = ncover[o];
    if (p == 0 || p > 8) {
      return false;
    }
    if (p == 1) {
      const QmCube c = covers[static_cast<std::size_t>(o) * 8 + 0];
      const std::uint8_t width = static_cast<std::uint8_t>(n - qm_popcount(c.mask));
      if (width == 1) {
        std::uint8_t jk = 0;
        for (std::uint8_t j = 0; j < n; ++j) {
          if (((c.mask >> j) & 1u) == 0u) {
            jk = j;
            break;
          }
        }
        const bool positive = (((c.bits >> jk) & 1u) == 0u);
        if (positive) {
          // f=x_j: buffer via shared inv? S=x_j, r=~x_j (shared inv), f=~r=x_j
          // needs final NOR(inv,inv). Use shared inv + dedicated final.
          if (inv_slot[jk] == kNoDriver) {
            return false;
          }
          auto r = add_gate(out, u, 2);
          if (!r.ok()) {
            return false;
          }
          out.gates[r.value].drivers[0] = inv_slot[jk];
          out.gates[r.value].drivers[1] = inv_slot[jk];
        } else {
          // f=~x_j: dedicated output inverter.
          auto r = add_gate(out, u, 2);
          if (!r.ok()) {
            return false;
          }
          if (!mini_add_dang(dangs, ndangs, dang_cap, r.value, 0, jk)) {
            return false;
          }
          if (!mini_add_dang(dangs, ndangs, dang_cap, r.value, 1, jk)) {
            return false;
          }
        }
      } else {
        // f=AND? No, POS single sum: f=S=OR lits. S via NOR + NOT?
        // S=OR lits, r=~S=NOR(lits) (1 gate), f=~r=NOT(r) (1 gate).
        auto rp = add_gate(out, u, width);
        if (!rp.ok()) {
          return false;
        }
        std::uint8_t idx = 0;
        for (std::uint8_t j = 0; j < n; ++j) {
          if (((c.mask >> j) & 1u) != 0u) {
            continue;
          }
          const bool positive = (((c.bits >> j) & 1u) == 0u);
          if (positive) {
            if (!mini_add_dang(dangs, ndangs, dang_cap, rp.value, idx, j)) {
              return false;
            }
          } else {
            if (inv_slot[j] == kNoDriver) {
              return false;
            }
            out.gates[rp.value].drivers[idx] = inv_slot[j];
          }
          ++idx;
        }
        auto ro = add_gate(out, u, 2);
        if (!ro.ok()) {
          return false;
        }
        out.gates[ro.value].drivers[0] = rp.value;
        out.gates[ro.value].drivers[1] = rp.value;
      }
      continue;
    }
    std::uint16_t q_slot[8];
    bool q_wired[8] = {};
    std::uint8_t q_dang_j[8] = {};
    for (std::size_t k = 0; k < p; ++k) {
      const QmCube c = covers[static_cast<std::size_t>(o) * 8 + k];
      const std::uint8_t width = static_cast<std::uint8_t>(n - qm_popcount(c.mask));
      if (width == 1) {
        std::uint8_t jk = 0;
        for (std::uint8_t j = 0; j < n; ++j) {
          if (((c.mask >> j) & 1u) == 0u) {
            jk = j;
            break;
          }
        }
        const bool positive = (((c.bits >> jk) & 1u) == 0u);
        if (positive) {
          // S=x_j single, r=~x_j needs inv (shared) as q? For multi-sum,
          // final AND? Wait NOR-NOR: f=AND_i S_i, r_i=~S_i=NOR(lits), final= NOR(r_i)= AND(S_i).
          // For single-lit sum S=x_j, r=~x_j (inv) as q (wired). Good.
          if (inv_slot[jk] == kNoDriver) {
            return false;
          }
          q_slot[k] = inv_slot[jk];
          q_wired[k] = true;
        } else {
          // S=~x_j, r=x_j wire (dangling).
          q_wired[k] = false;
          q_dang_j[k] = jk;
        }
      } else {
        auto rp = add_gate(out, u, width);
        if (!rp.ok()) {
          return false;
        }
        std::uint8_t idx = 0;
        for (std::uint8_t j = 0; j < n; ++j) {
          if (((c.mask >> j) & 1u) != 0u) {
            continue;
          }
          const bool positive = (((c.bits >> j) & 1u) == 0u);
          if (positive) {
            if (!mini_add_dang(dangs, ndangs, dang_cap, rp.value, idx, j)) {
              return false;
            }
          } else {
            if (inv_slot[j] == kNoDriver) {
              return false;
            }
            out.gates[rp.value].drivers[idx] = inv_slot[j];
          }
          ++idx;
        }
        q_slot[k] = rp.value;
        q_wired[k] = true;
      }
    }
    auto rf = add_gate(out, u, static_cast<std::uint8_t>(p));
    if (!rf.ok()) {
      return false;
    }
    for (std::size_t k = 0; k < p; ++k) {
      if (q_wired[k]) {
        out.gates[rf.value].drivers[k] = q_slot[k];
      } else {
        if (!mini_add_dang(dangs, ndangs, dang_cap, rf.value, static_cast<std::uint8_t>(k), q_dang_j[k])) {
          return false;
        }
      }
    }
  }
  return true;
}

// Attempt truth-table resynthesis for O2 (n<=8, m<=8, no hidden outputs).
// Builds a minimized two-level network (SOP for NAND, POS for NOR) with
// shared inverters and globally tied inputs, verifies equivalence under pin
// replication/permutation, and leaves the result in `out_min`. Returns false
// on any limit/budget/equivalence failure (caller falls back to structural,
// which is optimal for XOR-like parity). Deterministic, allocation-free,
// noexcept.
inline bool try_minimized_o2(Netlist &src, OptTarget target, Netlist &out_min) noexcept {
  const std::size_t n = block_input_count(src);
  const std::size_t m = block_output_count(src);
  if (n == 0 || n > 8 || m == 0 || m > 8) {
    return false;
  }
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    if (!src.gates[s].live) {
      continue;
    }
    if (src.gates[s].ignore_output) {
      return false; // preserve hidden logic via structural path
    }
    if (is_sequential_kind(src.gates[s].kind)) {
      return false; // O2 refuses sequential loudly; let structural fail
    }
  }
  BlockPin in_buf[8];
  BlockPin out_buf[8];
  std::size_t in_n = 0;
  std::size_t out_n = 0;
  if (!block_interface(src, in_buf, 8, &in_n, out_buf, 8, &out_n).ok()) {
    return false;
  }
  if (in_n != n || out_n != m) {
    return false;
  }
  if (!ensure_topology(src).ok()) {
    return false;
  }
  const std::size_t rows = static_cast<std::size_t>(1ULL << n);
  // Source truth table (rows*m, 0/1). 256*8 = 2KB stack.
  std::uint8_t tt_src[256 * 8];
  LogicValue in_vals[8];
  LogicValue out_vals[8];
  for (std::size_t r = 0; r < rows; ++r) {
    for (std::size_t j = 0; j < n; ++j) {
      in_vals[j] = (((r >> j) & 1ULL) != 0ULL) ? LogicValue::kHigh : LogicValue::kLow;
    }
    for (std::size_t o = 0; o < m; ++o) {
      out_vals[o] = LogicValue::kLow;
    }
    if (!solve(src, std::span<const LogicValue>(in_vals, n), std::span<LogicValue>(out_vals, m)).ok()) {
      return false;
    }
    for (std::size_t o = 0; o < m; ++o) {
      tt_src[r * m + o] = (out_vals[o] == LogicValue::kHigh) ? 1u : 0u;
    }
  }
  // QM covers per output.
  QmCube covers[8 * 8];
  std::size_t ncover[8] = {};
  bool is_const[8] = {};
  std::uint8_t const_val[8] = {};
  for (std::size_t o = 0; o < m; ++o) {
    std::uint16_t terms[256];
    std::size_t nterms = 0;
    if (target == OptTarget::kNand) {
      // SOP: minterms where output=1.
      for (std::size_t r = 0; r < rows; ++r) {
        if (tt_src[r * m + o] != 0u) {
          terms[nterms++] = static_cast<std::uint16_t>(r);
        }
      }
    } else {
      // POS: maxterms where output=0.
      for (std::size_t r = 0; r < rows; ++r) {
        if (tt_src[r * m + o] == 0u) {
          terms[nterms++] = static_cast<std::uint16_t>(r);
        }
      }
    }
    if (nterms == 0) {
      is_const[o] = true;
      const_val[o] = (target == OptTarget::kNand) ? 0u : 1u;
      // SOP with 0 minterms = constant 0; POS with 0 maxterms = constant 1.
      ncover[o] = 0;
      continue;
    }
    if (nterms == rows) {
      is_const[o] = true;
      const_val[o] = (target == OptTarget::kNand) ? 1u : 0u;
      ncover[o] = 0;
      continue;
    }
    QmCube primes[1024];
    std::size_t np = 0;
    if (!qm_primes(terms, nterms, static_cast<std::uint8_t>(n), primes, &np)) {
      return false;
    }
    QmCube cover[8];
    std::size_t ncov = 0;
    if (!qm_greedy_cover(terms, nterms, primes, np, cover, &ncov)) {
      return false;
    }
    if (ncov == 0 || ncov > 8) {
      return false;
    }
    // Width check (fan-in <=8): n<=8 ensures width<=8, but empty cubes
    // (width 0, tautology) only occur for constants (handled above).
    for (std::size_t k = 0; k < ncov; ++k) {
      const std::uint8_t width = static_cast<std::uint8_t>(n - qm_popcount(cover[k].mask));
      if (width == 0 || width > 8) {
        return false;
      }
    }
    is_const[o] = false;
    ncover[o] = ncov;
    for (std::size_t k = 0; k < ncov; ++k) {
      covers[o * 8 + k] = cover[k];
    }
  }
  Netlist built{};
  MiniDang dangs[1024];
  std::size_t ndangs = 0;
  bool built_ok = false;
  if (target == OptTarget::kNand) {
    built_ok = build_nand_sop(built, static_cast<std::uint8_t>(n), static_cast<std::uint8_t>(m),
                              covers, ncover, is_const, const_val, dangs, &ndangs, 1024);
  } else {
    built_ok = build_nor_pos(built, static_cast<std::uint8_t>(n), static_cast<std::uint8_t>(m),
                             covers, ncover, is_const, const_val, dangs, &ndangs, 1024);
  }
  if (!built_ok) {
    return false;
  }
  // Global tying: one block input per original variable.
  for (std::uint8_t j = 0; j < static_cast<std::uint8_t>(n); ++j) {
    bool have_leader = false;
    PinRef leader{};
    for (std::size_t d = 0; d < ndangs; ++d) {
      if (dangs[d].orig_j != j) {
        continue;
      }
      PinRef cur{};
      cur.slot = dangs[d].fresh;
      cur.is_output = false;
      cur.input = dangs[d].input;
      // Skip if no longer dangling (wired since? dang list only has
      // dangling at creation, but later tying doesn't wire, so still
      // dangling; defensive check).
      if (cur.slot >= kMaxGates || !built.gates[cur.slot].live) {
        return false;
      }
      const Gate &g = built.gates[cur.slot];
      if (cur.input >= g.input_count || g.drivers[cur.input] != kNoDriver) {
        continue;
      }
      if ((g.ignored_mask & static_cast<std::uint8_t>(1u << cur.input)) != 0) {
        continue;
      }
      if (!have_leader) {
        leader = cur;
        have_leader = true;
      } else {
        if (!tie_pins(built, leader, cur).ok()) {
          return false;
        }
      }
    }
  }
  recount_fanout(built);
  built.dirty = true;
  built.topo_size = 0;
  if (!ensure_topology(built).ok()) {
    return false;
  }
  // Interface + proj mapping (minimized inputs -> original inputs).
  BlockPin min_in[8];
  BlockPin min_out[8];
  std::size_t min_in_n = 0;
  std::size_t min_out_n = 0;
  if (!block_interface(built, min_in, 8, &min_in_n, min_out, 8, &min_out_n).ok()) {
    return false;
  }
  if (min_out_n != m) {
    return false; // output count must be preserved
  }
  if (min_in_n > n) {
    return false;
  }
  // Map each minimized input leader to its original j via dang list.
  std::size_t proj[8] = {};
  for (std::size_t p = 0; p < min_in_n; ++p) {
    bool found = false;
    for (std::size_t d = 0; d < ndangs; ++d) {
      // Any pin in the same tie class shares orig_j; leaders are in dang
      // list (since leaders are dangling pins themselves).
      if (dangs[d].fresh == min_in[p].slot && dangs[d].input == min_in[p].input) {
        proj[p] = dangs[d].orig_j;
        found = true;
        break;
      }
    }
    if (!found) {
      // Leader not directly in dang list? It should be (leaders are dangling
      // pins). Search by tie class: any dang pin in same class.
      const std::size_t flat = tie_flat(min_in[p].slot, min_in[p].input);
      bool found2 = false;
      for (std::size_t d = 0; d < ndangs; ++d) {
        const std::size_t f2 = tie_flat(dangs[d].fresh, dangs[d].input);
        if (tie_leader_flat(built, f2) == tie_leader_flat(built, flat)) {
          proj[p] = dangs[d].orig_j;
          found2 = true;
          break;
        }
      }
      if (!found2) {
        return false;
      }
    }
    if (proj[p] >= n) {
      return false;
    }
  }
  // Equivalence check under proj (and replication via ties is already exact,
  // since tied inputs share one column; proj handles permutation).
  LogicValue min_in_vals[8];
  LogicValue min_out_vals[8];
  for (std::size_t r = 0; r < rows; ++r) {
    for (std::size_t p = 0; p < min_in_n; ++p) {
      const std::size_t j = proj[p];
      min_in_vals[p] = (((r >> j) & 1ULL) != 0ULL) ? LogicValue::kHigh : LogicValue::kLow;
    }
    for (std::size_t o = 0; o < m; ++o) {
      min_out_vals[o] = LogicValue::kLow;
    }
    if (!solve(built, std::span<const LogicValue>(min_in_vals, min_in_n),
               std::span<LogicValue>(min_out_vals, m))
             .ok()) {
      return false;
    }
    for (std::size_t o = 0; o < m; ++o) {
      if (((min_out_vals[o] == LogicValue::kHigh) ? 1u : 0u) != tt_src[r * m + o]) {
        return false; // not equivalent; fallback to structural
      }
    }
  }
  out_min = built;
  return true;
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
    if (detail::exact_simplify_pass(nl)) {
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

  // Constant-aware shared pool: structural constant labels (ignored pins
  // tie to 0) let us reuse the pool instead of re-emitting logic for
  // constant gates. Internal constants share one pool; constant sinks get
  // dedicated output gates (sharing a sink would merge distinct block
  // outputs). This keeps 1-gate constants at 1-2 gates instead of 4+.
  detail::Constness clabel[kMaxGates];
  detail::classify(nl, clabel);
  bool need_shared_c1 = false;
  bool need_shared_c0 = false;
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    if (!nl.gates[s].live || !reach[s]) {
      continue;
    }
    const bool is_sink = (nl.gates[s].output_fanout == 0);
    const detail::Constness lc = clabel[s];
    if (lc == detail::Constness::kC0 || lc == detail::Constness::kC1) {
      if (is_sink) {
        // Dedicated sink gates need the shared C1 as driver for C0 sinks.
        if (lc == detail::Constness::kC0) {
          need_shared_c1 = true;
        }
      } else {
        if (lc == detail::Constness::kC0) {
          need_shared_c1 = true;
          need_shared_c0 = true;
        } else {
          need_shared_c1 = true;
        }
      }
    } else if (nl.gates[s].ignored_mask != 0) {
      // Non-constant gates with ignored inputs route through shared C0
      // (which needs C1).
      need_shared_c1 = true;
      need_shared_c0 = true;
    }
  }
  if (need_shared_c1) {
    const GateKind u = detail::universal_kind(opts.target);
    auto rc1 = add_gate(out, u, 2);
    if (!rc1.ok()) {
      return core::Result<Netlist>::failure("optimize: gate budget exceeded (kMaxGates)", core::StatusCode::kInternal);
    }
    Gate &c1 = out.gates[rc1.value];
    c1.ignored_mask = 0x03u; // U(0,0) = 1 in both vocabularies
    ctx.pool_c1 = rc1.value;
    if (need_shared_c0) {
      auto rc0 = add_gate(out, u, 2);
      if (!rc0.ok()) {
        return core::Result<Netlist>::failure("optimize: gate budget exceeded (kMaxGates)", core::StatusCode::kInternal);
      }
      out.gates[rc0.value].drivers[0] = rc1.value;
      out.gates[rc0.value].drivers[1] = rc1.value;
      ctx.pool_c0 = rc0.value;
    }
  }

  // Creation pass in original slot order (fresh slots follow it).
  // Constant gates reuse the pool (internal share, sinks get dedicated
  // outputs); all other gates emit via De Morgan as before.
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    if (!nl.gates[s].live || !reach[s]) {
      continue;
    }
    const detail::Constness lc = clabel[s];
    const bool is_sink = (nl.gates[s].output_fanout == 0);
    if (lc == detail::Constness::kC0 || lc == detail::Constness::kC1) {
      if (!is_sink) {
        // Internal constant: share the pool, no new gates.
        if (lc == detail::Constness::kC0) {
          if (ctx.pool_c0 == kNoDriver) {
            return core::Result<Netlist>::failure("optimize: internal wiring error", core::StatusCode::kInternal);
          }
          emit_map[s] = ctx.pool_c0;
        } else {
          if (ctx.pool_c1 == kNoDriver) {
            return core::Result<Netlist>::failure("optimize: internal wiring error", core::StatusCode::kInternal);
          }
          emit_map[s] = ctx.pool_c1;
        }
        continue;
      }
      // Constant sink: dedicated output gate(s) so distinct block outputs
      // stay distinct.
      const GateKind u = detail::universal_kind(opts.target);
      if (lc == detail::Constness::kC1) {
        auto r = add_gate(out, u, 2);
        if (!r.ok()) {
          return core::Result<Netlist>::failure("optimize: gate budget exceeded (kMaxGates)", core::StatusCode::kInternal);
        }
        out.gates[r.value].ignored_mask = 0x03u;
        emit_map[s] = r.value;
        continue;
      } else {
        // C0 sink: dedicated U(C1_shared, C1_shared). Shared C1 must exist
        // (ensured by need_shared_c1 above).
        if (ctx.pool_c1 == kNoDriver) {
          return core::Result<Netlist>::failure("optimize: internal wiring error", core::StatusCode::kInternal);
        }
        auto r = add_gate(out, u, 2);
        if (!r.ok()) {
          return core::Result<Netlist>::failure("optimize: gate budget exceeded (kMaxGates)", core::StatusCode::kInternal);
        }
        out.gates[r.value].drivers[0] = ctx.pool_c1;
        out.gates[r.value].drivers[1] = ctx.pool_c1;
        emit_map[s] = r.value;
        continue;
      }
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
  // Vocabulary-preserving post-cleanup: share identical wired gates and
  // cancel internal double inverters (both keep all-NAND/all-NOR and never
  // merge sinks, so output count is stable). Runs to fixpoint with DCE for
  // orphans (roots = original fresh outputs, hidden included, so orphans
  // that become sinks are removed, not kept as extra outputs).
  {
    const GateKind u = detail::universal_kind(opts.target);
    bool post_roots[kMaxGates] = {};
    for (std::uint16_t s = 0; s < kMaxGates; ++s) {
      if (!nl.gates[s].live || !reach[s]) {
        continue;
      }
      if (nl.gates[s].output_fanout == 0) {
        const std::uint16_t o = emit_map[s];
        if (o != kNoDriver && o < kMaxGates) {
          post_roots[o] = true;
        }
      }
    }
    constexpr std::size_t kPostIters = 2 * kMaxGates + 16;
    for (std::size_t it = 0; it < kPostIters; ++it) {
      bool pchanged = false;
      if (detail::dedup_universal_pass(out, u)) {
        pchanged = true;
      }
      if (!ensure_topology(out).ok()) {
        return core::Result<Netlist>::failure("optimize: topology error", core::StatusCode::kInternal);
      }
      if (detail::double_inv_universal_pass(out, u)) {
        pchanged = true;
      }
      if (!ensure_topology(out).ok()) {
        return core::Result<Netlist>::failure("optimize: topology error", core::StatusCode::kInternal);
      }
      if (detail::dce_pass(out, post_roots)) {
        pchanged = true;
      }
      if (!ensure_topology(out).ok()) {
        return core::Result<Netlist>::failure("optimize: topology error", core::StatusCode::kInternal);
      }
      if (!pchanged) {
        break;
      }
      if (it + 1 >= kPostIters) {
        return core::Result<Netlist>::failure("optimize: internal iteration bound", core::StatusCode::kInternal);
      }
    }
  }
  // Truth-table resynthesis for small blocks (Karnaugh-style exact
  // minimization): try minimized two-level network, pick smaller (gates,
  // then inputs). Fixes repeated O2 toggles (NAND<->NOR) staying near-optimal
  // instead of 4x per toggle; falls back to structural for XOR-like parity
  // (where two-level is larger) and large blocks (n>8 or m>8).
  {
    Netlist min_out{};
    if (detail::try_minimized_o2(nl, opts.target, min_out)) {
      const std::uint16_t g_struct = out.live_count;
      const std::uint16_t g_min = min_out.live_count;
      const std::size_t in_struct = block_input_count(out);
      const std::size_t in_min = block_input_count(min_out);
      if ((g_min < g_struct) || (g_min == g_struct && in_min < in_struct)) {
        return core::Result<Netlist>::success(min_out);
      }
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
