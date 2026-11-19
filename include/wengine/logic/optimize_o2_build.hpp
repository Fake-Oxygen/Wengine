/**
 * @file
 * O2 fresh-build phases (C2 deepening): the six stages of the NAND/NOR universal build
 * as small testable modules, with optimize() left as the orchestrator.
 * Move-only split: no policy, gate-count, or interface change —
 * structural De Morgan emission, pool sharing, wiring-time tying,
 * vocabulary-preserving cleanup, and minimized-vs-structural picking
 * all behave as before. Phases in order: size_pool_needs, emit_pool,
 * create_networks, wire_pending, restore_masks, post_cleanup,
 * finish_o2.
 *
 * What the build preserves: truth-equivalence with the O0/O1-fixed-point
 * net (structural duals plus the minimized alternative); output networks
 * follow original slot order so truth checks map positionally. Cost
 * class: linear creation and wiring plus the fixpoint post-cleanup
 * (bounded iterations) and the optional QM resynthesis for small
 * blocks.
 *
 * Opaque barrier: sequential gates ride through every phase verbatim
 * (same kind, pin count, flags, Q/NQ polarity); only the combinational
 * glue converts. Same-source dangling pins duplicated per use are
 * re-tied (one variable per pin) and tied duplicates merge in cleanup,
 * so repeated NAND/NOR toggles converge instead of multiplying gates
 * and pins.
 *
 * Dependency rules: header-only, allocation-free, exception-free
 * (finish_o2 returns Result only for the budget/topology failures the
 * caller already reports loudly). Depends on netlist.hpp,
 * optimize_barrier.hpp, optimize_exact.hpp, the O2
 * dedup/emit/min headers, optimize_passes.hpp, optimize_types.hpp,
 * seq_state.hpp, topo.hpp, and types.hpp. Logic is a standalone domain:
 * this header must never depend on CLI types. Consumers: optimize.hpp
 * (the O2 path) and the test_logic* suite.
 */

#pragma once

#include "wengine/core/status.hpp"
#include "wengine/logic/netlist.hpp"
#include "wengine/logic/optimize_barrier.hpp"
#include "wengine/logic/optimize_exact.hpp"
#include "wengine/logic/optimize_o2_dedup.hpp"
#include "wengine/logic/optimize_o2_emit.hpp"
#include "wengine/logic/optimize_o2_min.hpp"
#include "wengine/logic/optimize_passes.hpp"
#include "wengine/logic/optimize_types.hpp"
#include "wengine/logic/seq_state.hpp"
#include "wengine/logic/topo.hpp"
#include "wengine/logic/types.hpp"

#include <cstddef>
#include <cstdint>

namespace wengine::logic {

namespace detail {

/**
 * @brief Failure modes of the fresh build (mapped to loud messages
 *        once, in optimize(), so every phase stays a plain predicate).
 */
enum class BuildError : std::uint8_t {
  /** @brief No failure. */
  kNone = 0,
  /** @brief Gate budget exceeded (kMaxGates). */
  kBudget = 1, // gate budget exceeded (kMaxGates)
  /** @brief Internal wiring error (emit-map gap, bad indices). */
  kWiring = 2, // internal wiring error (emit-map gap, bad indices)
  /** @brief Topology error inside post-cleanup. */
  kTopo = 3,   // topology error inside post-cleanup
  /** @brief Internal iteration bound. */
  kIter = 4,   // internal iteration bound
};

/**
 * @brief Which shared constant-pool gates the fresh build needs.
 */
struct PoolNeeds {
  /** @brief True when the shared constant-1 pool gate is needed. */
  bool c1 = false;
  /** @brief True when the shared constant-0 pool gate is needed. */
  bool c0 = false;
};

/**
 * @brief Constant-aware shared-pool sizing: structural constant labels
 *        (ignored pins tie to 0) decide whether the fresh build needs
 *        the shared C1 pool (U(0,0) = 1) and the shared C0 pool
 *        (U(C1,C1) = 0). Internal constants share the pool; constant
 *        sinks get dedicated output gates.
 *
 * @param[in] nl Source netlist with structural labels.
 * @param[in] reach Reachable slots (barrier roots cone).
 * @param[in] clabel Per-slot Constness labels.
 * @return Pool gates the fresh build must materialize.
 */
[[nodiscard]] inline PoolNeeds size_pool_needs(const Netlist &nl, const bool *reach,
                                               const Constness *clabel) noexcept {
  PoolNeeds needs{};
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    if (!nl.gates[s].live || !reach[s]) {
      continue;
    }
    const bool is_sink = (nl.gates[s].output_fanout == 0);
    const Constness lc = clabel[s];
    if (lc == Constness::kC0 || lc == Constness::kC1) {
      if (is_sink) {
        // Dedicated sink gates need the shared C1 as driver for C0 sinks.
        if (lc == Constness::kC0) {
          needs.c1 = true;
        }
      } else {
        if (lc == Constness::kC0) {
          needs.c1 = true;
          needs.c0 = true;
        } else {
          needs.c1 = true;
        }
      }
    } else if (nl.gates[s].ignored_mask != 0) {
      // Non-constant gates with ignored inputs route through shared C0
      // (which needs C1).
      needs.c1 = true;
      needs.c0 = true;
    }
  }
  return needs;
}

/**
 * @brief Materialize the shared pool into the fresh build. U(0,0)=1 and
 *        U(C1,C1)=0 hold in both vocabularies, so the pool is
 *        target-neutral.
 *
 * @param[out] out Fresh netlist receiving the pool gates.
 * @param[out] ctx Emission state (pool slots recorded).
 * @param[in] target Conversion target (pool is target-neutral).
 * @param[in] needs Pool gates to materialize.
 * @return kNone on success; kBudget on gate-budget exhaustion.
 */
[[nodiscard]] inline BuildError emit_pool(Netlist &out, ConvertCtx &ctx, OptTarget target,
                                          PoolNeeds needs) noexcept {
  if (!needs.c1) {
    return BuildError::kNone;
  }
  const GateKind u = universal_kind(target);
  auto rc1 = add_gate(out, u, 2);
  if (!rc1.ok()) {
    return BuildError::kBudget;
  }
  Gate &c1 = out.gates[rc1.value];
  c1.ignored_mask = 0x03u; // U(0,0) = 1 in both vocabularies
  ctx.pool_c1 = rc1.value;
  if (needs.c0) {
    auto rc0 = add_gate(out, u, 2);
    if (!rc0.ok()) {
      return BuildError::kBudget;
    }
    out.gates[rc0.value].drivers[0] = rc1.value;
    out.gates[rc0.value].drivers[1] = rc1.value;
    ctx.pool_c0 = rc0.value;
  }
  return BuildError::kNone;
}

/**
 * @brief Creation pass in original slot order (fresh slots follow it).
 *
 * Constant gates reuse the pool (internal share, sinks get dedicated
 * outputs); sequential gates carry verbatim (opaque barrier); all other
 * gates emit via De Morgan as before.
 *
 * @param[in] nl Source netlist.
 * @param[in] reach Reachable slots to convert.
 * @param[in] clabel Per-slot Constness labels.
 * @param[out] ctx Emission state (fresh net + emit map).
 * @param[out] emit_map Orig slot to network-output fresh slot map.
 * @param[in] target Conversion target.
 * @return kNone on success; kBudget/kWiring on exhaustion or gaps.
 */
[[nodiscard]] inline BuildError create_networks(const Netlist &nl, const bool *reach,
                                                const Constness *clabel, ConvertCtx &ctx,
                                                std::uint16_t *emit_map,
                                                OptTarget target) noexcept {
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    if (!nl.gates[s].live || !reach[s]) {
      continue;
    }
    if (is_frozen_kind(nl.gates[s].kind)) {
      const std::uint16_t o = emit_seq_verbatim(ctx, nl, s);
      if (o == kNoDriver || ctx.failed) {
        return BuildError::kBudget;
      }
      emit_map[s] = o;
      continue;
    }
    const Constness lc = clabel[s];
    const bool is_sink = (nl.gates[s].output_fanout == 0);
    if (lc == Constness::kC0 || lc == Constness::kC1) {
      if (!is_sink) {
        // Internal constant: share the pool, no new gates.
        if (lc == Constness::kC0) {
          if (ctx.pool_c0 == kNoDriver) {
            return BuildError::kWiring;
          }
          emit_map[s] = ctx.pool_c0;
        } else {
          if (ctx.pool_c1 == kNoDriver) {
            return BuildError::kWiring;
          }
          emit_map[s] = ctx.pool_c1;
        }
        continue;
      }
      // Constant sink: dedicated output gate(s) so distinct block outputs
      // stay distinct.
      const GateKind u = universal_kind(target);
      if (lc == Constness::kC1) {
        auto r = add_gate(*ctx.out, u, 2);
        if (!r.ok()) {
          return BuildError::kBudget;
        }
        ctx.out->gates[r.value].ignored_mask = 0x03u;
        emit_map[s] = r.value;
        continue;
      } else {
        // C0 sink: dedicated U(C1_shared, C1_shared). Shared C1 must exist
        // (ensured by size_pool_needs above).
        if (ctx.pool_c1 == kNoDriver) {
          return BuildError::kWiring;
        }
        auto r = add_gate(*ctx.out, u, 2);
        if (!r.ok()) {
          return BuildError::kBudget;
        }
        ctx.out->gates[r.value].drivers[0] = ctx.pool_c1;
        ctx.out->gates[r.value].drivers[1] = ctx.pool_c1;
        emit_map[s] = r.value;
        continue;
      }
    }
    const std::uint16_t o = emit_gate(ctx, nl, s);
    if (o == kNoDriver || ctx.failed) {
      return BuildError::kBudget;
    }
    emit_map[s] = o;
  }
  return BuildError::kNone;
}

/**
 * @brief Wiring pass from the complete map, with wiring-time tying for
 *        sequential nets only: dangling pins derived from the same
 *        source tie leader share one tie class, so one variable stays
 *        one variable and sequential NAND/NOR toggles converge instead
 *        of multiplying pins and gates. Pure-combinational output stays
 *        bit-identical (no tying).
 *
 * @param[out] out Fresh netlist receiving wires and ties.
 * @param[out] ctx Emission state.
 * @param[in] pend Deferred wires from the creation pass.
 * @param[in] npend Pending-wire count.
 * @param[in] emit_map Complete orig-to-fresh driver map.
 * @param[in] structural_tie_duplicates True ties same-source dangling
 *            duplicates (sequential nets only).
 * @return kNone on success; kWiring on emit-map gaps or bad indices.
 */
[[nodiscard]] inline BuildError wire_pending(Netlist &out, ConvertCtx &ctx, const PendingWire *pend,
                                             std::size_t npend, const std::uint16_t *emit_map,
                                             bool structural_tie_duplicates) noexcept {
  std::uint16_t tie_first_fresh[kMaxGates * kMaxInputs];
  std::uint8_t tie_first_input[kMaxGates * kMaxInputs];
  for (std::size_t i = 0; i < kMaxGates * kMaxInputs; ++i) {
    tie_first_fresh[i] = kNoDriver;
    tie_first_input[i] = 0;
  }
  for (std::size_t p = 0; p < npend; ++p) {
    const PendingWire &w = pend[p];
    std::uint16_t drv = kNoDriver;
    if (w.use_pool) {
      drv = ctx.pool_c0;
    } else if (w.orig != kNoDriver) {
      if (w.orig >= kMaxGates || emit_map[w.orig] == kNoDriver) {
        return BuildError::kWiring;
      }
      drv = emit_map[w.orig];
    }
    if (w.fresh >= kMaxGates || w.input >= kMaxInputs) {
      return BuildError::kWiring;
    }
    wire_fresh_with_polarity(out, w.fresh, w.input, drv, w.inv);
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
  return BuildError::kNone;
}

/**
 * @brief Restore ignored marks on fresh dangling pins that mirror
 *        ignored original pins (they stay excluded from the block
 *        interface), plus ignored-output flags on the fresh output
 *        gates.
 *
 * Only single-gate networks mirror pin positions 1:1; multi-gate
 * networks route ignored originals through the pool (wired, no mark).
 *
 * @param[in] nl Source netlist (masks to mirror).
 * @param[out] out Fresh netlist receiving the marks.
 * @param[in] reach Reachable slots.
 * @param[in] emit_map Orig-to-fresh map.
 * @param[in] target Conversion target (single-gate mirror rule).
 */
inline void restore_masks(const Netlist &nl, Netlist &out, const bool *reach,
                          const std::uint16_t *emit_map, OptTarget target) noexcept {
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    if (!nl.gates[s].live || !reach[s]) {
      continue;
    }
    // Only single-gate networks mirror pin positions 1:1; multi-gate
    // networks route ignored originals through the pool (wired, no mark).
    const Gate &g = nl.gates[s];
    const std::uint16_t o = emit_map[s];
    const bool single_gate_network = (g.kind == GateKind::kNand && target == OptTarget::kNand) ||
        (g.kind == GateKind::kNor && target == OptTarget::kNor);
    if (single_gate_network) {
      out.gates[o].ignored_mask = g.ignored_mask;
    }
  }
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    if (!nl.gates[s].live || !reach[s]) {
      continue;
    }
    if (nl.gates[s].ignore_output) {
      out.gates[emit_map[s]].ignore_output = true;
    }
  }
}

/**
 * @brief Vocabulary-preserving post-cleanup: share identical wired gates
 *        and cancel internal double inverters (both keep all-NAND/
 *        all-NOR and never merge sinks, so output count is stable).
 *
 * Runs to fixpoint with DCE for orphans (roots are original fresh
 * outputs, hidden included, plus every verbatim-carried sequential
 * gate — the same opaque-barrier rule as the O0/O1 fixpoint roots — so
 * orphans that become sinks are removed, not kept as extra outputs).
 *
 * @param[out] out Fresh netlist to clean.
 * @param[in] nl Source netlist (root mapping).
 * @param[in] reach Reachable slots.
 * @param[in] emit_map Orig-to-fresh map (root translation).
 * @param[in] target Conversion target (universal kind).
 * @return kNone on success; kTopo/kIter on topology or bound failures.
 */
[[nodiscard]] inline BuildError post_cleanup(Netlist &out, const Netlist &nl, const bool *reach,
                                             const std::uint16_t *emit_map,
                                             OptTarget target) noexcept {
  const GateKind u = universal_kind(target);
  bool post_roots[kMaxGates] = {};
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    if (!nl.gates[s].live || !reach[s]) {
      continue;
    }
    if (is_barrier_root(nl, s)) {
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
    compute_feeds_seq(out, post_feeds);
    if (seq_net) {
      if (dedup_universal_tied_pass(out, u, post_feeds)) {
        pchanged = true;
      }
    } else if (dedup_universal_pass(out, u, post_feeds)) {
      pchanged = true;
    }
    if (!topo::rebuild(out, has_sequential_gates(out) ? EdgeFilter::kCombOnly : EdgeFilter::kAll)
             .ok()) {
      return BuildError::kTopo;
    }
    compute_feeds_seq(out, post_feeds);
    if (double_inv_universal_pass(out, u, post_feeds)) {
      pchanged = true;
    }
    if (!topo::rebuild(out, has_sequential_gates(out) ? EdgeFilter::kCombOnly : EdgeFilter::kAll)
             .ok()) {
      return BuildError::kTopo;
    }
    if (dce_pass(out, post_roots)) {
      pchanged = true;
    }
    if (!topo::rebuild(out, has_sequential_gates(out) ? EdgeFilter::kCombOnly : EdgeFilter::kAll)
             .ok()) {
      return BuildError::kTopo;
    }
    if (!pchanged) {
      break;
    }
    if (it + 1 >= kPostIters) {
      return BuildError::kIter;
    }
  }
  return BuildError::kNone;
}

/**
 * @brief Truth-table resynthesis pick plus final recompile: try the
 *        minimized two-level network, keep the smaller (gates, then
 *        inputs), then recount and recompile.
 *
 * Small blocks only (exact minimization fixes repeated O2 toggles
 * staying near-optimal); falls back to structural for XOR-like parity
 * (where two-level is larger) and large blocks (n>8 or m>8).
 *
 * @param[out] out Structural fresh netlist (returned when it wins).
 * @param[out] src_nl Source netlist (recompiled on the structural path).
 * @param[in] target Conversion target.
 * @return kOk with the winning netlist; kInternal with an "optimize:
 *         ..." message on topology errors.
 */
[[nodiscard]] inline core::Result<Netlist> finish_o2(Netlist &out, Netlist &src_nl,
                                                     OptTarget target) noexcept {
  {
    Netlist min_out{};
    if (try_minimized_o2(src_nl, target, min_out)) {
      const std::uint16_t g_struct = out.live_count;
      const std::uint16_t g_min = min_out.live_count;
      const std::size_t in_struct = block_input_count(out);
      const std::size_t in_min = block_input_count(min_out);
      if ((g_min < g_struct) || (g_min == g_struct && in_min < in_struct)) {
        return core::Result<Netlist>::success(min_out);
      }
    }
  }
  recount_fanout(out);
  out.dirty = true;
  out.topo_size = 0;
  if (!topo::rebuild(out, has_sequential_gates(out) ? EdgeFilter::kCombOnly : EdgeFilter::kAll)
           .ok()) {
    return core::Result<Netlist>::failure("optimize: topology error", core::StatusCode::kInternal);
  }
  return core::Result<Netlist>::success(out);
}

/**
 * @brief Loud message for a fresh-build failure mode (reported once by
 *        optimize()).
 *
 * @param[in] e Build error to name.
 * @return String-literal message (zero-copy, no allocation).
 */
[[nodiscard]] inline const char *build_error_message(BuildError e) noexcept {
  switch (e) {
  case BuildError::kNone:
    return "optimize: topology error";
  case BuildError::kBudget:
    return "optimize: gate budget exceeded (kMaxGates)";
  case BuildError::kWiring:
    return "optimize: internal wiring error";
  case BuildError::kTopo:
    return "optimize: topology error";
  case BuildError::kIter:
    return "optimize: internal iteration bound";
  }
  return "optimize: topology error";
}

} // namespace detail

} // namespace wengine::logic
