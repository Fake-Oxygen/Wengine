#pragma once

/**
 * @file
 * O2 single-kind emitter: the conversion stage of the O2 fresh build
 * inside the optimize stage of the logic pipeline (netlist parse ->
 * state -> eval -> layout/topo -> optimize). Converts one reachable
 * source gate into fresh target-kind (NAND/NOR) gates with deferred
 * wiring: creation runs in original slot order (fresh slots follow it,
 * so block input/output order is preserved positionally); wiring applies
 * afterwards from the complete driver map. Dangling pins that derive
 * from the same source tie leader are tied at wiring time (one variable
 * per pin), so sequential O2 toggles converge instead of multiplying
 * pins and gates.
 *
 * What it preserves: truth-equivalence gate by gate (structural De
 * Morgan duals; XOR cascades; verbatim sequential carry). Cost class:
 * linear in reachable gates times fan-in, plus the XOR cascade blocks.
 *
 * Opaque barrier: sequential gates travel via emit_seq_verbatim (same
 * kind, pin count, and flags; ignored dangling pins stay ignored and
 * dangling, never pool-routed), never through conversion.
 *
 * Dynamic pool (015): the emitter works over Netlist with pool-owned
 * sizing (slot_count bounds every scan, never kMaxGates);
 * PendingWire/ConvertCtx ride beside it. The O2 orchestration in
 * optimize_o2_build.hpp instantiates it. Internal cross-calls are bare
 * (deduction over Netlist, never explicit capacity arguments).
 *
 * Dependency rules: header-only, allocation-free, exception-free;
 * depends on netlist.hpp, optimize_types.hpp, optimize_barrier.hpp,
 * optimize_exact.hpp, optimize_passes.hpp, and types.hpp. Logic is a
 * standalone domain: this header must never depend on CLI types.
 * Consumers: optimize_o2_build.hpp (create_networks) and the
 * test_logic* suite.
 */

#include "wengine/core/status.hpp"
#include "wengine/logic/netlist.hpp"
#include "wengine/logic/optimize_types.hpp"
#include "wengine/logic/optimize_barrier.hpp"
#include "wengine/logic/optimize_exact.hpp"
#include "wengine/logic/optimize_passes.hpp"
#include "wengine/logic/types.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace wengine::logic {

namespace detail {

/**
 * @brief Deferred-wiring conversion context: creation runs in original
 *        slot order (fresh slots follow it); wiring applies afterwards
 *        from the complete driver map.
 *
 * Dynamic pool (015): every entry works over Netlist.
 */
/** @brief Sentinel for "no source tie leader" (wired or pool-routed pins). */
inline constexpr std::size_t kNoLeader = kMaxGates * kMaxInputs;

/**
 * @brief One deferred wire: a fresh sink input resolved in the wiring
 *        pass from the complete emit map (or the shared constant pool).
 */
struct PendingWire {
  /** @brief Fresh sink slot. */
  Slot fresh = kNoDriver;
  /** @brief Original driver slot (kNoDriver means dangling). */
  Slot orig = kNoDriver; // original driver slot (kNoDriver = dangling)
  /** @brief 0-based fresh sink input index. */
  std::uint8_t input = 0;
  /** @brief True routes the shared C0 pool gate (ignored original input). */
  bool use_pool = false; // ignored original input: wire the shared C0 pool gate
  /** @brief Q/NQ polarity of the original sink pin (verbatim seq carry). */
  std::uint8_t inv = 0;  // Q/NQ polarity of the original sink pin (verbatim seq carry)
  /** @brief Source tie leader for dangling pins (kNoLeader when wired). */
  std::size_t src_leader = kNoLeader; // source tie leader for dangling pins
};

/**
 * @brief Mutable O2 emission state: output net, target vocabulary,
 *        orig-to-fresh driver map, pending wires, and shared pools.
 */
struct ConvertCtx {
  /** @brief Fresh output netlist under construction. */
  Netlist *out = nullptr;
  /** @brief Single-kind target (NAND or NOR). */
  OptTarget target = OptTarget::kNand;
  /** @brief Orig slot to network-output fresh slot map. */
  Slot *emit_map = nullptr; // orig slot -> network output fresh slot
  /** @brief Pending-wire buffer (deferred wiring pass, grows geometrically). */
  std::vector<PendingWire> *pend = nullptr;
  /** @brief Shared constant-0 pool gate (kNoDriver when unneeded). */
  Slot pool_c0 = kNoDriver;
  /** @brief Shared constant-1 pool gate (kNoDriver when unneeded). */
  Slot pool_c1 = kNoDriver;
  /** @brief Latched failure (budget/overflow); checked by the caller. */
  bool failed = false;
};

/**
 * @brief Allocate one fresh gate in the output net, latching ctx.failed
 *        on budget exhaustion.
 *
 * @param[out] ctx Emission state.
 * @param[in] kind Kind to create.
 * @param[in] n Fan-in.
 * @return Fresh slot, or kNoDriver on failure.
 */
inline Slot fresh_gate(ConvertCtx &ctx, GateKind kind, std::uint8_t n) noexcept {
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

/**
 * @brief Record one network input: original input orig_i of orig_s
 *        feeding fresh at input_idx.
 *
 * Ignored pins route to the shared C0 pool; dangling block inputs pass
 * through as dangling (mask stays clear) with the source tie leader
 * carried for wiring-time tying.
 *
 * @param[out] ctx Emission state receiving the pending wire.
 * @param[in] src Source netlist (masks, drivers, tie leaders).
 * @param[in] orig_s Original source slot.
 * @param[in] orig_i Original source input index.
 * @param[in] fresh Fresh sink slot.
 * @param[in] input_idx Fresh sink input index.
 */
inline void record_input(ConvertCtx &ctx, const Netlist &src, Slot orig_s, std::uint8_t orig_i, Slot fresh, std::uint8_t input_idx) noexcept {
  if (ctx.failed || ctx.pend == nullptr) {
    ctx.failed = true;
    return;
  }
  const Gate &g = src.slots[orig_s];
  PendingWire w{};
  w.fresh = fresh;
  w.input = input_idx;
  w.src_leader = tie_leader_flat(src, tie_flat(orig_s, orig_i));
  if ((g.ignored_mask & static_cast<std::uint8_t>(1u << orig_i)) != 0) {
    w.use_pool = true;
    w.orig = ctx.pool_c0;
  } else {
    w.use_pool = false;
    w.orig = g.drivers[orig_i];
  }
  ctx.pend->push_back(w);
}

/**
 * @brief Universal kind for a conversion target.
 *
 * Capacity-free: kinds never scale.
 *
 * @param[in] t Conversion target.
 * @return kNand for kNand targets, kNor otherwise.
 */
inline GateKind universal_kind(OptTarget t) noexcept {
  return (t == OptTarget::kNand) ? GateKind::kNand : GateKind::kNor;
}

/**
 * @brief Verbatim carry of one sequential gate into the O2 fresh build
 *        (opaque barrier): same kind, pin count, and flags.
 *
 * Ignored dangling pins stay ignored and dangling (never pool-routed);
 * wired inputs resolve through the emit map in the deferred wiring pass
 * with per-pin Q/NQ polarity preserved. Fresh slot/counter discipline
 * matches add_gate.
 *
 * @param[out] ctx Emission state.
 * @param[in] src Source netlist.
 * @param[in] s Sequential slot to carry.
 * @return Fresh slot, or kNoDriver on budget exhaustion.
 */
inline Slot emit_seq_verbatim(ConvertCtx &ctx, const Netlist &src, Slot s) noexcept {
  const Gate &g = src.slots[s];
  const Slot o = fresh_gate(ctx, g.kind, g.input_count);
  if (o == kNoDriver || ctx.failed) {
    ctx.failed = true;
    return kNoDriver;
  }
  Gate &fresh = ctx.out->slots[o];
  fresh.ignored_mask = g.ignored_mask;
  fresh.ignore_output = g.ignore_output;
  fresh.ignore_nq_output = g.ignore_nq_output;
  fresh.propagation_delay_ps = 0; // fresh build reshapes the network (comb emit drops it too)
  for (std::uint8_t i = 0; i < g.input_count; ++i) {
    if (g.drivers[i] == kNoDriver) {
      continue; // stays dangling (ignored or block input); add_gate zeroed the polarity
    }
    if (ctx.failed || ctx.pend == nullptr) {
      ctx.failed = true;
      return kNoDriver;
    }
    PendingWire w{};
    w.fresh = o;
    w.input = i;
    w.use_pool = false;
    w.orig = g.drivers[i];
    w.inv = src.driver_inv[static_cast<std::size_t>(s) * kMaxInputs + i];
    w.src_leader = kNoLeader; // wired: no tying
    ctx.pend->push_back(w);
  }
  return o;
}

/**
 * @brief NOT(a) in the target vocabulary: one universal gate over (a, a).
 *
 * @param[out] ctx Emission state.
 * @param[in] src Source netlist (input recording).
 * @param[in] orig_s Original source slot.
 * @param[in] orig_i Original source input index.
 * @return Fresh inverter slot, or kNoDriver on budget exhaustion.
 */
inline Slot emit_not(ConvertCtx &ctx, const Netlist &src, Slot orig_s, std::uint8_t orig_i) noexcept {
  const GateKind u = universal_kind(ctx.target);
  const Slot t = fresh_gate(ctx, u, 2);
  if (t == kNoDriver) {
    return kNoDriver;
  }
  record_input(ctx, src, orig_s, orig_i, t, 0);
  // Both inputs read the same source: record twice (duplicate drivers are
  // legal; fanout counts each edge, block-output detection only needs != 0).
  record_input(ctx, src, orig_s, orig_i, t, 1);
  return t;
}

/**
 * @brief NOT over an already-emitted fresh driver (internal edge, wired
 *        at once).
 *
 * @param[out] ctx Emission state.
 * @param[in] a Fresh driver slot to invert.
 * @return Fresh inverter slot, or kNoDriver on budget exhaustion.
 */
inline Slot emit_not_fresh(ConvertCtx &ctx, Slot a) noexcept {
  const GateKind u = universal_kind(ctx.target);
  const Slot o = fresh_gate(ctx, u, 2);
  if (o == kNoDriver) {
    return kNoDriver;
  }
  ctx.out->slots[o].drivers[0] = a;
  ctx.out->slots[o].drivers[1] = a;
  return o;
}

/**
 * @brief Convert one reachable source gate into fresh target-kind
 *        gates: natives pass through, AND/OR duals via De Morgan with
 *        complemented input inverters, XOR/XNOR via odd-parity 2-input
 *        cascades (4-gate NAND blocks, 5-gate NOR blocks).
 *
 * Sequential kinds never convert here (opaque barrier: they travel via
 * emit_seq_verbatim; reaching this switch is a defensive refusal).
 *
 * @param[out] ctx Emission state.
 * @param[in] src Source netlist.
 * @param[in] s Reachable source slot.
 * @return Network output fresh slot (kNoDriver on budget exhaustion).
 */
inline Slot emit_gate(ConvertCtx &ctx, const Netlist &src, Slot s) noexcept {
  const Gate &g = src.slots[s];
  const GateKind u = universal_kind(ctx.target);
  const std::uint8_t n = g.input_count;
  const bool is_nand_target = (ctx.target == OptTarget::kNand);
  auto single = [&]() noexcept -> Slot {
    const Slot o = fresh_gate(ctx, u, n);
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
    const Slot o = fresh_gate(ctx, u, 2);
    if (o == kNoDriver) {
      return kNoDriver;
    }
    record_input(ctx, src, s, 0, o, 0);
    record_input(ctx, src, s, 0, o, 1);
    return o;
  }
  case GateKind::kBuf: {
    // BUF(a) = NOT(NOT(a)).
    const Slot t = emit_not(ctx, src, s, 0);
    if (t == kNoDriver) {
      return kNoDriver;
    }
    return emit_not_fresh(ctx, t);
  }
  case GateKind::kTbuf:
    // 012: no verbatim-carry O2 emitter for buffers (a TBUF cannot be
    // expressed in the universal-gate vocabulary without losing the
    // release-to-Z shape). Unreachable: O2 refuses live-buffer netlists
    // at entry (optimize.hpp) — defensive refusal, keeps -Wswitch
    // clean, zero per-word cost.
    return kNoDriver;
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
      const Slot inner = single();
      if (inner == kNoDriver) {
        return kNoDriver;
      }
      return emit_not_fresh(ctx, inner);
    }
    const bool complemented = (g.kind == GateKind::kNor && is_nand_target) || (g.kind == GateKind::kNand && !is_nand_target);
    {
      const Slot mid = fresh_gate(ctx, u, n);
      if (mid == kNoDriver) {
        return kNoDriver;
      }
      for (std::uint8_t i = 0; i < n; ++i) {
        const Slot v = emit_not(ctx, src, s, i);
        if (v == kNoDriver) {
          return kNoDriver;
        }
        ctx.out->slots[mid].drivers[i] = v;
      }
      if (!complemented) {
        return mid;
      }
      const Slot o = fresh_gate(ctx, u, 2);
      if (o == kNoDriver) {
        return kNoDriver;
      }
      ctx.out->slots[o].drivers[0] = mid;
      ctx.out->slots[o].drivers[1] = mid;
      return o;
    }
  }
  case GateKind::kXor:
  case GateKind::kXnor: {
    // Odd-parity cascade of 2-input XOR blocks.
    // NAND block (4 gates): t1=N(a,b); t2=N(a,t1); t3=N(b,t1); o=N(t2,t3).
    // NOR block (5 gates): t1=NOR(a,b); na=~a; nb=~b; t4=NOR(na,nb)=a&b;
    // o=NOR(t1,t4) = (a|b)&~(a&b). Each block input is either an existing
    // fresh driver (wired at once) or an original source input (recorded
    // for the deferred wiring pass).
    struct Feed {
      bool is_fresh = false;
      Slot fresh_slot = kNoDriver;
      std::uint8_t orig_idx = 0;
    };
    auto drive = [&](Slot gate, std::uint8_t idx, const Feed &f) noexcept {
      if (f.is_fresh) {
        ctx.out->slots[gate].drivers[idx] = f.fresh_slot;
      } else {
        record_input(ctx, src, s, f.orig_idx, gate, idx);
      }
    };
    auto xor2 = [&](const Feed &a, const Feed &b) noexcept -> Slot {
      if (is_nand_target) {
        const Slot t1 = fresh_gate(ctx, u, 2);
        const Slot t2 = fresh_gate(ctx, u, 2);
        const Slot t3 = fresh_gate(ctx, u, 2);
        const Slot o = fresh_gate(ctx, u, 2);
        if (t1 == kNoDriver || t2 == kNoDriver || t3 == kNoDriver || o == kNoDriver) {
          return kNoDriver;
        }
        drive(t1, 0, a);
        drive(t1, 1, b);
        drive(t2, 0, a);
        ctx.out->slots[t2].drivers[1] = t1;
        drive(t3, 0, b);
        ctx.out->slots[t3].drivers[1] = t1;
        ctx.out->slots[o].drivers[0] = t2;
        ctx.out->slots[o].drivers[1] = t3;
        return o;
      }
      const Slot t1 = fresh_gate(ctx, u, 2);
      const Slot na = fresh_gate(ctx, u, 2);
      const Slot nb = fresh_gate(ctx, u, 2);
      const Slot t4 = fresh_gate(ctx, u, 2);
      const Slot o = fresh_gate(ctx, u, 2);
      if (t1 == kNoDriver || na == kNoDriver || nb == kNoDriver || t4 == kNoDriver || o == kNoDriver) {
        return kNoDriver;
      }
      drive(t1, 0, a);
      drive(t1, 1, b);
      drive(na, 0, a);
      drive(na, 1, a);
      drive(nb, 0, b);
      drive(nb, 1, b);
      ctx.out->slots[t4].drivers[0] = na;
      ctx.out->slots[t4].drivers[1] = nb;
      ctx.out->slots[o].drivers[0] = t1;
      ctx.out->slots[o].drivers[1] = t4;
      return o;
    };
    const Feed r0{false, kNoDriver, 0};
    const Feed r1{false, kNoDriver, 1};
    Slot acc = xor2(r0, r1);
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
  case GateKind::kJk:
  case GateKind::kT:
  case GateKind::kSrlatch:
    return kNoDriver; // opaque barrier: sequential gates travel via emit_seq_verbatim,
                            // never through conversion; defensive refusal.
  }
  return kNoDriver; // unreachable (all kinds handled); defensive
}


} // namespace detail

} // namespace wengine::logic
