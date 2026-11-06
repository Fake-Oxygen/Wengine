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
//   any pass, ignored outputs included) plus every live sequential gate:
//   state readback observes flops outside the block interface, so lack of
//   fanout never makes a state element dead (opaque barrier). Optimization
//   orphans (bypassed inners, fused-away drivers, dropped constant gates)
//   are removed, but user logic feeding an ignored output is never deleted:
//   ignore means "hidden", not "dead", and the optimizer must not destroy
//   circuitry the user only hid.
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
#include "wengine/logic/optimize_barrier.hpp"
#include "wengine/logic/optimize_exact.hpp"
#include "wengine/logic/optimize_o2_dedup.hpp"
#include "wengine/logic/optimize_o2_emit.hpp"
#include "wengine/logic/optimize_o2_min.hpp"
#include "wengine/logic/optimize_passes.hpp"
#include "wengine/logic/optimize_types.hpp"
#include "wengine/logic/seq_state.hpp"
#include "wengine/logic/types.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace wengine::logic {


// Pure optimizer: `in` is never modified (all surgery happens on the copy).
[[nodiscard]] inline core::Result<Netlist> optimize(const Netlist &in, OptimizeOptions opts) noexcept {
  Netlist nl = in;
  nl.dirty = true;
  nl.topo_size = 0;
  detail::recount_fanout(nl);

  bool roots[kMaxGates] = {};
  detail::collect_barrier_roots(nl, roots);

  const bool want_fuse = (opts.level == OptLevel::kO1) || (opts.level == OptLevel::kO2);
  constexpr std::size_t kMaxIters = 2 * kMaxGates + 16;
  std::size_t iter = 0;
  while (true) {
    if (iter >= kMaxIters) {
      return core::Result<Netlist>::failure("optimize: internal iteration bound", core::StatusCode::kInternal);
    }
    ++iter;
    if (!detail::recompile(nl)) {
      return core::Result<Netlist>::failure("optimize: topology error", core::StatusCode::kInternal);
    }
    bool feeds_seq[kMaxGates] = {};
    detail::compute_feeds_seq(nl, feeds_seq);
    bool changed = false;
    if (detail::bypass_pass(nl, feeds_seq)) {
      changed = true;
    }
    if (!detail::recompile(nl)) {
      return core::Result<Netlist>::failure("optimize: topology error", core::StatusCode::kInternal);
    }
    detail::compute_feeds_seq(nl, feeds_seq);
    if (want_fuse && detail::fusion_sweep(nl, feeds_seq)) {
      changed = true;
    }
    if (!detail::recompile(nl)) {
      return core::Result<Netlist>::failure("optimize: topology error", core::StatusCode::kInternal);
    }
    if (detail::const_fold_pass(nl, roots)) {
      changed = true;
    }
    if (!detail::recompile(nl)) {
      return core::Result<Netlist>::failure("optimize: topology error", core::StatusCode::kInternal);
    }
    if (detail::exact_simplify_pass(nl)) {
      changed = true;
    }
    if (!detail::recompile(nl)) {
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
    if (!detail::recompile(nl)) {
      return core::Result<Netlist>::failure("optimize: topology error", core::StatusCode::kInternal);
    }
    return core::Result<Netlist>::success(nl);
  }

  // --- O2: fresh universal build -----------------------------------------
  // Reachable set (reverse DFS from every barrier root: original sinks
  // plus every live sequential gate so closed state clusters with no
  // block-output path still carry through; driver cones of kept flops
  // follow via the DFS below).
  bool reach[kMaxGates] = {};
  {
    std::uint16_t stack[kMaxGates];
    std::size_t top = 0;
    for (std::uint16_t s = 0; s < kMaxGates; ++s) {
      if (nl.gates[s].live && detail::is_barrier_root(nl, s)) {
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
  // outputs); sequential gates carry verbatim (opaque barrier); all other
  // gates emit via De Morgan as before.
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    if (!nl.gates[s].live || !reach[s]) {
      continue;
    }
    if (detail::is_frozen_kind(nl.gates[s].kind)) {
      const std::uint16_t o = detail::emit_seq_verbatim(ctx, nl, s);
      if (o == kNoDriver || ctx.failed) {
        return core::Result<Netlist>::failure("optimize: gate budget exceeded (kMaxGates)", core::StatusCode::kInternal);
      }
      emit_map[s] = o;
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

  // Wiring pass from the complete map, with wiring-time tying for
  // sequential nets only: dangling pins derived from the same source tie
  // leader share one tie class, so one variable stays one variable and
  // sequential nand<->nor toggles converge instead of multiplying pins
  // and gates. Pure-combinational output stays bit-identical (no tying).
  const bool structural_tie_duplicates = has_sequential_gates(nl);
  std::uint16_t tie_first_fresh[kMaxGates * kMaxInputs];
  std::uint8_t tie_first_input[kMaxGates * kMaxInputs];
  for (std::size_t i = 0; i < kMaxGates * kMaxInputs; ++i) {
    tie_first_fresh[i] = kNoDriver;
    tie_first_input[i] = 0;
  }
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
    detail::wire_fresh_with_polarity(out, w.fresh, w.input, drv, w.inv);
    if (!structural_tie_duplicates) {
      continue;
    }
    if (drv != kNoDriver || w.use_pool || w.src_leader >= kMaxGates * kMaxInputs) {
      continue; // wired or pool-routed: nothing to tie
    }
    if (out.gates[w.fresh].drivers[w.input] != kNoDriver) {
      continue; // defensive: only dangling pins participate
    }
    const std::size_t leader = w.src_leader;
    if (tie_first_fresh[leader] == kNoDriver) {
      tie_first_fresh[leader] = w.fresh;
      tie_first_input[leader] = w.input;
    } else {
      PinRef dst{.slot = w.fresh, .is_output = false, .input = w.input};
      PinRef src{.slot = tie_first_fresh[leader], .is_output = false, .input = tie_first_input[leader]};
      (void)tie_pins(out, dst, src);
    }
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
  // orphans (roots = original fresh outputs, hidden included, plus every
  // verbatim-carried sequential gate — the same opaque-barrier rule as the
  // O0/O1 fixpoint roots — so orphans that become sinks are removed, not
  // kept as extra outputs).
  {
    const GateKind u = detail::universal_kind(opts.target);
    bool post_roots[kMaxGates] = {};
    for (std::uint16_t s = 0; s < kMaxGates; ++s) {
      if (!nl.gates[s].live || !reach[s]) {
        continue;
      }
      if (detail::is_barrier_root(nl, s)) {
        const std::uint16_t o = emit_map[s];
        if (o != kNoDriver && o < kMaxGates) {
          post_roots[o] = true;
        }
      }
    }
    const bool seq_net = has_sequential_gates(nl);
    bool post_feeds[kMaxGates] = {};
    constexpr std::size_t kPostIters = 2 * kMaxGates + 16;
    for (std::size_t it = 0; it < kPostIters; ++it) {
      bool pchanged = false;
      detail::compute_feeds_seq(out, post_feeds);
      if (seq_net) {
        if (detail::dedup_universal_tied_pass(out, u, post_feeds)) {
          pchanged = true;
        }
      } else if (detail::dedup_universal_pass(out, u, post_feeds)) {
        pchanged = true;
      }
      if (!detail::recompile(out)) {
        return core::Result<Netlist>::failure("optimize: topology error", core::StatusCode::kInternal);
      }
      detail::compute_feeds_seq(out, post_feeds);
      if (detail::double_inv_universal_pass(out, u, post_feeds)) {
        pchanged = true;
      }
      if (!detail::recompile(out)) {
        return core::Result<Netlist>::failure("optimize: topology error", core::StatusCode::kInternal);
      }
      if (detail::dce_pass(out, post_roots)) {
        pchanged = true;
      }
      if (!detail::recompile(out)) {
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
  if (!detail::recompile(out)) {
    return core::Result<Netlist>::failure("optimize: topology error", core::StatusCode::kInternal);
  }
  return core::Result<Netlist>::success(out);
}

} // namespace wengine::logic
