#pragma once

#include "wengine/core/status.hpp"
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

namespace wengine::logic {

namespace detail {

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
    if (is_frozen_kind(src.gates[s].kind)) {
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

} // namespace wengine::logic
