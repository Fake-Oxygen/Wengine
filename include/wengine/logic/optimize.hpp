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
#include "wengine/logic/optimize_o2_build.hpp"
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
    if (!topo::rebuild(nl, has_sequential_gates(nl) ? EdgeFilter::kCombOnly : EdgeFilter::kAll).ok()) {
      return core::Result<Netlist>::failure("optimize: topology error", core::StatusCode::kInternal);
    }
    bool feeds_seq[kMaxGates] = {};
    detail::compute_feeds_seq(nl, feeds_seq);
    bool changed = false;
    if (detail::bypass_pass(nl, feeds_seq)) {
      changed = true;
    }
    if (!topo::rebuild(nl, has_sequential_gates(nl) ? EdgeFilter::kCombOnly : EdgeFilter::kAll).ok()) {
      return core::Result<Netlist>::failure("optimize: topology error", core::StatusCode::kInternal);
    }
    detail::compute_feeds_seq(nl, feeds_seq);
    if (want_fuse && detail::fusion_sweep(nl, feeds_seq)) {
      changed = true;
    }
    if (!topo::rebuild(nl, has_sequential_gates(nl) ? EdgeFilter::kCombOnly : EdgeFilter::kAll).ok()) {
      return core::Result<Netlist>::failure("optimize: topology error", core::StatusCode::kInternal);
    }
    if (detail::const_fold_pass(nl, roots)) {
      changed = true;
    }
    if (!topo::rebuild(nl, has_sequential_gates(nl) ? EdgeFilter::kCombOnly : EdgeFilter::kAll).ok()) {
      return core::Result<Netlist>::failure("optimize: topology error", core::StatusCode::kInternal);
    }
    if (detail::exact_simplify_pass(nl)) {
      changed = true;
    }
    if (!topo::rebuild(nl, has_sequential_gates(nl) ? EdgeFilter::kCombOnly : EdgeFilter::kAll).ok()) {
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
    if (!topo::rebuild(nl, has_sequential_gates(nl) ? EdgeFilter::kCombOnly : EdgeFilter::kAll).ok()) {
      return core::Result<Netlist>::failure("optimize: topology error", core::StatusCode::kInternal);
    }
    return core::Result<Netlist>::success(nl);
  }

  // --- O2: fresh universal build (phases live in optimize_o2_build.hpp;
  // optimize() only orchestrates) ---------------------------------------
  bool reach[kMaxGates] = {};
  detail::collect_reachable(nl, reach);

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
  const detail::PoolNeeds pool_needs = detail::size_pool_needs(nl, reach, clabel);
  {
    const detail::BuildError perr = detail::emit_pool(out, ctx, opts.target, pool_needs);
    if (perr != detail::BuildError::kNone) {
      return core::Result<Netlist>::failure(detail::build_error_message(perr), core::StatusCode::kInternal);
    }
  }

  // Creation pass in original slot order (fresh slots follow it).
  {
    const detail::BuildError cerr =
        detail::create_networks(nl, reach, clabel, ctx, emit_map, opts.target);
    if (cerr != detail::BuildError::kNone) {
      return core::Result<Netlist>::failure(detail::build_error_message(cerr), core::StatusCode::kInternal);
    }
  }

  // Wiring pass from the complete map (wiring-time tying for sequential
  // nets only; pure-combinational output stays bit-identical).
  {
    const bool structural_tie_duplicates = has_sequential_gates(nl);
    const detail::BuildError werr =
        detail::wire_pending(out, ctx, pend, npend, emit_map, structural_tie_duplicates);
    if (werr != detail::BuildError::kNone) {
      return core::Result<Netlist>::failure(detail::build_error_message(werr), core::StatusCode::kInternal);
    }
  }
  detail::restore_masks(nl, out, reach, emit_map, opts.target);
  // Vocabulary-preserving post-cleanup (dedup + double-inverter cancel +
  // DCE to fixpoint) and minimized-vs-structural picking.
  {
    const detail::BuildError perr = detail::post_cleanup(out, nl, reach, emit_map, opts.target);
    if (perr != detail::BuildError::kNone) {
      return core::Result<Netlist>::failure(detail::build_error_message(perr),
                                            core::StatusCode::kInternal);
    }
  }
  return detail::finish_o2(out, nl, opts.target);
}

} // namespace wengine::logic
