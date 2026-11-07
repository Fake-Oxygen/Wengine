#pragma once

#include "wengine/core/status.hpp"
#include "wengine/logic/eval.hpp"
#include "wengine/logic/netlist.hpp"
#include "wengine/logic/optimize_types.hpp"
#include "wengine/logic/optimize_barrier.hpp"
#include "wengine/logic/types.hpp"

#include <cstddef>
#include <cstdint>

namespace wengine::logic {

namespace detail {


enum class Constness : std::uint8_t {
  kC0 = 0,
  kC1 = 1,
  kV = 2, // varies with block inputs
};

// Forward declarations for exact pass (defined in optimize_passes.hpp;
// detach_slot_ties lives in tie_set.hpp).
inline void recount_fanout(Netlist &nl) noexcept;
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
  // Opaque barrier: block truth tables are undefined for stateful nets
  // (outputs depend on history, and solve() reads flop outputs as kLow),
  // so the exact pass skips sequential nets entirely. Structural passes
  // still simplify the combinational glue around the barriers.
  if (has_sequential_gates(nl)) {
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
  PinRef in_buf[10];
  PinRef out_buf[16];
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
  if (!topo::rebuild(nl, EdgeFilter::kAll).ok()) {
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
    // Dissolve via the TieSet seam: untie every member, mark ignored.
    // The class always holds at least its leader pin, so this changes the net.
    ignore_tie_class(nl, leader);
    changed = true;
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
    } else {
      const bool canonical = g.kind == GateKind::kNand && g.input_count == 2 && g.ignored_mask == 0x03u &&
                             g.drivers[0] == kNoDriver && g.drivers[1] == kNoDriver;
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

} // namespace detail

} // namespace wengine::logic
