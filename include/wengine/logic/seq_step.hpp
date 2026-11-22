#pragma once

/**
 * @file
 * Scalar single-step sequential eval: the clocking stage of the logic
 * pipeline (netlist parse -> state -> eval -> layout/topo -> optimize).
 * Zero-delay stepper for edge-triggered DFF/JK/T plus level-sensitive
 * DLATCH/SRLATCH, with deterministic end-of-step evaluation. One call
 * advances every edge flop by one rising edge and re-settles every latch
 * from the current nets.
 *
 * Edge kinds (DFF/JK/T): CLK pins are validated Low/High but do not
 * gate; the edge itself is the call (same as the signals two-phase
 * convention where generators evaluate at time T and sums commit at
 * T+1). Level kinds (DLATCH/SRLATCH): Q resolves immediately from the
 * settled nets with no advance (transparent D follows D while EN is
 * High and holds while EN is Low; SR sets/resets/holds, S=R=1 refused
 * loudly). Advances never gate level Q.
 *
 * Two-phase (mirrors signals: generators then sums): phase 1 is
 * combinational eval at time T with OLD Qs plus block inputs; the level
 * settle resolves every latch's inputs from phase 1, in slot order
 * (committed in slot order — parallel update); every edge flop's D
 * snapshots from the level-settled phase 1, in slot order; all Qs commit
 * from snapshots, in slot order (parallel update — no ordering
 * dependency, so swaps and counters behave synchronously); phase 2 is
 * combinational re-eval with NEW Qs for block outputs (zero-delay
 * settling, so outputs are end-of-step values at T+1).
 *
 * Rules: rising-edge only for edge kinds; full-control DFFs (5 inputs:
 * D/CLK/RST/SET/EN) resolve with total priority RST > SET > EN-hold > D
 * (reset wins simultaneous RST+SET); JK resolves its excitation table
 * (J=0,K=0 hold / J=0,K=1 reset / J=1,K=0 set / J=1,K=1 toggle from
 * pre-edge Q), T holds on 0 and toggles on 1; RST/SET are asynchronous
 * (see apply_async_controls); EN gates only the edge capture;
 * unconnected RST/SET read inactive (kLow), unconnected EN reads
 * asserted (kHigh). Bare 2-input flops behave exactly as before
 * (controls absent read inactive). X/Z rejected loudly: block inputs
 * must be kLow/kHigh (same rule as solve()), flop Qs are always
 * kLow/kHigh by construction (power-on kLow, commits only write
 * snapshots resolved to kLow/kHigh); any other value fails without
 * committing.
 *
 * solve()/eval_block_word() stay combinational-only: do not call them on
 * nets with sequential gates — topo::rebuild(kAll) sees ALL edges and
 * fails loudly on flop feedback loops by design; use topo::validate
 * with EdgeFilter::kCombOnly plus seq_step() instead.
 *
 * Dependency rules (docs/logic.md section 4): header-only,
 * allocation-free, exception-free; pool workspace (`ws_*` spans, no
 * fixed stack), no syscalls, no iostream. Determinism: slot-order snapshot
 * plus slot-order commit; sync via init_seq_state is idempotent, so
 * calling seq_step every step (or sporadically) keeps state. Logic is a
 * standalone domain: this header must never depend on CLI types.
 * Consumers: the CLI step/poke/peek verbs, the SequentialSolver engine
 * path, and the test_logic_seq_step suite.
 *
 * Capacity (015): the pool is dynamic (see types.hpp) — every scan and
 * init here is bounded by `slot_count(nl)`, never by the loud
 * `kMaxGates` cap. The stepper entries are plain (non-template)
 * functions over `Netlist`/`SeqState` with caller-owned scratch (the
 * `SeqScratch` bundle plus the `topo::TopoScratch` bundle); the
 * fewer-param wrappers below keep today's signatures over any pool by
 * growing the pool workspace (`ensure_workspace` plus the `ws_*`
 * spans) instead of fixed stack arrays. Caller-owned order buffers
 * (`comb_order`) must hold at least pool-size slots. The 4-arg
 * seq_step (outputs discarded) borrows `ws_next_q` as its discard
 * buffer (next-Q values are dead after the commit phase, so the
 * end-of-step output scan cannot observe them).
 *
 * Dependency rules (docs/logic.md section 4): header-only,
 * allocation-free, exception-free, no syscalls, no iostream.
 * Determinism: slot-order snapshot plus slot-order commit; sync via
 * init_seq_state is idempotent. Logic is a standalone domain: this
 * header must never depend on CLI types. Consumers: the CLI
 * step/poke/peek verbs, the SequentialSolver engine path, and the
 * test_logic_seq_step suite.
 */

#include "wengine/core/status.hpp"
#include "wengine/logic/eval.hpp"
#include "wengine/logic/netlist.hpp"
#include "wengine/logic/optimize_barrier.hpp"
#include "wengine/logic/seq_state.hpp"
#include "wengine/logic/topo.hpp"
#include "wengine/logic/types.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace wengine::logic {

namespace detail {

/**
 * @brief Subset-step membership (T013: selective clock domains): a
 *        null member list means every live flop (the global edge);
 *        otherwise only listed slots snapshot/commit.
 *
 * Linear scan — member counts are small and stepping is CLI-paced.
 *
 * @param[in] members Member slots, or nullptr for every live flop.
 * @param[in] n_members Member count (ignored when members is nullptr).
 * @param[in] slot Slot to test.
 * @return True when slot steps on this edge.
 */
[[nodiscard]] inline bool is_step_member(const Slot *members, std::size_t n_members, Slot slot) noexcept {
  if (members == nullptr) {
    return true;
  }
  for (std::size_t i = 0; i < n_members; ++i) {
    if (members[i] == slot) {
      return true;
    }
  }
  return false;
}

/**
 * @brief Phase 1 combinational eval at time T with OLD Qs (shared):
 *        seeds gate_out with flop Qs, then settles combinational gates
 *        in the deterministic comb order.
 *
 * Used by the stepper snapshot and the async-apply path alike so wired
 * RST/SET resolve identically.
 *
 * @param[in] nl Netlist to evaluate.
 * @param[in] st Clocked state providing OLD Qs.
 * @param[in] resolved Pre-resolved dangling values from fill_resolved.
 * @param[out] gate_out Per-slot settled outputs (caller-owned).
 * @param[out] comb_order Combinational order from compile_comb_order
 *             (caller-owned, pool-sized).
 * @param[out] n_comb Number of combinational slots emitted.
 * @param[in] diag Optional contention channel (nullptr = no counting).
 * @param[in] ts Caller-owned Kahn scratch (see TopoScratch).
 * @return kOk on success; kInvalidArgument on combinational cycles,
 *         dangling drivers, X/Z inputs, or undersized scratch.
 */
[[nodiscard]] inline core::Status eval_comb_phase1(const Netlist &nl, const SeqState &st,
                                                   const LogicValue *resolved, LogicValue *gate_out,
                                                   Slot *comb_order, std::size_t &n_comb,
                                                   SolveDiag *diag, const topo::TopoScratch &ts) noexcept {
  if (!topo::compile_comb_order(nl, comb_order, n_comb, ts.indeg, ts.queue)) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  for (std::size_t s = 0; s < slot_count(nl); ++s) {
    gate_out[s] = LogicValue::kLow;
  }
  for (Slot s = 0; s < slot_count(nl); ++s) {
    if (nl.slots[s].live && is_sequential_kind(nl.slots[s].kind)) {
      gate_out[s] = st.q[s];
    }
  }
  LogicValue ins[kMaxInputs];
  for (std::size_t oi = 0; oi < n_comb; ++oi) {
    const Slot s = comb_order[oi];
    const Gate &g = nl.slots[s];
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      if (!resolve_input(nl, s, i, gate_out, resolved, ins[i], diag)) {
        return core::Status::failure(core::StatusCode::kInvalidArgument);
      }
    }
    gate_out[s] = eval_gate(g.kind, ins, g.input_count);
  }
  return core::Status::success();
}

/**
 * @brief DFF control-pin resolution (full-control flops).
 *
 * Wired pins resolve the net (with NQ polarity); dangling pins read the
 * pre-resolved block value (poke value, tie share, or dangling Low —
 * except ignored dangling EN, which reads kHigh: FR-008 unconnected
 * enable is asserted). Pins absent from the flop's fan-in read the inactive
 * default (RST/SET kLow, EN kHigh). X/Z anywhere fails loudly. Pure
 * readout: no state mutated.
 *
 * @param[in] nl Netlist holding drivers and ignore marks.
 * @param[in] slot Flop slot.
 * @param[in] input Control input index (2 RST, 3 SET, 4 EN).
 * @param[in] gate_out Per-slot settled gate outputs.
 * @param[in] resolved Pre-resolved dangling values.
 * @param[out] out Resolved control value.
 * @param[in] diag Optional contention channel (nullptr = no counting).
 * @return True on success; false on X/Z values.
 */
[[nodiscard]] inline bool resolve_control(const Netlist &nl, Slot slot, std::uint8_t input,
                                          const LogicValue *gate_out, const LogicValue *resolved,
                                          LogicValue &out, SolveDiag *diag = nullptr) noexcept {
  const Gate &g = nl.slots[slot];
  if (input >= g.input_count) {
    out = (g.kind == GateKind::kDff && input == 4) ? LogicValue::kHigh : LogicValue::kLow;
    return true;
  }
  if (g.drivers[input] == kNoDriver && (g.ignored_mask & static_cast<std::uint8_t>(1u << input)) != 0 &&
      g.kind == GateKind::kDff && input == 4) {
    out = LogicValue::kHigh;
    return true;
  }
  // Shared scalar funnel (BlockInterface seam): wired/dangling/ignored
  // dispatch plus NQ polarity live in resolve_input; the EN-default arms
  // above are the only control-specific rules.
  return resolve_input(nl, slot, input, gate_out, resolved, out, diag);
}

/**
 * @brief True for the level-sensitive kinds (DLATCH and SRLATCH),
 *        resolved in the level settle instead of the edge snapshot.
 *
 * Capacity-free: kinds never scale.
 *
 * @param[in] k Kind to test.
 * @return True for kDlatch/kSrlatch, false otherwise.
 */
[[nodiscard]] inline bool is_level_kind(GateKind k) noexcept {
  return k == GateKind::kDlatch || k == GateKind::kSrlatch;
}

/**
 * @brief Level settle for DLATCH (D + EN) and SRLATCH (S + R).
 *
 * The edge snapshot never touches these; they resolve here, in slot
 * order, from the phase-1 settled nets plus OLD Q, then commit in slot
 * order (parallel update among latches, same discipline as the edge
 * commit). Resolve-all-then-commit: any X/Z (or S=R=1) fails with state
 * untouched. Committed Qs are re-seeded into gate_out so edge flops
 * wired from latch Q see end-of-level values.
 *
 * @param[in] nl Netlist holding the latches.
 * @param[out] st Pool receiving the committed latch Qs.
 * @param[in] resolved Pre-resolved dangling values.
 * @param[out] gate_out Phase-1 outputs, re-seeded with new latch Qs.
 * @param[out] next_q Caller-owned snapshot scratch (pool-size entries).
 * @param[in] diag Optional contention channel (nullptr = no counting).
 * @return kOk on success; kInvalidArgument on X/Z latch inputs or an
 *         illegal S=R=1 net (state untouched) or undersized scratch.
 */
[[nodiscard]] inline core::Status apply_level_resolve(const Netlist &nl, SeqState &st,
                                                      const LogicValue *resolved, LogicValue *gate_out,
                                                      std::span<LogicValue> next_q,
                                                      SolveDiag *diag = nullptr) noexcept {
  if (next_q.size() < slot_count(nl)) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  for (std::size_t s = 0; s < slot_count(nl); ++s) {
    next_q[s] = LogicValue::kLow;
  }
  for (Slot s = 0; s < slot_count(nl); ++s) {
    const Gate &g = nl.slots[s];
    if (!g.live || !is_level_kind(g.kind)) {
      continue;
    }
    LogicValue a = LogicValue::kLow;
    LogicValue b = LogicValue::kLow;
    if (!resolve_input(nl, s, 0, gate_out, resolved, a, diag)) {
      return core::Status::failure(core::StatusCode::kInvalidArgument);
    }
    if (!resolve_input(nl, s, 1, gate_out, resolved, b, diag)) {
      return core::Status::failure(core::StatusCode::kInvalidArgument);
    }
    if ((a != LogicValue::kLow && a != LogicValue::kHigh) ||
        (b != LogicValue::kLow && b != LogicValue::kHigh)) {
      return core::Status::failure(core::StatusCode::kInvalidArgument);
    }
    if (g.kind == GateKind::kDlatch) {
      // EN=1 transparent (Q follows D), EN=0 frozen (Q holds, D ignored).
      next_q[s] = (b == LogicValue::kHigh) ? a : st.q[s];
    } else {
      // SRLATCH (US2): S=1,R=0 set; S=0,R=1 reset; S=R=0 hold (memory);
      // S=R=1 is illegal — refused loudly with nothing committed, on
      // every entry path (direct drive, wired comb, tied leader, load).
      const bool set = (a == LogicValue::kHigh);
      const bool rst = (b == LogicValue::kHigh);
      if (set && rst) {
        return core::Status::failure(core::StatusCode::kInvalidArgument);
      }
      if (set) {
        next_q[s] = LogicValue::kHigh;
      } else if (rst) {
        next_q[s] = LogicValue::kLow;
      } else {
        next_q[s] = st.q[s];
      }
    }
  }
  for (Slot s = 0; s < slot_count(nl); ++s) {
    if (nl.slots[s].live && is_level_kind(nl.slots[s].kind)) {
      st.q[s] = next_q[s];
      gate_out[s] = next_q[s]; // seed phase-1-with-level-Qs for edge use
    }
  }
  return core::Status::success();
}

} // namespace detail

/**
 * @brief Single rising edge over a flop subset (T013: selective clock
 *        domains).
 *
 * Null members steps every live flop (the global edge); otherwise only
 * listed slots snapshot/commit, in slot order (parallel update among
 * members, same as the global edge). Combinational settle (phase 1 and
 * phase 2) is always whole-netlist, so observed outputs are end-of-step
 * values. Otherwise the file contract above holds unchanged: validation
 * first, untouched state on failure, no Netlist topology mutation.
 *
 * @param[out] nl Netlist to step (topology validated, never mutated).
 * @param[out] st Pool synced (via init_seq_state) and committed.
 * @param[in] block_inputs Input values in interface order.
 * @param[out] block_outputs End-of-step values in interface order.
 * @param[in] members Member slots to step, or nullptr for every live flop.
 * @param[in] n_members Member count (ignored when members is nullptr).
 * @param[in] diag Optional contention channel (nullptr = no counting).
 * @param[in] ss Caller-owned step scratch (see SeqScratch).
 * @param[in] ts Caller-owned Kahn scratch (see TopoScratch).
 * @return kOk on success; kInvalidArgument on topology errors
 *         (combinational cycle under kCombOnly), size mismatches, X/Z
 *         inputs or state, S=R=1 nets (state untouched), or undersized
 *         scratch.
 */
[[nodiscard]] inline core::Status seq_step_filtered(Netlist &nl, SeqState &st,
                                                     std::span<const LogicValue> block_inputs,
                                                     std::span<LogicValue> block_outputs,
                                                     const Slot *members, std::size_t n_members,
                                                     SolveDiag *diag, const SeqScratch &ss,
                                                     const topo::TopoScratch &ts) noexcept {
  if (!ss.validate(slot_count(nl)).ok()) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  init_seq_state(st, nl);
  if (!topo::validate(nl, EdgeFilter::kCombOnly, ts).ok()) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  const std::size_t n_in = block_input_count(nl);
  const std::size_t n_out = block_output_count(nl);
  if (block_inputs.size() != n_in || block_outputs.size() != n_out) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  // Defensive: Qs are Low/High by construction (power-on Low, commits write
  // snapshots only). A corrupted pool (or a future tri-state Q) fails loudly
  // before any eval reads it.
  for (Slot s = 0; s < slot_count(nl); ++s) {
    if (st.valid[s] != 0 && st.q[s] != LogicValue::kLow && st.q[s] != LogicValue::kHigh) {
      return core::Status::failure(core::StatusCode::kInvalidArgument);
    }
  }
  // Dangling-input resolution (same order/rules as solve): leaders consume
  // block_inputs in slot order, X/Z rejected; ignored pins tie to kLow.
  if (!fill_resolved(nl, block_inputs, ss.resolved).ok()) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }

  std::size_t n_comb = 0;
  if (!detail::eval_comb_phase1(nl, st, ss.resolved.data(), ss.gate_out.data(), ss.comb_order.data(), n_comb, diag, ts).ok()) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }

  // Level settle (004-sr-d-latches): latches resolve from the phase-1
  // nets and commit before any edge snapshot, so edge flops wired from
  // latch Q see end-of-level values. Fails with state untouched on X/Z
  // (or, from US2, S=R=1). Unconditional: domain filtering is edge-only.
  if (!detail::apply_level_resolve(nl, st, ss.resolved.data(), ss.gate_out.data(), ss.next_q, diag).ok()) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }

  // Snapshot: D (input 0) for every edge flop + CLK (input 1) validation,
  // in slot order. Level kinds (DLATCH/SRLATCH) are exempt — settled
  // above; advances never gate level Q. Full-control DFFs additionally
  // resolve RST/SET/EN with priority RST > SET > EN-hold > D (reset wins
  // RST+SET; EN=Low holds Q). D/CLK resolve first so X/Z there fails
  // loudly even for held flops. X/Z anywhere fails before any commit
  // (state untouched).
  for (std::size_t s = 0; s < slot_count(nl); ++s) {
    ss.next_q[s] = LogicValue::kLow;
  }
  for (Slot s = 0; s < slot_count(nl); ++s) {
    const Gate &g = nl.slots[s];
    if (!g.live || !is_sequential_kind(g.kind)) {
      continue;
    }
    if (detail::is_level_kind(g.kind)) {
      continue; // level-settled above; advances never gate level Q
    }
    if (!detail::is_step_member(members, n_members, s)) {
      continue; // selective domain: this flop holds its Q this edge
    }
    LogicValue d = LogicValue::kLow;
    LogicValue clk = LogicValue::kLow;
    if (!resolve_input(nl, s, 0, ss.gate_out.data(), ss.resolved.data(), d, diag)) {
      return core::Status::failure(core::StatusCode::kInvalidArgument);
    }
    if (!resolve_input(nl, s, 1, ss.gate_out.data(), ss.resolved.data(), clk, diag)) {
      return core::Status::failure(core::StatusCode::kInvalidArgument);
    }
    if ((d != LogicValue::kLow && d != LogicValue::kHigh) ||
        (clk != LogicValue::kLow && clk != LogicValue::kHigh)) {
      return core::Status::failure(core::StatusCode::kInvalidArgument);
    }
    if (g.kind == GateKind::kT) {
      // T flop (003-jk-t-flops): T=High toggles pre-edge Q, T=Low holds
      // it. CLK (input 1) validated above; per the SEQ4 rule the edge
      // itself is the call, so the value does not gate the update.
      if (d == LogicValue::kHigh) {
        ss.next_q[s] = (st.q[s] == LogicValue::kHigh) ? LogicValue::kLow : LogicValue::kHigh;
      } else {
        ss.next_q[s] = st.q[s]; // hold: T ignored this edge
      }
      continue;
    }
    if (g.kind == GateKind::kJk) {
      // JK flop (003-jk-t-flops): the shared prelude resolved J (input 0)
      // into d and K (input 1) into clk, both validated Low/High above.
      // Resolve the clock (input 2) here; per the SEQ4 rule it is
      // validated, not gating. Excitation resolves from pre-edge Q.
      LogicValue jk_clk = LogicValue::kLow;
      if (!resolve_input(nl, s, 2, ss.gate_out.data(), ss.resolved.data(), jk_clk, diag)) {
        return core::Status::failure(core::StatusCode::kInvalidArgument);
      }
      if (jk_clk != LogicValue::kLow && jk_clk != LogicValue::kHigh) {
        return core::Status::failure(core::StatusCode::kInvalidArgument);
      }
      const bool j = (d == LogicValue::kHigh);
      const bool k = (clk == LogicValue::kHigh);
      if (!j && !k) {
        ss.next_q[s] = st.q[s]; // hold
      } else if (j && !k) {
        ss.next_q[s] = LogicValue::kHigh; // set
      } else if (!j && k) {
        ss.next_q[s] = LogicValue::kLow; // reset
      } else if (st.q[s] == LogicValue::kHigh) {
        ss.next_q[s] = LogicValue::kLow; // toggle
      } else {
        ss.next_q[s] = LogicValue::kHigh; // toggle
      }
      continue;
    }
    if (g.kind == GateKind::kDff && g.input_count > 2) {
      LogicValue rst = LogicValue::kLow;
      LogicValue set = LogicValue::kLow;
      LogicValue en = LogicValue::kHigh;
      if (!detail::resolve_control(nl, s, 2, ss.gate_out.data(), ss.resolved.data(), rst, diag) ||
          !detail::resolve_control(nl, s, 3, ss.gate_out.data(), ss.resolved.data(), set, diag) ||
          !detail::resolve_control(nl, s, 4, ss.gate_out.data(), ss.resolved.data(), en, diag)) {
        return core::Status::failure(core::StatusCode::kInvalidArgument);
      }
      if (rst == LogicValue::kHigh) {
        ss.next_q[s] = LogicValue::kLow;
        continue;
      }
      if (set == LogicValue::kHigh) {
        ss.next_q[s] = LogicValue::kHigh;
        continue;
      }
      if (en == LogicValue::kLow) {
        ss.next_q[s] = st.q[s]; // synchronous hold: D ignored this edge
        continue;
      }
    }
    // Edge capture for bare DFFs (004: DLATCH/SRLATCH are level-exempt
    // and never reach here). CLK validated above; the edge itself is the
    // call. T/JK flops take their own branches earlier (003-jk-t-flops).
    ss.next_q[s] = d;
  }

  // Commit: parallel Q update in slot order (members only for subsets;
  // level kinds already committed by the level settle and stay exempt).
  for (Slot s = 0; s < slot_count(nl); ++s) {
    if (nl.slots[s].live && is_sequential_kind(nl.slots[s].kind) && !detail::is_level_kind(nl.slots[s].kind) && detail::is_step_member(members, n_members, s)) {
      st.q[s] = ss.next_q[s];
      ss.gate_out[s] = ss.next_q[s]; // seed phase 2 with NEW Qs
    }
  }

  // Phase 2: zero-delay re-settle with NEW Qs for end-of-step outputs.
  LogicValue ins[kMaxInputs];
  for (std::size_t oi = 0; oi < n_comb; ++oi) {
    const Slot s = ss.comb_order[oi];
    const Gate &g = nl.slots[s];
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      if (!resolve_input(nl, s, i, ss.gate_out.data(), ss.resolved.data(), ins[i], diag)) {
        return core::Status::failure(core::StatusCode::kInvalidArgument);
      }
    }
    ss.gate_out[s] = eval_gate(g.kind, ins, g.input_count);
  }

  std::size_t out_cursor = 0;
  for (Slot s = 0; s < slot_count(nl); ++s) {
    const Gate &g = nl.slots[s];
    if (!g.live) {
      continue;
    }
    if (g.output_fanout == 0 && !g.ignore_output) {
      if (out_cursor >= block_outputs.size()) {
        return core::Status::failure(core::StatusCode::kInvalidArgument);
      }
      block_outputs[out_cursor++] = ss.gate_out[s];
    }
    if (is_sequential_kind(g.kind) && g.nq_fanout == 0 && !g.ignore_nq_output) {
      if (out_cursor >= block_outputs.size()) {
        return core::Status::failure(core::StatusCode::kInvalidArgument);
      }
      block_outputs[out_cursor++] = apply_polarity(ss.gate_out[s], 1);
    }
  }
  if (out_cursor != block_outputs.size()) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  return core::Status::success();
}

/**
 * @brief Single rising edge over a flop subset.
 *
 * Workspace-backed convenience entry: grows the pool's step/Kahn
 * scratch to the pool size, then runs the core above. (No noexcept:
 * workspace growth may allocate.)
 *
 * @param[out] nl Netlist to step (topology validated, never mutated;
 *             workspace grows).
 * @param[out] st Pool synced (via init_seq_state) and committed.
 * @param[in] block_inputs Input values in interface order.
 * @param[out] block_outputs End-of-step values in interface order.
 * @param[in] members Member slots to step, or nullptr for every live flop.
 * @param[in] n_members Member count (ignored when members is nullptr).
 * @param[in] diag Optional contention channel (nullptr = no counting).
 * @return kOk on success; kInvalidArgument on topology errors
 *         (combinational cycle under kCombOnly), size mismatches, X/Z
 *         inputs or state, or S=R=1 nets (state untouched).
 */
[[nodiscard]] inline core::Status seq_step_filtered(Netlist &nl, SeqState &st, std::span<const LogicValue> block_inputs,
                                                     std::span<LogicValue> block_outputs, const Slot *members,
                                                     std::size_t n_members, SolveDiag *diag = nullptr) {
  const std::size_t n = slot_count(nl);
  ensure_workspace(nl, n);
  if (diag != nullptr && diag->per_net.size() < n * kMaxInputs) {
    diag->per_net.assign(n * kMaxInputs, 0);
  }
  const SeqScratch ss{std::span<LogicValue>(nl.ws_resolved.data(), n * kMaxInputs),
                      std::span<Slot>(nl.ws_comb_order.data(), n),
                      std::span<LogicValue>(nl.ws_gate_out.data(), n), std::span<LogicValue>(nl.ws_next_q.data(), n),
                      std::span<LogicValue>(nl.ws_forced_q.data(), n),
                      std::span<std::uint8_t>(nl.ws_force.data(), n)};
  const topo::TopoScratch ts{std::span<Slot>(nl.ws_order.data(), n), std::span<Slot>(nl.ws_levels.data(), n),
                             std::span<std::uint8_t>(nl.ws_indeg.data(), n), std::span<Slot>(nl.ws_queue.data(), n)};
  return seq_step_filtered(nl, st, block_inputs, block_outputs, members, n_members, diag, ss, ts);
}

/**
 * @brief Single rising edge over the whole netlist (global clock):
 *        every live flop steps. Exact SEQ4 semantics, via the filtered
 *        core.
 *
 * @param[out] nl Netlist to step.
 * @param[out] st Pool synced and committed.
 * @param[in] block_inputs Input values in interface order.
 * @param[out] block_outputs End-of-step values in interface order.
 * @param[in] diag Optional contention channel (nullptr = no counting).
 * @param[in] ss Caller-owned step scratch (see SeqScratch).
 * @param[in] ts Caller-owned Kahn scratch (see TopoScratch).
 * @return kOk on success; kInvalidArgument on any seq_step_filtered
 *         failure (state untouched).
 */
[[nodiscard]] inline core::Status seq_step(Netlist &nl, SeqState &st,
                                           std::span<const LogicValue> block_inputs,
                                           std::span<LogicValue> block_outputs, SolveDiag *diag,
                                           const SeqScratch &ss,
                                           const topo::TopoScratch &ts) noexcept {
  return seq_step_filtered(nl, st, block_inputs, block_outputs, nullptr, 0, diag, ss, ts);
}

/**
 * @brief Single rising edge over the whole netlist (global clock).
 *
 * Workspace-backed convenience entry: grows the pool's step/Kahn
 * scratch to the pool size, then runs the core above. (No noexcept:
 * workspace growth may allocate.)
 *
 * @param[out] nl Netlist to step (workspace grows).
 * @param[out] st Pool synced and committed.
 * @param[in] block_inputs Input values in interface order.
 * @param[out] block_outputs End-of-step values in interface order.
 * @param[in] diag Optional contention channel (nullptr = no counting).
 * @return kOk on success; kInvalidArgument on any seq_step_filtered
 *         failure (state untouched).
 */
[[nodiscard]] inline core::Status seq_step(Netlist &nl, SeqState &st, std::span<const LogicValue> block_inputs, std::span<LogicValue> block_outputs, SolveDiag *diag = nullptr) {
  const std::size_t n = slot_count(nl);
  ensure_workspace(nl, n);
  if (diag != nullptr && diag->per_net.size() < n * kMaxInputs) {
    diag->per_net.assign(n * kMaxInputs, 0);
  }
  const SeqScratch ss{std::span<LogicValue>(nl.ws_resolved.data(), n * kMaxInputs),
                      std::span<Slot>(nl.ws_comb_order.data(), n),
                      std::span<LogicValue>(nl.ws_gate_out.data(), n), std::span<LogicValue>(nl.ws_next_q.data(), n),
                      std::span<LogicValue>(nl.ws_forced_q.data(), n),
                      std::span<std::uint8_t>(nl.ws_force.data(), n)};
  const topo::TopoScratch ts{std::span<Slot>(nl.ws_order.data(), n), std::span<Slot>(nl.ws_levels.data(), n),
                             std::span<std::uint8_t>(nl.ws_indeg.data(), n), std::span<Slot>(nl.ws_queue.data(), n)};
  return seq_step(nl, st, block_inputs, block_outputs, diag, ss, ts);
}

/**
 * @brief Immediate async-apply for full-control DFFs
 *        (FR-002/FR-003): resolves RST/SET nets for every live 5-input
 *        DFF and forces Q per priority (RST to 0, else SET to 1), in
 *        slot order — taking effect with no clock advance.
 *
 * Session calls this after every control-net mutation (poke/wire/tie/load)
 * and at step top; it never touches cycle counters, EN, or D, and is a
 * success no-op on nets without full-control flops.
 * Resolve-all-then-commit: X/Z on any resolved control fails loudly with
 * state untouched. Same validation/size contract as seq_step_filtered.
 *
 * @param[out] nl Netlist holding the control nets.
 * @param[out] st Pool receiving forced Qs.
 * @param[in] block_inputs Input values in interface order.
 * @param[in] ss Caller-owned step scratch (see SeqScratch).
 * @param[in] ts Caller-owned Kahn scratch (see TopoScratch).
 * @return kOk on success (including no-op nets); kInvalidArgument on
 *         topology errors, size mismatches, X/Z controls (state
 *         untouched), or undersized scratch.
 */
[[nodiscard]] inline core::Status apply_async_controls(Netlist &nl, SeqState &st, std::span<const LogicValue> block_inputs,
                                                        const SeqScratch &ss, const topo::TopoScratch &ts) noexcept {
  if (!ss.validate(slot_count(nl)).ok()) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  init_seq_state(st, nl);
  if (!has_sequential_gates(nl)) {
    return core::Status::success(); // (011) Vacuous settle: no flops to
    // force, so shared-net contention (a resolution event) never refuses
    // authoring on combinational netlists. Callers validate topology on
    // their own path; only SeqState forcing lives here.
  }
  if (!topo::validate(nl, EdgeFilter::kCombOnly, ts).ok()) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  const std::size_t n_in = block_input_count(nl);
  if (block_inputs.size() != n_in) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  for (Slot s = 0; s < slot_count(nl); ++s) {
    if (st.valid[s] != 0 && st.q[s] != LogicValue::kLow && st.q[s] != LogicValue::kHigh) {
      return core::Status::failure(core::StatusCode::kInvalidArgument);
    }
  }
  if (!fill_resolved(nl, block_inputs, ss.resolved).ok()) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  std::size_t n_comb = 0;
  if (!detail::eval_comb_phase1(nl, st, ss.resolved.data(), ss.gate_out.data(), ss.comb_order.data(), n_comb, nullptr, ts).ok()) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  for (std::size_t s = 0; s < slot_count(nl); ++s) {
    ss.forced_q[s] = LogicValue::kLow;
    ss.force[s] = 0;
  }
  for (Slot s = 0; s < slot_count(nl); ++s) {
    const Gate &g = nl.slots[s];
    if (!g.live || !is_sequential_kind(g.kind) || g.kind != GateKind::kDff || g.input_count <= 2) {
      continue;
    }
    LogicValue rst = LogicValue::kLow;
    LogicValue set = LogicValue::kLow;
    if (!detail::resolve_control(nl, s, 2, ss.gate_out.data(), ss.resolved.data(), rst) || !detail::resolve_control(nl, s, 3, ss.gate_out.data(), ss.resolved.data(), set)) {
      return core::Status::failure(core::StatusCode::kInvalidArgument);
    }
    if (rst == LogicValue::kHigh) {
      ss.forced_q[s] = LogicValue::kLow;
      ss.force[s] = 1;
    } else if (set == LogicValue::kHigh) {
      ss.forced_q[s] = LogicValue::kHigh;
      ss.force[s] = 1;
    }
  }
  for (Slot s = 0; s < slot_count(nl); ++s) {
    if (ss.force[s]) {
      st.q[s] = ss.forced_q[s];
    }
  }
  return core::Status::success();
}

/**
 * @brief Immediate async-apply for full-control DFFs.
 *
 * Workspace-backed convenience entry: grows the pool's step/Kahn
 * scratch to the pool size, then runs the core above. (No noexcept:
 * workspace growth may allocate.)
 *
 * @param[out] nl Netlist holding the control nets (workspace grows).
 * @param[out] st Pool receiving forced Qs.
 * @param[in] block_inputs Input values in interface order.
 * @return kOk on success (including no-op nets); kInvalidArgument on
 *         topology errors, size mismatches, or X/Z controls (state
 *         untouched).
 */
[[nodiscard]] inline core::Status apply_async_controls(Netlist &nl, SeqState &st, std::span<const LogicValue> block_inputs) {
  const std::size_t n = slot_count(nl);
  ensure_workspace(nl, n);
  const SeqScratch ss{std::span<LogicValue>(nl.ws_resolved.data(), n * kMaxInputs),
                      std::span<Slot>(nl.ws_comb_order.data(), n),
                      std::span<LogicValue>(nl.ws_gate_out.data(), n), std::span<LogicValue>(nl.ws_next_q.data(), n),
                      std::span<LogicValue>(nl.ws_forced_q.data(), n),
                      std::span<std::uint8_t>(nl.ws_force.data(), n)};
  const topo::TopoScratch ts{std::span<Slot>(nl.ws_order.data(), n), std::span<Slot>(nl.ws_levels.data(), n),
                             std::span<std::uint8_t>(nl.ws_indeg.data(), n), std::span<Slot>(nl.ws_queue.data(), n)};
  return apply_async_controls(nl, st, block_inputs, ss, ts);
}

/**
 * @brief Immediate level-settle for DLATCH/SRLATCH
 *        (004-sr-d-latches): resolves latch nets and commits Q per the
 *        level rule (transparent D follows D while EN is High, frozen
 *        hold otherwise; SR holds, set/reset plus S=R=1 refusal), in
 *        slot order — taking effect with no clock advance.
 *
 * Session calls this after every control-net mutation (poke/wire/tie/load)
 * and at step top; it never touches cycle counters or edge-flop state,
 * and is a success no-op on nets without latches.
 * Resolve-all-then-commit: X/Z on any resolved latch input fails loudly
 * with state untouched. Same validation/size contract as
 * apply_async_controls.
 *
 * @param[out] nl Netlist holding the latch nets.
 * @param[out] st Pool receiving settled latch Qs.
 * @param[in] block_inputs Input values in interface order.
 * @param[in] ss Caller-owned step scratch (see SeqScratch).
 * @param[in] ts Caller-owned Kahn scratch (see TopoScratch).
 * @return kOk on success (including no-op nets); kInvalidArgument on
 *         topology errors, size mismatches, X/Z inputs, S=R=1 nets
 *         (state untouched), or undersized scratch.
 */
[[nodiscard]] inline core::Status apply_level_latches(Netlist &nl, SeqState &st,
                                                       std::span<const LogicValue> block_inputs,
                                                       const SeqScratch &ss,
                                                       const topo::TopoScratch &ts) noexcept {
  if (!ss.validate(slot_count(nl)).ok()) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  init_seq_state(st, nl);
  if (!has_sequential_gates(nl)) {
    return core::Status::success(); // (011) Vacuous settle: no latches to
    // settle (same rationale as apply_async_controls above).
  }
  if (!topo::validate(nl, EdgeFilter::kCombOnly, ts).ok()) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  const std::size_t n_in = block_input_count(nl);
  if (block_inputs.size() != n_in) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  for (Slot s = 0; s < slot_count(nl); ++s) {
    if (st.valid[s] != 0 && st.q[s] != LogicValue::kLow && st.q[s] != LogicValue::kHigh) {
      return core::Status::failure(core::StatusCode::kInvalidArgument);
    }
  }
  if (!fill_resolved(nl, block_inputs, ss.resolved).ok()) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  std::size_t n_comb = 0;
  if (!detail::eval_comb_phase1(nl, st, ss.resolved.data(), ss.gate_out.data(), ss.comb_order.data(), n_comb, nullptr, ts).ok()) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  return detail::apply_level_resolve(nl, st, ss.resolved.data(), ss.gate_out.data(), ss.next_q);
}

/**
 * @brief Immediate level-settle for DLATCH/SRLATCH.
 *
 * Workspace-backed convenience entry: grows the pool's step/Kahn
 * scratch to the pool size, then runs the core above. (No noexcept:
 * workspace growth may allocate.)
 *
 * @param[out] nl Netlist holding the latch nets (workspace grows).
 * @param[out] st Pool receiving settled latch Qs.
 * @param[in] block_inputs Input values in interface order.
 * @return kOk on success (including no-op nets); kInvalidArgument on
 *         topology errors, size mismatches, X/Z inputs, or S=R=1 nets
 *         (state untouched).
 */
[[nodiscard]] inline core::Status apply_level_latches(Netlist &nl, SeqState &st, std::span<const LogicValue> block_inputs) {
  const std::size_t n = slot_count(nl);
  ensure_workspace(nl, n);
  const SeqScratch ss{std::span<LogicValue>(nl.ws_resolved.data(), n * kMaxInputs),
                      std::span<Slot>(nl.ws_comb_order.data(), n),
                      std::span<LogicValue>(nl.ws_gate_out.data(), n), std::span<LogicValue>(nl.ws_next_q.data(), n),
                      std::span<LogicValue>(nl.ws_forced_q.data(), n),
                      std::span<std::uint8_t>(nl.ws_force.data(), n)};
  const topo::TopoScratch ts{std::span<Slot>(nl.ws_order.data(), n), std::span<Slot>(nl.ws_levels.data(), n),
                             std::span<std::uint8_t>(nl.ws_indeg.data(), n), std::span<Slot>(nl.ws_queue.data(), n)};
  return apply_level_latches(nl, st, block_inputs, ss, ts);
}

/**
 * @brief Step with block outputs discarded (state observed via SeqState
 *        Q / find_seq_serial). Computes the same end-of-step values;
 *        only the caller-visible copy is dropped.
 *
 * Workspace-backed thin wrapper over the 5-arg form: the discard
 * buffer borrows `ws_next_q` (next-Q values are dead after the commit
 * phase, so the output scan cannot observe them).
 *
 * @param[out] nl Netlist to step (workspace grows).
 * @param[out] st Pool synced and committed.
 * @param[in] block_inputs Input values in interface order.
 * @return kOk on success; kInvalidArgument on any seq_step failure
 *         (state untouched).
 */
[[nodiscard]] inline core::Status seq_step(Netlist &nl, SeqState &st, std::span<const LogicValue> block_inputs) {
  const std::size_t n = slot_count(nl);
  ensure_workspace(nl, n);
  const std::size_t n_out = block_output_count(nl);
  std::span<LogicValue> out_span(nl.ws_next_q.data(), n_out);
  return seq_step(nl, st, block_inputs, out_span);
}

} // namespace wengine::logic
