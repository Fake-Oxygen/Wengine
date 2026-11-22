#pragma once

/**
 * @file
 * Combinational optimizer (LOGIC-4): pure `optimize(netlist, opts)` —
 * the optimize stage of the logic pipeline (netlist parse -> state ->
 * eval -> layout/topo -> optimize). Sells the simplification story in
 * three explicit levels; the input is never touched (const ref in, new
 * Netlist out); overflow past kMaxGates is a loud Result failure,
 * never a silent drop. This header only orchestrates: the individual
 * passes live in optimize_passes.hpp (O0/O1), the sequential barrier
 * vocabulary in optimize_barrier.hpp, exact truth-table simplification
 * in optimize_exact.hpp, and the O2 fresh build in
 * optimize_o2_{build,dedup,emit,min}.hpp.
 *
 * Levels (docs/logic.md section 6):
 *   O0: constant folding / annihilator + identity with tied inputs,
 *       double-negation + BUF bypass, dead-gate elimination + exact
 *       truth-table simplification for small blocks (n<=10,m<=16:
 *       semantic constants like x&~x==0 + dead inputs). Never grows
 *       the net (gates_after <= gates_before).
 *   O1: O0 + De Morgan bubble-push canonical form: every NOT driven
 *       by an AND/OR/NAND/NOR/XOR/XNOR is fused into the complementary
 *       kind in place (NOT(AND) -> NAND, NOT(OR) -> NOR, ...). Still
 *       mixed gates. Invariant afterwards: no live NOT is driven by a
 *       fusable kind. Output double-negations (NOT driven by NOT at a
 *       block output) are deliberately preserved: deleting the outer
 *       gate would move the output to another slot, and interface
 *       stability wins over fusion.
 *   O2: O1 + full conversion to NAND-only (--to nand) or NOR-only
 *       (--to nor). Small blocks (n<=8,m<=8) try minimized two-level
 *       resynthesis (QM + greedy, SOP/POS, shared inverters, tied
 *       inputs) and pick smaller vs structural De Morgan (which is
 *       optimal for XOR-like parity); large blocks use structural +
 *       vocabulary-preserving cleanup (dedup + double-inverter
 *       cancellation + DCE). Constants reuse a shared pool (1-2
 *       gates, not 4+). Gate count may still grow for large nets; the
 *       caller reports before/after counts so the trade is visible.
 *       Minimized may permute inputs (tied, one per variable);
 *       structural preserves positional order (duplicating pins for
 *       inversion).
 *
 * Determinism / interface rules (all levels):
 * - Slot-order processing everywhere; new gates take the lowest free
 *   slot (O0/O1) or compact slot order (O2); per-kind counters are
 *   never reused (in-place kind changes take a fresh counter, so
 *   display names stay unique for resolve_gate).
 * - Block outputs keep their slots (O0/O1 convert in place) or their
 *   relative order (O2 emits output networks in original slot order),
 *   so truth-equivalence checks can map outputs positionally.
 * - DCE roots are the ORIGINAL sinks (live fanout-0 gates captured
 *   before any pass, ignored outputs included) plus every live
 *   sequential gate: state readback observes flops outside the block
 *   interface, so lack of fanout never makes a state element dead
 *   (opaque barrier). Optimization orphans (bypassed inners,
 *   fused-away drivers, dropped constant gates) are removed, but user
 *   logic feeding an ignored output is never deleted: ignore means
 *   "hidden", not "dead", and the optimizer must not destroy
 *   circuitry the user only hid.
 * - Ignored dangling inputs tie to kLow during eval (netlist.hpp
 *   rule). O0 reasons about them as constant-0; O2 materializes a
 *   shared target-kind constant pool for them. O2 resets
 *   propagation_delay_ps (the network is reshaped; O0/O1 preserve
 *   delays in place).
 *
 * Capacity (dynamic pool): optimize is a core over Netlist taking the
 * caller-owned OptimizeScratch bundle (the data-model scratch table:
 * work/fresh/min out-buffers replacing the old by-value locals,
 * pending wires, labels/roots/feeds/reach, emit map, eval + Kahn
 * scratch); the same-name wrapper below keeps today's by-value
 * semantics over the dynamic pool with heap-backed scratch. Internal
 * cross-calls are bare (no capacity parameter).
 *
 * Dependency rules: header-only, allocation-free, exception-free like
 * the rest of wengine/logic. No iostream (report printing belongs to
 * the CLI verb in LOGIC-8; the engine exposes before/after counts via
 * live_count). Logic is a standalone domain — never depends on CLI
 * types. Consumers: the CLI `optimize` verb, SequentialSolver
 * lowering, and the logic optimizer tests.
 */


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
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace wengine::logic {

/**
 * @brief Caller-owned optimize scratch bundle (see the data-model
 *        scratch table): work/fresh/min out-buffers replacing the old
 *        by-value Netlist locals, pending wires, constness labels,
 *        root/feed/reachability flags, DCE keep/stack, emit map, tie
 *        scratch, eval scratch, and the nested Kahn bundle. Sized by
 *        the pool (see ensure_workspace); validated loudly before any
 *        work.
 *
 * Sequential sharing: the phases run one after another, so labels,
 * keep, stack, tie scratch, and the eval/Kahn scratch are reused across
 * phases (never live at once for two purposes).
 *
 */
struct OptimizeScratch {
  /** @brief O0/O1 fixpoint working net (caller-owned out-buffer). */
  Netlist *work = nullptr;
  /** @brief O2 fresh-build net (caller-owned out-buffer). */
  Netlist *fresh = nullptr;
  /** @brief Minimized-candidate net (caller-owned out-buffer). */
  Netlist *min_buf = nullptr;
  /** @brief Per-slot Constness labels (n entries). */
  std::span<detail::Constness> labels;
  /** @brief DCE/O2 root flags (n entries). */
  std::span<bool> roots;
  /** @brief Sequential-feed flags (n entries). */
  std::span<bool> feeds;
  /** @brief O2 reachability flags (n entries). */
  std::span<bool> reach;
  /** @brief DCE keep flags (n entries). */
  std::span<bool> keep;
  /** @brief DFS stack (n slots). */
  std::span<Slot> stack;
  /** @brief Orig-to-fresh driver map (n slots). */
  std::span<Slot> emit_map;
  /** @brief Tie scratch (n * kMaxInputs flats). */
  std::span<std::size_t> tie_scratch;
  /** @brief Resolved dangling values (n * kMaxInputs entries). */
  std::span<LogicValue> resolved;
  /** @brief Per-slot settled outputs (n entries). */
  std::span<LogicValue> gate_out;
  /** @brief Nested Kahn scratch bundle. */
  topo::TopoScratch topo;
  /**
   * @brief Loud size validation before any work (engine-seam D3 rule).
   *
   * Covers the O0/O1 fixpoint scratch (work net, labels, roots, feeds,
   * keep, DFS stack, tie scratch, eval scratch, Kahn bundle). The O2
   * fresh-build scratch (fresh/min out-buffers, pending wires, emit
   * map, reach flags, wiring maps) is validated separately by
   * validate_o2(), so O0/O1 runs never pay for scratch they cannot
   * touch.
   *
   * @param[in] n Addressable slot count the spans must cover.
   * @return kOk when every O0/O1 span meets its data-model size and the work buffer is present.
   */
  [[nodiscard]] core::Status validate(std::size_t n) const noexcept {
    if (work == nullptr) {
      return core::Status::failure(core::StatusCode::kInvalidArgument);
    }
    if (labels.size() < n || roots.size() < n || feeds.size() < n || keep.size() < n || stack.size() < n ||
        tie_scratch.size() < n * kMaxInputs || resolved.size() < n * kMaxInputs || gate_out.size() < n) {
      return core::Status::failure(core::StatusCode::kInvalidArgument);
    }
    return topo.validate(n);
  }
  /**
   * @brief Loud size validation for the O2 fresh-build scratch.
   *
   * @param[in] n Addressable slot count the spans must cover.
   * @return kOk when every O2-only span meets its data-model size and
   *         both out-buffers are present.
   */
  [[nodiscard]] core::Status validate_o2(std::size_t n) const noexcept {
    if (fresh == nullptr || min_buf == nullptr) {
      return core::Status::failure(core::StatusCode::kInvalidArgument);
    }
    if (emit_map.size() < n || reach.size() < n) {
      return core::Status::failure(core::StatusCode::kInvalidArgument);
    }
    return core::Status::success();
  }
};

/**
 * @brief Pure optimizer: const ref in, new Netlist out; the input is
 *        never modified (all surgery happens on the caller's work buffer).
 *
 * Runs the O0/O1 fixpoint (bypass, De Morgan fusion at O1/O2, constant
 * folding, exact simplification, DCE) then, at O2, the fresh universal
 * rebuild (shared constant pool, creation in original slot order,
 * wiring from the complete driver map, vocabulary-preserving cleanup,
 * minimized-vs-structural pick). Sequential nets ride through as opaque
 * barriers (state elements carried verbatim, combinational glue
 * converted around them).
 *
 * @param[in] in Source netlist (read-only).
 * @param[in] opts Level (O0/O1/O2) plus the O2 target kind.
 * @param[in] scr Caller-owned scratch (see OptimizeScratch).
 * @param[out] out Caller-owned buffer receiving the optimized netlist
 *             (heap at scale — the result is never returned by value,
 *             which would put a pool-sized object on the caller stack).
 * @return kOk on success; kInvalidArgument with an "optimize: ..."
 *         message on topology errors, kInternal on gate-budget
 *         exhaustion or undersized scratch. Never silently drops gates.
 * @note Optimizing twice is idempotent. O0 never grows the net
 *       (gates_after <= gates_before); O2 may grow large nets and the
 *       caller reports before/after counts.
 */

[[nodiscard]] inline core::Result<Netlist *> optimize(const Netlist &in, OptimizeOptions opts, OptimizeScratch &scr, Netlist &out) {
  if (!scr.validate(slot_count(in)).ok()) {
    return core::Result<Netlist *>::failure("optimize: scratch too small", core::StatusCode::kInternal);
  }
  if (opts.level == OptLevel::kO2 && !scr.validate_o2(slot_count(in)).ok()) {
    return core::Result<Netlist *>::failure("optimize: scratch too small", core::StatusCode::kInternal);
  }
  // 011: O2 replaces the workspace wholesale via its own emitter, which
  // has no shared-span vocabulary. Emitting O1-level output under an O2
  // request would be a silent contract break — refuse loudly, input
  // untouched, O0/O1 remain available. A verbatim-carry O2 emitter is
  // the named follow-on.
  if (opts.level == OptLevel::kO2 && any_shared_mark(in)) {
    return core::Result<Netlist *>::failure("optimize: shared nets unsupported at O2 (use O0/O1)", core::StatusCode::kInvalidArgument);
  }
  // 012: O2's emitter has no buffer/pull vocabulary either (no Z in the
  // universal-gate rebuild, no flat-keyed pull_cfg in the fresh pool).
  // Refuse loudly, input untouched, O0/O1 remain available; a
  // verbatim-carry O2 emitter is the named follow-on.
  if (opts.level == OptLevel::kO2 && (has_buffer_gates(in) || any_pull(in))) {
    return core::Result<Netlist *>::failure("optimize: buffers/pulls unsupported at O2 (use O0/O1)", core::StatusCode::kInvalidArgument);
  }
  Netlist &nl = *scr.work;
  nl = in;
  nl.dirty = true;
  nl.topo_order.clear();
  detail::recount_fanout(nl);

  detail::collect_barrier_roots(nl, scr.roots.data());

  const bool want_fuse = (opts.level == OptLevel::kO1) || (opts.level == OptLevel::kO2);
  constexpr std::size_t kMaxItersBase = 16;
  std::size_t iter = 0;
  // --- O0/O1 fixpoint (canonical shape: one call per pass) ---
  while (true) {
    if (iter >= 2 * slot_count(nl) + kMaxItersBase) {
      return core::Result<Netlist *>::failure("optimize: internal iteration bound", core::StatusCode::kInternal);
    }
    ++iter;
    if (!topo::rebuild(nl, has_sequential_gates(nl) ? EdgeFilter::kCombOnly : EdgeFilter::kAll, scr.topo).ok()) {
      return core::Result<Netlist *>::failure("optimize: topology error", core::StatusCode::kInternal);
    }
    bool changed = false;
    detail::compute_feeds_seq(nl, scr.feeds.data());
    if (detail::bypass_pass(nl, scr.feeds.data(), scr.tie_scratch)) {
      changed = true;
    }
    if (!topo::rebuild(nl, has_sequential_gates(nl) ? EdgeFilter::kCombOnly : EdgeFilter::kAll, scr.topo).ok()) {
      return core::Result<Netlist *>::failure("optimize: topology error", core::StatusCode::kInternal);
    }
    detail::compute_feeds_seq(nl, scr.feeds.data());
    if (want_fuse && detail::fusion_sweep(nl, scr.feeds.data(), scr.tie_scratch)) {
      changed = true;
    }
    if (!topo::rebuild(nl, has_sequential_gates(nl) ? EdgeFilter::kCombOnly : EdgeFilter::kAll, scr.topo).ok()) {
      return core::Result<Netlist *>::failure("optimize: topology error", core::StatusCode::kInternal);
    }
    if (detail::const_fold_pass(nl, scr.roots.data(), scr.labels, scr.tie_scratch)) {
      changed = true;
    }
    if (!topo::rebuild(nl, has_sequential_gates(nl) ? EdgeFilter::kCombOnly : EdgeFilter::kAll, scr.topo).ok()) {
      return core::Result<Netlist *>::failure("optimize: topology error", core::StatusCode::kInternal);
    }
    if (detail::exact_simplify_pass(nl, scr.resolved, scr.gate_out, scr.topo, scr.tie_scratch)) {
      changed = true;
    }
    if (!topo::rebuild(nl, has_sequential_gates(nl) ? EdgeFilter::kCombOnly : EdgeFilter::kAll, scr.topo).ok()) {
      return core::Result<Netlist *>::failure("optimize: topology error", core::StatusCode::kInternal);
    }
    if (detail::dce_pass(nl, scr.roots.data(), scr.keep, scr.stack, scr.tie_scratch)) {
      changed = true;
    }
    if (!changed) {
      break;
    }
  }

  if (opts.level != OptLevel::kO2) {
    if (!topo::rebuild(nl, has_sequential_gates(nl) ? EdgeFilter::kCombOnly : EdgeFilter::kAll, scr.topo).ok()) {
      return core::Result<Netlist *>::failure("optimize: topology error", core::StatusCode::kInternal);
    }
    out = nl;
    return core::Result<Netlist *>::success(&out);
  }

  // --- O2: fresh universal build (phases live in optimize_o2_build.hpp;
  // optimize() only orchestrates) ---------------------------------------
  detail::collect_reachable(nl, scr.reach.data(), scr.keep, scr.stack);

  Netlist &fresh = *scr.fresh;
  fresh = Netlist{};
  for (std::size_t s = 0; s < slot_count(nl); ++s) {
    scr.emit_map[s] = kNoDriver;
  }
  std::vector<detail::PendingWire> pend;
  pend.reserve(slot_count(nl) * kMaxInputs);
  detail::ConvertCtx ctx{};
  ctx.out = &fresh;
  ctx.target = opts.target;
  ctx.emit_map = scr.emit_map.data();
  ctx.pend = &pend;

  // Constant-aware shared pool: structural constant labels (ignored pins
  // tie to 0) let us reuse the pool instead of re-emitting logic for
  // constant gates. Internal constants share one pool; constant sinks get
  // dedicated output gates (sharing a sink would merge distinct block
  // outputs). This keeps 1-gate constants at 1-2 gates instead of 4+.
  detail::classify(nl, scr.labels.data());
  const detail::PoolNeeds pool_needs = detail::size_pool_needs(nl, scr.reach.data(), scr.labels.data());
  {
    const detail::BuildError perr = detail::emit_pool(fresh, ctx, opts.target, pool_needs);
    if (perr != detail::BuildError::kNone) {
      return core::Result<Netlist *>::failure(detail::build_error_message(perr), core::StatusCode::kInternal);
    }
  }

  // Creation pass in original slot order (fresh slots follow it).
  {
    const detail::BuildError cerr =
        detail::create_networks(nl, scr.reach.data(), scr.labels.data(), ctx, scr.emit_map.data(), opts.target);
    if (cerr != detail::BuildError::kNone) {
      return core::Result<Netlist *>::failure(detail::build_error_message(cerr), core::StatusCode::kInternal);
    }
  }

  // Wiring pass from the complete map (wiring-time tying for sequential
  // nets only; pure-combinational output stays bit-identical).
  //
  // Fresh-pool scratch: emission grows `fresh` past the input pool, so
  // the input-sized wrapper spans cannot serve the out-pool-indexed
  // phases. These locals are sized to the emitted pool (cold path).
  {
    const std::size_t m = slot_count(fresh);
    std::vector<Slot> tie_first(m * kMaxInputs, kNoDriver);
    std::vector<std::uint8_t> tie_first_in(m * kMaxInputs, 0);
    const bool structural_tie_duplicates = has_sequential_gates(nl);
      const detail::BuildError werr = detail::wire_pending(
          fresh, ctx, pend, scr.emit_map.data(), structural_tie_duplicates,
        std::span<Slot>(tie_first.data(), tie_first.size()),
        std::span<std::uint8_t>(tie_first_in.data(), tie_first_in.size()));
    if (werr != detail::BuildError::kNone) {
      return core::Result<Netlist *>::failure(detail::build_error_message(werr), core::StatusCode::kInternal);
    }
  }
  detail::restore_masks(nl, fresh, scr.reach.data(), scr.emit_map.data(), opts.target);
  // Vocabulary-preserving post-cleanup (dedup + double-inverter
  // cancellation + DCE to fixpoint) and minimized-vs-structural picking.
  {
    const std::size_t m = slot_count(fresh);
    auto proots = std::make_unique<bool[]>(m);
    auto pfeeds = std::make_unique<bool[]>(m);
    auto pkeep = std::make_unique<bool[]>(m);
    std::vector<Slot> pstack(m);
    std::vector<std::size_t> ptie(m * kMaxInputs, 0);
    ensure_workspace(fresh, m);
    const topo::TopoScratch fts{std::span<Slot>(fresh.ws_order.data(), m),
                                std::span<Slot>(fresh.ws_levels.data(), m),
                                std::span<std::uint8_t>(fresh.ws_indeg.data(), m),
                                std::span<Slot>(fresh.ws_queue.data(), m)};
    const detail::BuildError perr = detail::post_cleanup(fresh, nl, scr.reach.data(), scr.emit_map.data(), opts.target, std::span<bool>(proots.get(), m),
        std::span<bool>(pfeeds.get(), m), std::span<bool>(pkeep.get(), m), std::span<Slot>(pstack.data(), m), std::span<std::size_t>(ptie.data(), ptie.size()), fts);
    if (perr != detail::BuildError::kNone) {
      return core::Result<Netlist *>::failure(detail::build_error_message(perr), core::StatusCode::kInternal);
    }
  }
  {
    const std::size_t m = slot_count(fresh);
    std::vector<LogicValue> mresolved(m * kMaxInputs);
    std::vector<LogicValue> mgate_out(m);
    ensure_workspace(fresh, m);
    const topo::TopoScratch mts{std::span<Slot>(fresh.ws_order.data(), m),
                                std::span<Slot>(fresh.ws_levels.data(), m),
                                std::span<std::uint8_t>(fresh.ws_indeg.data(), m),
                                std::span<Slot>(fresh.ws_queue.data(), m)};
    const detail::BuildError ferr = detail::finish_o2(fresh, nl, opts.target, *scr.min_buf, std::span<LogicValue>(mresolved.data(), mresolved.size()),
                                                      std::span<LogicValue>(mgate_out.data(), mgate_out.size()), mts);
    if (ferr != detail::BuildError::kNone) {
      return core::Result<Netlist *>::failure(detail::build_error_message(ferr), core::StatusCode::kInternal);
    }
  }
  out = fresh;
  return core::Result<Netlist *>::success(&out);
}

/**
 * @brief Pure optimizer: const ref in, new Netlist out; the input is
 *        never modified (all surgery happens on the copy).
 *
 * Workspace-backed convenience entry: heap-backed scratch sized by the
 * pool; the core above serves caller-owned scratch.
 *
 * Runs the O0/O1 fixpoint (bypass, De Morgan fusion at O1/O2, constant
 * folding, exact simplification, DCE) then, at O2, the fresh universal
 * rebuild. Sequential nets ride through as opaque barriers.
 *
 * @param[in] in Source netlist (read-only).
 * @param[in] opts Level (O0/O1/O2) plus the O2 target kind.
 * @return kOk with the optimized netlist on success; kInvalidArgument
 *         with an "optimize: ..." message on topology errors, kInternal
 *         on gate-budget exhaustion or internal iteration bounds. Never
 *         silently drops gates.
 * @note Optimizing twice is idempotent. O0 never grows the net
 *       (gates_after <= gates_before); O2 may grow large nets and the
 *       caller reports before/after counts.
 */
[[nodiscard]] inline core::Result<Netlist> optimize(const Netlist &in, OptimizeOptions opts) {
  const std::size_t n = slot_count(in);
  const std::size_t nflat = n * kMaxInputs;
  Netlist work;
  Netlist fresh;
  Netlist min_buf;
  if (!ensure_slots(work, n) || !ensure_slots(fresh, n) || !ensure_slots(min_buf, n)) {
    return core::Result<Netlist>::failure("optimize: netlist full", core::StatusCode::kInternal);
  }
  std::vector<detail::Constness> labels(n);
  auto roots = std::make_unique<bool[]>(n);
  auto feeds = std::make_unique<bool[]>(n);
  auto reach = std::make_unique<bool[]>(n);
  auto keep = std::make_unique<bool[]>(n);
  std::vector<Slot> stack(n);
  std::vector<Slot> emit_map(n);
  std::vector<std::size_t> tie_scratch(nflat);
  std::vector<LogicValue> resolved(nflat);
  std::vector<LogicValue> gate_out(n);
  std::vector<Slot> order(n);
  std::vector<Slot> levels(n);
  std::vector<std::uint8_t> indeg(n);
  std::vector<Slot> queue(n);
  OptimizeScratch scr{&work, &fresh, &min_buf,
                      std::span<detail::Constness>(labels.data(), n),
                      std::span<bool>(roots.get(), n),
                      std::span<bool>(feeds.get(), n),
                      std::span<bool>(reach.get(), n),
                      std::span<bool>(keep.get(), n),
                      std::span<Slot>(stack.data(), n),
                      std::span<Slot>(emit_map.data(), n),
                      std::span<std::size_t>(tie_scratch.data(), nflat),
                      std::span<LogicValue>(resolved.data(), nflat),
                      std::span<LogicValue>(gate_out.data(), n),
                      topo::TopoScratch{std::span<Slot>(order.data(), n), std::span<Slot>(levels.data(), n), std::span<std::uint8_t>(indeg.data(), n), std::span<Slot>(queue.data(), n)}};
  Netlist opt_out{};
  auto r = optimize(in, opts, scr, opt_out);
  if (!r.ok()) {
    return core::Result<Netlist>::failure(r.error, r.status.code);
  }
  return core::Result<Netlist>::success(std::move(opt_out));
}

} // namespace wengine::logic
