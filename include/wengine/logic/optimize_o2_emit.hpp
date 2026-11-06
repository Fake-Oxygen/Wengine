#pragma once

#include "wengine/core/status.hpp"
#include "wengine/logic/netlist.hpp"
#include "wengine/logic/optimize_types.hpp"
#include "wengine/logic/optimize_barrier.hpp"
#include "wengine/logic/optimize_exact.hpp"
#include "wengine/logic/optimize_passes.hpp"
#include "wengine/logic/types.hpp"

#include <cstddef>
#include <cstdint>

namespace wengine::logic {

namespace detail {

// --- O2 fresh-build conversion -------------------------------------------
// Deferred wiring: creation runs in original slot order (fresh slots follow
// it, so block input/output order is preserved positionally); wiring
// applies afterwards from the complete driver map. Dangling pins that
// derive from the same source tie leader are tied at wiring time (one
// variable per pin), so sequential O2 toggles converge instead of
// multiplying pins and gates.
inline constexpr std::size_t kNoLeader = kMaxGates * kMaxInputs;

struct PendingWire {
  std::uint16_t fresh = kNoDriver;
  std::uint16_t orig = kNoDriver; // original driver slot (kNoDriver = dangling)
  std::uint8_t input = 0;
  bool use_pool = false; // ignored original input: wire the shared C0 pool gate
  std::uint8_t inv = 0;  // Q/NQ polarity of the original sink pin (verbatim seq carry)
  std::size_t src_leader = kNoLeader; // source tie leader for dangling pins
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
// dangling block inputs pass through as dangling (mask stays clear) with
// the source tie leader carried for wiring-time tying.
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
  w.src_leader = tie_leader_flat(src, tie_flat(orig_s, orig_i));
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

// Verbatim carry of one sequential gate into the O2 fresh build (opaque
// barrier): same kind, pin count, and flags; ignored dangling pins stay
// ignored and dangling (never pool-routed); wired inputs resolve through
// the emit map in the deferred wiring pass with per-pin Q/NQ polarity
// preserved. Fresh slot/counter discipline matches add_gate.
inline std::uint16_t emit_seq_verbatim(ConvertCtx &ctx, const Netlist &src, std::uint16_t s) noexcept {
  const Gate &g = src.gates[s];
  const std::uint16_t o = fresh_gate(ctx, g.kind, g.input_count);
  if (o == kNoDriver || ctx.failed) {
    ctx.failed = true;
    return kNoDriver;
  }
  Gate &fresh = ctx.out->gates[o];
  fresh.ignored_mask = g.ignored_mask;
  fresh.ignore_output = g.ignore_output;
  fresh.ignore_nq_output = g.ignore_nq_output;
  fresh.propagation_delay_ps = 0; // fresh build reshapes the network (comb emit drops it too)
  for (std::uint8_t i = 0; i < g.input_count; ++i) {
    if (g.drivers[i] == kNoDriver) {
      continue; // stays dangling (ignored or block input); add_gate zeroed the polarity
    }
    if (*ctx.npend >= ctx.pend_cap) {
      ctx.failed = true; // defensive: bounded by reachable*8 by construction
      return kNoDriver;
    }
    PendingWire w{};
    w.fresh = o;
    w.input = i;
    w.use_pool = false;
    w.orig = g.drivers[i];
    w.inv = src.driver_inv[static_cast<std::size_t>(s) * kMaxInputs + i];
    w.src_leader = kNoLeader; // wired: no tying
    ctx.pend[*ctx.npend] = w;
    ++(*ctx.npend);
  }
  return o;
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
