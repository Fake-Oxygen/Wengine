#pragma once

/**
 * @file
 * Pass seam for the logic optimizer: the one module behind fixpoint
 * orchestration. Before this seam, optimize() re-ran topo::rebuild 5x
 * per fixpoint iteration (plus 3x per post-cleanup iteration) and each
 * pass carried its own scratch signature — a new pass meant
 * orchestrator surgery. Now a pass is one adapter
 * (`run(work, ctx) -> changed`) over an opaque PassCtx; both fixpoint
 * loops are one table + run_fixpoint. New pass = one adapter + one
 * table entry, zero orchestrator edits; every pass is testable through
 * run() alone.
 *
 * Rebuild discipline (one rebuild per iteration): passes declare
 * needs_topo (only topo readers, i.e. const-fold's classify) and
 * needs_feeds (barrier-bit readers); the driver recompiles topology
 * and recomputes feeds_seq only when stale (dirty). Steady-state
 * iterations do exactly one rebuild; changed iterations rebuild only
 * before readers. Observable behavior (pass order, convergence,
 * messages) is unchanged.
 *
 * Dependency rules: header-only, allocation-free, exception-free,
 * noexcept throughout. Depends on the pass headers (adapters call the
 * existing detail:: passes) plus barrier/exact/topo vocabulary — never
 * the reverse (no pass header includes this one). Logic is a
 * standalone domain: never depends on CLI types. Consumers:
 * optimize.hpp (O0/O1 loop) and optimize_o2_build.hpp (post-cleanup).
 */

#include "wengine/core/status.hpp"
#include "wengine/logic/netlist.hpp"
#include "wengine/logic/optimize_barrier.hpp"
#include "wengine/logic/optimize_exact.hpp"
#include "wengine/logic/optimize_o2_dedup.hpp"
#include "wengine/logic/optimize_passes.hpp"
#include "wengine/logic/optimize_types.hpp"
#include "wengine/logic/seq_state.hpp"
#include "wengine/logic/topo.hpp"

#include <cstddef>

namespace wengine::logic::detail {

/**
 * @brief Opaque pass context: every scratch slice the passes draw
 *        from, plus the policy flags selecting variants.
 *
 * The orchestrator fills this once from its caller-owned scratch;
 * adapters read only their own slices. Pool-sized spans are validated
 * by the caller (OptimizeScratch::validate / post_cleanup entry
 * checks) before the driver runs. Topology rides the CompiledOrder
 * seam (compile/query own their workspace internally), so no pass
 * carries Kahn scratch.
 */
struct PassCtx {
  /** @brief Per-slot sequential-user bits (recomputed by the driver when stale). */
  bool *feeds = nullptr;
  /** @brief Per-slot DCE/root flags (barrier roots, fixed per run). */
  const bool *roots = nullptr;
  /** @brief Per-slot Constness labels (const-fold scratch). */
  std::span<Constness> labels;
  /** @brief Resolved values (n * kMaxInputs, exact-pass eval scratch). */
  std::span<LogicValue> resolved;
  /** @brief Gate outputs (n, exact-pass eval scratch). */
  std::span<LogicValue> gate_out;
  /** @brief Tie survivors (n * kMaxInputs flats). */
  std::span<std::size_t> ties;
  /** @brief DCE keep flags (n). */
  std::span<bool> keep;
  /** @brief DFS stack (n slots). */
  std::span<Slot> stack;
  /** @brief Dedup key hashes (n, post-cleanup). */
  std::span<std::uint64_t> bucket;
  /** @brief Dedup chain heads (n, post-cleanup). */
  std::span<Slot> bhead;
  /** @brief Dedup chain links (n, post-cleanup). */
  std::span<Slot> bnext;
  /** @brief True selects the kCombOnly filter (sequential nets). */
  bool seq_net = false;
  /** @brief False skips the De Morgan fusion sweep (O0). */
  bool want_fuse = false;
  /** @brief Universal kind for the O2 post-cleanup passes. */
  GateKind universe = GateKind::kNand;
  /** @brief True selects tied-dangling dedup (sequential nets). */
  bool merge_tied = false;
};

/**
 * @brief One optimizer pass over the opaque context.
 *
 * @param[in,out] nl Netlist under optimization.
 * @param[in,out] ctx Scratch + policy (see PassCtx).
 * @return True when the net changed (topology now dirty).
 */
using PassRun = bool (*)(Netlist &, PassCtx &);

/**
 * @brief One fixpoint table entry: a pass plus its freshness needs.
 *
 * needs_topo marks topo_order readers (classify); needs_feeds marks
 * barrier-bit readers (bypass/fusion/dedup/double-inv). The driver
 * recompiles/recomputes only when stale, so steady-state iterations
 * cost one rebuild.
 */
struct Pass {
  /** @brief Pass name (diagnostics only). */
  const char *name = nullptr;
  /** @brief True when the pass reads topo_order (rebuild if dirty). */
  bool needs_topo = false;
  /** @brief True when the pass reads feeds_seq (recompute if stale). */
  bool needs_feeds = false;
  /** @brief The adapter (see pass_* below). */
  PassRun run = nullptr;
};

// --- Adapters (one per pass; thin, behavior-identical) -----------------------

/** @brief BUF/double-NOT bypass adapter. */
inline bool pass_bypass(Netlist &nl, PassCtx &ctx) noexcept {
  return bypass_pass(nl, ctx.feeds, ctx.ties);
}

/** @brief De Morgan fusion adapter (no-op unless want_fuse). */
inline bool pass_fusion(Netlist &nl, PassCtx &ctx) noexcept {
  if (!ctx.want_fuse) {
    return false;
  }
  return fusion_sweep(nl, ctx.feeds, ctx.ties);
}

/** @brief Constant-folding adapter. */
inline bool pass_const_fold(Netlist &nl, PassCtx &ctx) noexcept {
  return const_fold_pass(nl, ctx.roots, ctx.labels, ctx.ties);
}

/** @brief Exact truth-table simplification adapter (self-rebuilding). */
inline bool pass_exact(Netlist &nl, PassCtx &ctx) {
  return exact_simplify_pass(nl, ctx.resolved, ctx.gate_out, ctx.ties);
}

/** @brief Dead-gate elimination adapter. */
inline bool pass_dce(Netlist &nl, PassCtx &ctx) noexcept {
  return dce_pass(nl, ctx.roots, ctx.keep, ctx.stack, ctx.ties);
}

/** @brief Universal-gate dedup adapter (strict vs tied by merge_tied). */
inline bool pass_dedup(Netlist &nl, PassCtx &ctx) noexcept {
  if (ctx.merge_tied) {
    return dedup_universal_tied_pass(nl, ctx.universe, ctx.feeds, ctx.ties, ctx.bucket, ctx.bhead, ctx.bnext);
  }
  return dedup_universal_pass(nl, ctx.universe, ctx.feeds, ctx.ties, ctx.bucket, ctx.bhead, ctx.bnext);
}

/** @brief Double-inverter cancellation adapter. */
inline bool pass_double_inv(Netlist &nl, PassCtx &ctx) noexcept {
  return double_inv_universal_pass(nl, ctx.universe, ctx.feeds, ctx.ties);
}

// --- Driver ------------------------------------------------------------------

/**
 * @brief Recompile topology iff stale (the one-rebuild-per-iteration
 *        seam: dirty is authoritative — every topo_order.clear() pairs
 *        with dirty = true, and only a successful rebuild clears it).
 *
 * @param[out] nl Netlist receiving topo_order/level/dirty.
 * @param[in] f Edge set to compile (kCombOnly for sequential nets).
 * @return kOk on success; kInvalidArgument on topology defects.
 */
[[nodiscard]] inline core::Status ensure_topo(Netlist &nl, EdgeFilter f) {
  if (!nl.dirty && !nl.topo_order.empty()) {
    return core::Status::success();
  }
  return topo::compile(nl, f);
}

/**
 * @brief Table-driven fixpoint: run passes in order until none changes.
 *
 * One rebuild per steady-state iteration (ensure_topo skips when
 * clean); changed iterations rebuild only before topo readers, and
 * feeds_seq recomputes only before barrier readers when stale. Pass
 * order, the iteration bound, and the failure modes match the
 * pre-seam loops exactly.
 *
 * @param[out] nl Netlist under optimization (compiled on entry iff
 *             dirty; validated on every compiled pass).
 * @param[in] passes Fixpoint table (see kFixpoint/kPostCleanup).
 * @param[in] npass Table length.
 * @param[in,out] ctx Scratch + policy (feeds recomputed in place).
 * @return kNone on convergence; kTopo on topology defects, kIter on
 *         iteration-bound exhaustion.
 */
[[nodiscard]] inline BuildError run_fixpoint(Netlist &nl, const Pass *passes, std::size_t npass,
                                             PassCtx &ctx) {
  constexpr std::size_t kMaxItersBase = 16;
  bool feeds_stale = true;
  std::size_t iter = 0;
  while (true) {
    if (iter >= 2 * slot_count(nl) + kMaxItersBase) {
      return BuildError::kIter;
    }
    ++iter;
    // Recompute per iteration: passes only retag to combinational kinds,
    // but a hoisted bool would silently pick the wrong filter if that ever
    // changes. One O(N) scan per iteration is negligible next to a rebuild.
    const EdgeFilter f = has_sequential_gates(nl) ? EdgeFilter::kCombOnly : EdgeFilter::kAll;
    ctx.seq_net = (f == EdgeFilter::kCombOnly);
    if (!ensure_topo(nl, f).ok()) {
      return BuildError::kTopo;
    }
    bool changed = false;
    for (std::size_t p = 0; p < npass; ++p) {
      if (passes[p].needs_topo && nl.dirty) {
        if (!ensure_topo(nl, f).ok()) {
          return BuildError::kTopo;
        }
      }
      if (passes[p].needs_feeds && feeds_stale) {
        compute_feeds_seq(nl, ctx.feeds);
        feeds_stale = false;
      }
      if (passes[p].run(nl, ctx)) {
        changed = true;
        feeds_stale = true;
      }
    }
    if (!changed) {
      break;
    }
  }
  return BuildError::kNone;
}

} // namespace wengine::logic::detail
