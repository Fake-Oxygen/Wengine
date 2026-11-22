/**
 * @file
 * Minimized two-level resynthesis for O2 (n<=8, m<=8): the exact-
 * minimization stage of the O2 fresh build inside the optimize stage of
 * the logic pipeline (netlist parse -> state -> eval -> layout/topo ->
 * optimize). Truth-table driven NAND-NAND (SOP) / NOR-NOR (POS)
 * synthesis with shared input inverters and globally tied dangling pins
 * (one block input per variable, so the interface stays stable instead
 * of duplicating pins for inversion). Used as an alternative to the
 * structural De Morgan translation; the caller picks the smaller
 * (gates, then inputs).
 *
 * What it preserves: the truth table exactly, verified under pin
 * replication/permutation. Cost class: QM prime search plus greedy
 * cover per output — bounded by the n/m caps, with loud false-return
 * fallback to the structural De Morgan translation (which is optimal
 * for XOR-like parity).
 *
 * Capacity (dynamic pool): the builder works over Netlist (MiniDang
 * beside it); the truth-table bounds (n<=8, m<=8) and every QM/day
 * scratch stay capacity-free and fixed. The equivalence re-solve takes
 * the caller's eval + Kahn scratch. Internal cross-calls are bare (no
 * capacity parameter).
 *
 * Dependency rules: header-only, allocation-free, exception-free;
 * noexcept throughout; deterministic (slot order, j-ascending
 * literals, greedy cover order). Lives in the
 * wengine::logic::detail seam — internal to the optimizer family.
 * Never depends on CLI types. Consumers: optimize_o2_build.hpp
 * (finish_o2 picking) and the logic optimizer tests.
 */

#pragma once

/**
 * @file
 * Minimized two-level resynthesis for O2 (the "min" stage of the O2
 * fresh build inside the optimize stage of the logic pipeline: netlist
 * parse -> state -> eval -> layout/topo -> optimize). For small blocks
 * (n<=8, m<=8, no hidden outputs, no sequential gates) it rebuilds the
 * net from the truth table — SOP NAND-NAND for kNand, POS NOR-NOR for
 * kNor — with shared input inverters and globally tied dangling pins
 * (one block input per variable, so the interface stays stable instead
 * of duplicating pins for inversion). optimize_o2_build.hpp's
 * finish_o2 then picks the smaller of minimized vs structural (gates,
 * then inputs), so repeated nand/nor toggles converge instead of
 * growing 4x per toggle.
 *
 * What it preserves: the truth table exactly, verified by explicit
 * re-solve under pin permutation (tied inputs share one column);
 * output count (constant sinks get dedicated gates). Cost class:
 * truth-table enumeration (256 rows max) plus Quine-McCluskey primes
 * and greedy cover per output — bounded by the n/m caps, with loud
 * false-return fallback to the structural De Morgan translation
 * (which is optimal for XOR-like parity).
 *
 * Dependency rules: header-only, allocation-free, exception-free,
 * noexcept throughout; deterministic (slot order, j-ascending
 * literals, greedy cover order). Lives in the
 * wengine::logic::detail seam — internal to the optimizer family.
 * Never depends on CLI types. Consumers: optimize_o2_build.hpp
 * (finish_o2 picking) and the logic optimizer tests.
 */

#include "wengine/core/status.hpp"
#include "wengine/logic/adjacency.hpp"
#include "wengine/logic/eval.hpp"
#include "wengine/logic/netlist.hpp"
#include "wengine/logic/optimize_types.hpp"
#include "wengine/logic/optimize_barrier.hpp"
#include "wengine/logic/optimize_exact.hpp"
#include "wengine/logic/optimize_o2_dedup.hpp"
#include "wengine/logic/optimize_o2_emit.hpp"
#include "wengine/logic/optimize_passes.hpp"
#include "wengine/logic/types.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <span>

namespace wengine::logic {

namespace detail {

/**
 * @brief One dangling pin of the minimized build awaiting global tying
 *        (one block input per original variable).
 *
 */
 struct MiniDang {
  /** @brief Fresh gate slot holding the dangling pin. */
  Slot fresh = kNoDriver;
  /** @brief 0-based input index on the fresh gate. */
  std::uint8_t input = 0;
  /** @brief Original variable index (block-input ordinal in src). */
  std::uint8_t orig_j = 0;
};

/**
 * @brief Production minimized-build dangling pin.
 */

/**
 * @brief Append one dangling pin to the global-tying list.
 *
 * Capacity-free: the list itself is bounded by the n<=8/m<=8 caps.
 *
 * @param[out] list Dangling-pin buffer.
 * @param[out] n Pin count (in/out).
 * @param[in] cap Buffer capacity.
 * @param[in] fresh Fresh gate slot.
 * @param[in] input 0-based input index.
 * @param[in] orig_j Original variable index.
 * @return True on success; false when the buffer is full.
 */

inline bool mini_add_dang(MiniDang *list, std::size_t *n, std::size_t cap, Slot fresh, std::uint8_t input, std::uint8_t orig_j) noexcept {
  if (*n >= cap) {
    return false;
  }
  list[*n].fresh = fresh;
  list[*n].input = input;
  list[*n].orig_j = orig_j;
  ++(*n);
  return true;
}

/**
 * @brief Minimized two-level build from QM covers (one cover per
 *        output, greedy order).
 *
 * SOP for NAND (lit = x_j if bits=1 else ~x_j), POS for NOR (lit = x_j
 * if bits=0 else ~x_j): the two forms are duals, so a single builder
 * parameterized by target serves both — only the lit-polarity reading
 * differs. Shared input inverters, globally tied dangling pins (one
 * block input per variable).
 *
 * @param[out] out Fresh minimized netlist under construction.
 * @param[in] n Block-input count (<= 8).
 * @param[in] m Block-output count (<= 8).
 * @param[in] covers Per-output QM cover cubes (m rows of 8).
 * @param[in] ncover Per-output cover sizes.
 * @param[in] is_const Per-output constant flags.
 * @param[in] const_val Per-output constant values (meaningful for consts).
 * @param[in] target Conversion target (polarity reading).
 * @param[out] dangs Dangling-pin buffer for global tying.
 * @param[out] ndangs Dangling-pin count (in/out).
 * @param[in] dang_cap Dangling-pin capacity.
 * @return True on success; false on budget/tie failure.
 */

inline bool build_two_level(Netlist &out, std::uint8_t n, std::uint8_t m,
                             const QmCube *covers, const std::size_t *ncover,
                             const bool *is_const, const std::uint8_t *const_val,
                             OptTarget target, MiniDang *dangs, std::size_t *ndangs,
                             std::size_t dang_cap) noexcept {
  const GateKind u = universal_kind(target);
  // Shared C1 for constant-0 sinks (dedicated C0 per sink). U(0,0)=1 and
  // U(C1,C1)=0 hold in both vocabularies, so the pool is target-neutral.
  bool need_c1 = false;
  for (std::uint8_t o = 0; o < m; ++o) {
    if (is_const[o] && const_val[o] == 0) {
      need_c1 = true;
      break;
    }
  }
  Slot pool_c1 = kNoDriver;
  if (need_c1) {
    auto r = add_gate(out, u, 2);
    if (!r.ok()) {
      return false;
    }
    out.slots[r.value].ignored_mask = 0x03u;
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
        const bool positive = ((((c.bits >> jk) & 1u) != 0u) != (target == OptTarget::kNor));
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
          const bool positive = ((((c.bits >> j) & 1u) != 0u) != (target == OptTarget::kNor));
          if (!positive) {
            need_inv[j] = true;
          }
        }
      }
    }
    // Single-product single-negative outputs need a dedicated output
    // inverter (not shared, to keep outputs distinct); no shared needed.
  }
  Slot inv_slot[8];
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
        out.slots[r.value].ignored_mask = 0x03u;
      } else {
        if (pool_c1 == kNoDriver) {
          return false;
        }
        auto r = add_gate(out, u, 2);
        if (!r.ok()) {
          return false;
        }
        out.slots[r.value].drivers[0] = pool_c1;
        out.slots[r.value].drivers[1] = pool_c1;
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
        const bool positive = ((((c.bits >> jk) & 1u) != 0u) != (target == OptTarget::kNor));
        if (positive) {
          // f=x_j: buffer via shared inv + dedicated output gate.
          if (inv_slot[jk] == kNoDriver) {
            return false;
          }
          auto r = add_gate(out, u, 2);
          if (!r.ok()) {
            return false;
          }
          out.slots[r.value].drivers[0] = inv_slot[jk];
          out.slots[r.value].drivers[1] = inv_slot[jk];
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
        // f=AND lits: product gate + output inverter.
        auto rp = add_gate(out, u, width);
        if (!rp.ok()) {
          return false;
        }
        std::uint8_t idx = 0;
        for (std::uint8_t j = 0; j < n; ++j) {
          if (((c.mask >> j) & 1u) != 0u) {
            continue;
          }
          const bool positive = ((((c.bits >> j) & 1u) != 0u) != (target == OptTarget::kNor));
          if (positive) {
            if (!mini_add_dang(dangs, ndangs, dang_cap, rp.value, idx, j)) {
              return false;
            }
          } else {
            if (inv_slot[j] == kNoDriver) {
              return false;
            }
            out.slots[rp.value].drivers[idx] = inv_slot[j];
          }
          ++idx;
        }
        auto ro = add_gate(out, u, 2);
        if (!ro.ok()) {
          return false;
        }
        out.slots[ro.value].drivers[0] = rp.value;
        out.slots[ro.value].drivers[1] = rp.value;
      }
      continue;
    }
    // Multi-product: product gates + final NAND.
    Slot q_slot[8];
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
        const bool positive = ((((c.bits >> jk) & 1u) != 0u) != (target == OptTarget::kNor));
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
          const bool positive = ((((c.bits >> j) & 1u) != 0u) != (target == OptTarget::kNor));
          if (positive) {
            if (!mini_add_dang(dangs, ndangs, dang_cap, rp.value, idx, j)) {
              return false;
            }
          } else {
            if (inv_slot[j] == kNoDriver) {
              return false;
            }
            out.slots[rp.value].drivers[idx] = inv_slot[j];
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
        out.slots[rf.value].drivers[k] = q_slot[k];
      } else {
        if (!mini_add_dang(dangs, ndangs, dang_cap, rf.value, static_cast<std::uint8_t>(k), q_dang_j[k])) {
          return false;
        }
      }
    }
  }
  return true;
}

/**
 * @brief Attempt truth-table resynthesis for O2 (n<=8, m<=8, no hidden
 *        outputs).
 *
 * Builds a minimized two-level network (SOP for NAND, POS for NOR) with
 * shared inverters and globally tied inputs, verifies equivalence under
 * pin permutation, and leaves the result in out_min.
 *
 * @param[out] src Source netlist (recompiled for enumeration).
 * @param[in] target Conversion target.
 * @param[out] out_min Minimized netlist on success (caller-owned
 *             out-buffer; contents undefined on failure).
 * @param[in] resolved Caller-owned eval scratch (`slot_count(src) * kMaxInputs`
 *            values).
 * @param[in] gate_out Caller-owned eval scratch (pool-sized outputs).
 * @return True with out_min on success; false on any limit/budget/
 *         equivalence failure (caller falls back to structural, which is
 *         optimal for XOR-like parity). Deterministic.
 *         (No noexcept: topology compiles through the CompiledOrder seam,
 *         which grows workspace amortized.)
 * @note Opaque barrier: sequential nets and hidden outputs refuse loudly
 *       to the structural path.
 */

inline bool try_minimized_o2(Netlist &src, OptTarget target, Netlist &out_min,
                              std::span<LogicValue> resolved, std::span<LogicValue> gate_out) {
  const std::size_t n = block_input_count(src);
  const std::size_t m = block_output_count(src);
  if (n == 0 || n > 8 || m == 0 || m > 8) {
    return false;
  }
  for (Slot s = 0; s < slot_count(src); ++s) {
    if (!src.slots[s].live) {
      continue;
    }
    if (src.slots[s].ignore_output) {
      return false; // preserve hidden logic via structural path
    }
    if (is_frozen_kind(src.slots[s].kind)) {
      return false; // O2 refuses sequential loudly; let structural fail
    }
  }
  PinRef in_buf[8];
  PinRef out_buf[8];
  std::size_t in_n = 0;
  std::size_t out_n = 0;
  if (!block_interface(src, in_buf, 8, &in_n, out_buf, 8, &out_n).ok()) {
    return false;
  }
  if (in_n != n || out_n != m) {
    return false;
  }
  if (!topo::compile(src, EdgeFilter::kAll).ok()) {
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
    if (!solve(src, std::span<const LogicValue>(in_vals, n), std::span<LogicValue>(out_vals, m), nullptr, resolved, gate_out).ok()) {
      return false;
    }
    for (std::size_t o = 0; o < m; ++o) {
      tt_src[r * m + o] = (out_vals[o] == LogicValue::kHigh) ? 1u : 0u;
    }
  }
  // QM covers per output. SOP (NAND): minterms where output=1.
  // POS (NOR): maxterms where output=0 — the dual collection.
  const std::uint8_t collect = (target == OptTarget::kNand) ? 1u : 0u;
  QmCube covers[8 * 8];
  std::size_t ncover[8] = {};
  bool is_const[8] = {};
  std::uint8_t const_val[8] = {};
  for (std::size_t o = 0; o < m; ++o) {
    std::uint16_t terms[256];
    std::size_t nterms = 0;
    for (std::size_t r = 0; r < rows; ++r) {
      if (tt_src[r * m + o] == collect) {
        terms[nterms++] = static_cast<std::uint16_t>(r);
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
  Netlist &built = out_min;
  MiniDang dangs[1024];
  std::size_t ndangs = 0;
  bool built_ok = build_two_level(built, static_cast<std::uint8_t>(n), static_cast<std::uint8_t>(m), covers, ncover, is_const, const_val, target, dangs, &ndangs, 1024);
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
      if (cur.slot >= slot_count(built) || !built.slots[cur.slot].live) {
        return false;
      }
      const Gate &g = built.slots[cur.slot];
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
  // PoolStore (016): the minimized build wrote tables directly — one seam
  // rebuilds every derived store before the validation below (which reads
  // the chains).
  pool_store::rebuild_all(built);
  recount_fanout(built);
  built.dirty = true;
  built.topo_order.clear();
  if (!topo::compile(built, EdgeFilter::kAll).ok()) {
    return false;
  }
  // Interface + proj mapping (minimized inputs -> original inputs).
  PinRef min_in[8];
  PinRef min_out[8];
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
    if (!solve(built, std::span<const LogicValue>(min_in_vals, min_in_n), std::span<LogicValue>(min_out_vals, m), nullptr, resolved, gate_out).ok()) {
      return false;
    }
    for (std::size_t o = 0; o < m; ++o) {
      if (((min_out_vals[o] == LogicValue::kHigh) ? 1u : 0u) != tt_src[r * m + o]) {
        return false; // not equivalent; fallback to structural
      }
    }
  }
  return true;
}


} // namespace detail

} // namespace wengine::logic
