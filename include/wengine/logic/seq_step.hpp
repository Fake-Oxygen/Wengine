#pragma once

// Scalar single-step sequential eval (LOGIC-V2-SEQ4 + 004-sr-d-latches).
//
// Zero-delay stepper for edge-triggered DFF/JK/T plus level-sensitive
// DLATCH/SRLATCH, with deterministic end-of-step evaluation. One call
// advances every edge flop by one rising edge and re-settles every latch
// from the current nets:
//   edge kinds (DFF/JK/T): CLK pins are validated Low/High but do not
//   gate; the edge itself is the call (same as the signals two-phase
//   convention where generators evaluate at time T and sums commit at T+1).
//   level kinds (DLATCH/SRLATCH): Q resolves immediately from the settled
//   nets with no advance (transparent D follows D while EN is High and
//   holds while EN is Low; SR sets/resets/holds, S=R=1 refused loudly).
//   Advances never gate level Q.
//
// Two-phase (mirrors signals: generators then sums):
//   Phase 1: combinational eval at time T with OLD Qs + block inputs.
//   Level settle: every latch's inputs resolved from phase 1, in slot
//     order; DLATCH Q follows/holds, SR holds (US2 adds set/reset plus
//     the S=R=1 refusal). Committed in slot order (parallel update).
//   Snapshot: every edge flop's D (input 0) resolved from the
//     level-settled phase 1, in slot order.
//   Commit: all Qs updated from snapshots, in slot order (parallel update —
//     no ordering dependency, swaps and counters behave synchronously).
//   Phase 2: combinational re-eval with NEW Qs for block outputs (zero-delay
//     settling, so outputs are end-of-step values at T+1).
//
// Rules (SEQ4 + 004):
// - Rising-edge only for edge kinds. CLK (DFF input 1, JK input 2, T
//   input 1) must resolve to kLow/kHigh; HighZ/Unknown fail loudly. The
//   value does not gate the update. DLATCH/SR never snapshot from an
//   edge (level-exempt); their frozen Q holds across any number of
//   advances while a transparent D latch re-tracks D at every step top.
// - DLATCH (004): EN=1 transparent (Q follows D), EN=0 frozen (Q holds,
//   D ignored). Resolved in the level settle, visible with no advance.
// - SRLATCH (004, US2): S/R level arm lands with set/reset plus the loud
//   S=R=1 refusal; until then the level settle validates S/R and holds Q.
// - Full-control DFFs (5 inputs: D/CLK/RST/SET/EN) resolve with total
//   priority RST > SET > EN-hold > D (reset wins simultaneous RST+SET).
//   JK/T resolve their excitation tables instead (003-jk-t-flops):
//   JK J=0,K=0 hold / J=0,K=1 reset / J=1,K=0 set / J=1,K=1 toggle from
//   pre-edge Q; T=0 hold, T=1 toggle.
//   RST/SET are asynchronous (see apply_async_controls below); EN gates
//   only the edge capture. Unconnected RST/SET read inactive (kLow);
//   unconnected EN reads asserted (kHigh, FR-008). Bare 2-input flops
//   behave exactly as before (controls absent → inactive).
// - X/Z rejected loudly: block inputs must be kLow/kHigh (same rule as
//   solve()), flop Qs are always kLow/kHigh by construction (power-on kLow,
//   commits only write snapshots resolved to kLow/kHigh); any other value
//   fails without committing.
// - Determinism: slot-order snapshot + slot-order commit; sync via
//   init_seq_state is idempotent, so calling seq_step every step (or
//   sporadically) keeps state. No allocation, no exceptions, noexcept.
// - solve()/eval_block_word() stay combinational-only (untouched by SEQ4):
//   do not call them on nets with sequential gates — ensure_topology sees
//   ALL edges and fails loudly on flop feedback loops by design; use
//   validate_sequential() + seq_step() instead.
//
// Header-only, allocation-free, exception-free, noexcept throughout.
// Fixed stack arrays (kMaxGates bounds), no syscalls, no iostream.

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

// Subset-step membership (T013): a null member list means every live flop
// (the global SEQ4 edge); otherwise only listed slots snapshot/commit.
// Linear scan — member counts are small and stepping is CLI-paced.
[[nodiscard]] inline bool is_step_member(const std::uint16_t *members, std::size_t n_members, std::uint16_t slot) noexcept {
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

// Resolve one gate input to a Low/High value. Wired inputs read gate_out
// (old-Q phase or new-Q phase); dangling inputs read the pre-resolved
// block vector (leaders only — caller fans ties out first); ignored
// dangling pins tie to kLow (netlist rule). Returns false on X/Z or on a
// resolve_net refusal (wired X/Z driver) — loud, never silent.
[[nodiscard]] inline bool resolve_step_input(const Netlist &nl, std::uint16_t slot, std::uint8_t input,
                                            const LogicValue *gate_out, const LogicValue *resolved,
                                            LogicValue &out) noexcept {
  const Gate &g = nl.gates[slot];
  const std::uint16_t drv = g.drivers[input];
  if (drv != kNoDriver) {
    if (drv >= kMaxGates) {
      return false;
    }
    LogicValue sunk = LogicValue::kLow;
    if (!resolve_net(&gate_out[drv], 1, sunk)) {
      return false;
    }
    // NQ source: the sink reads the negated value (T005).
    out = apply_polarity(sunk, nl.driver_inv[static_cast<std::size_t>(slot) * kMaxInputs + input]);
    return true;
  }
  if ((g.ignored_mask & static_cast<std::uint8_t>(1u << input)) != 0) {
    out = LogicValue::kLow;
    return true;
  }
  const LogicValue v =
      resolved[static_cast<std::size_t>(slot) * kMaxInputs + static_cast<std::size_t>(input)];
  if (v != LogicValue::kLow && v != LogicValue::kHigh) {
    return false;
  }
  out = v;
  return true;
}

// Phase 1 combinational eval at time T with OLD Qs (shared): seeds
// gate_out with flop Qs, then settles combinational gates in the
// deterministic comb order. Used by the stepper snapshot and the
// async-apply path alike so wired RST/SET resolve identically.
[[nodiscard]] inline core::Status eval_comb_phase1(const Netlist &nl, const SeqState &st, const LogicValue *resolved,
                                                  LogicValue *gate_out, std::uint16_t *comb_order,
                                                  std::size_t &n_comb) noexcept {
  if (!topo::compile_comb_order(nl, comb_order, n_comb)) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  for (std::size_t s = 0; s < kMaxGates; ++s) {
    gate_out[s] = LogicValue::kLow;
  }
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    if (nl.gates[s].live && is_sequential_kind(nl.gates[s].kind)) {
      gate_out[s] = st.q[s];
    }
  }
  LogicValue ins[kMaxInputs];
  for (std::size_t oi = 0; oi < n_comb; ++oi) {
    const std::uint16_t s = comb_order[oi];
    const Gate &g = nl.gates[s];
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      if (!resolve_step_input(nl, s, i, gate_out, resolved, ins[i])) {
        return core::Status::failure(core::StatusCode::kInvalidArgument);
      }
    }
    gate_out[s] = eval_gate(g.kind, ins, g.input_count);
  }
  return core::Status::success();
}

// DFF control-pin resolution (full-control flops). Wired pins resolve the
// net (with NQ polarity); dangling pins read the pre-resolved block value
// (poke value, tie share, or dangling Low — except ignored dangling EN,
// which reads kHigh: FR-008 unconnected enable is asserted). Pins absent
// from the flop's fan-in read the inactive default (RST/SET kLow,
// EN kHigh). X/Z anywhere fails loudly. Pure readout: no state mutated.
[[nodiscard]] inline bool resolve_control(const Netlist &nl, std::uint16_t slot, std::uint8_t input,
                                         const LogicValue *gate_out, const LogicValue *resolved,
                                         LogicValue &out) noexcept {
  const Gate &g = nl.gates[slot];
  if (input >= g.input_count) {
    out = (g.kind == GateKind::kDff && input == 4) ? LogicValue::kHigh : LogicValue::kLow;
    return true;
  }
  if (g.drivers[input] == kNoDriver && (g.ignored_mask & static_cast<std::uint8_t>(1u << input)) != 0 &&
      g.kind == GateKind::kDff && input == 4) {
    out = LogicValue::kHigh;
    return true;
  }
  if (g.drivers[input] != kNoDriver) {
    return resolve_step_input(nl, slot, input, gate_out, resolved, out);
  }
  const LogicValue v = resolved[static_cast<std::size_t>(slot) * kMaxInputs + static_cast<std::size_t>(input)];
  if (v != LogicValue::kLow && v != LogicValue::kHigh) {
    return false;
  }
  out = v;
  return true;
}

// Level-sensitive kinds (004-sr-d-latches): DLATCH (D + EN) and SRLATCH
// (S + R). The edge snapshot never touches these; they resolve here, in
// slot order, from the phase-1 settled nets + OLD Q, then commit in slot
// order (parallel update among latches, same discipline as the edge
// commit). Resolve-all-then-commit: any X/Z (or, from US2, S=R=1)
// fails with state untouched. Committed Qs are re-seeded into gate_out
// so edge flops wired from latch Q see end-of-level values.
[[nodiscard]] inline bool is_level_kind(GateKind k) noexcept {
  return k == GateKind::kDlatch || k == GateKind::kSrlatch;
}

[[nodiscard]] inline core::Status apply_level_resolve(const Netlist &nl, SeqState &st, const LogicValue *resolved,
                                                     LogicValue *gate_out) noexcept {
  LogicValue next_q[kMaxGates];
  for (std::size_t s = 0; s < kMaxGates; ++s) {
    next_q[s] = LogicValue::kLow;
  }
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    const Gate &g = nl.gates[s];
    if (!g.live || !is_level_kind(g.kind)) {
      continue;
    }
    LogicValue a = LogicValue::kLow;
    LogicValue b = LogicValue::kLow;
    if (!resolve_step_input(nl, s, 0, gate_out, resolved, a)) {
      return core::Status::failure(core::StatusCode::kInvalidArgument);
    }
    if (!resolve_step_input(nl, s, 1, gate_out, resolved, b)) {
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
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    if (nl.gates[s].live && is_level_kind(nl.gates[s].kind)) {
      st.q[s] = next_q[s];
      gate_out[s] = next_q[s]; // seed phase-1-with-level-Qs for edge use
    }
  }
  return core::Status::success();
}

} // namespace detail

// Single rising edge over a flop subset (T013: selective clock domains).
// Null `members` steps every live flop (the global SEQ4 edge); otherwise
// only listed slots snapshot/commit, in slot order (parallel update among
// members, same as the global edge). Combinational settle (phase 1 and
// phase 2) is always whole-netlist, so observed outputs are end-of-step
// values. Otherwise the SEQ4 contract above holds unchanged: validation
// first, untouched state on failure, no Netlist topology mutation.
[[nodiscard]] inline core::Status seq_step_filtered(Netlist &nl, SeqState &st, std::span<const LogicValue> block_inputs,
                                                   std::span<LogicValue> block_outputs, const std::uint16_t *members,
                                                   std::size_t n_members) noexcept {
  init_seq_state(st, nl);
  if (!validate_sequential(nl).ok()) {
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
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    if (st.valid[s] != 0 && st.q[s] != LogicValue::kLow && st.q[s] != LogicValue::kHigh) {
      return core::Status::failure(core::StatusCode::kInvalidArgument);
    }
  }
  // Dangling-input resolution (same order/rules as solve): leaders consume
  // block_inputs in slot order, X/Z rejected; ignored pins tie to kLow.
  LogicValue resolved[kMaxGates * kMaxInputs];
  if (!fill_resolved(nl, block_inputs, resolved).ok()) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }

  std::uint16_t comb_order[kMaxGates];
  std::size_t n_comb = 0;
  LogicValue gate_out[kMaxGates];
  if (!detail::eval_comb_phase1(nl, st, resolved, gate_out, comb_order, n_comb).ok()) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }

  // Level settle (004-sr-d-latches): latches resolve from the phase-1
  // nets and commit before any edge snapshot, so edge flops wired from
  // latch Q see end-of-level values. Fails with state untouched on X/Z
  // (or, from US2, S=R=1). Unconditional: domain filtering is edge-only.
  if (!detail::apply_level_resolve(nl, st, resolved, gate_out).ok()) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }

  // Snapshot: D (input 0) for every edge flop + CLK (input 1) validation,
  // in slot order. Level kinds (DLATCH/SRLATCH) are exempt — settled
  // above; advances never gate their Q. Full-control DFFs additionally
  // resolve RST/SET/EN with priority RST > SET > EN-hold > D (reset wins
  // RST+SET; EN=Low holds Q). D/CLK resolve first so X/Z there fails
  // loudly even for held flops. X/Z anywhere fails before any commit
  // (state untouched).
  LogicValue next_q[kMaxGates];
  for (std::size_t s = 0; s < kMaxGates; ++s) {
    next_q[s] = LogicValue::kLow;
  }
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    const Gate &g = nl.gates[s];
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
    if (!detail::resolve_step_input(nl, s, 0, gate_out, resolved, d)) {
      return core::Status::failure(core::StatusCode::kInvalidArgument);
    }
    if (!detail::resolve_step_input(nl, s, 1, gate_out, resolved, clk)) {
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
        next_q[s] = (st.q[s] == LogicValue::kHigh) ? LogicValue::kLow : LogicValue::kHigh;
      } else {
        next_q[s] = st.q[s]; // hold: T ignored this edge
      }
      continue;
    }
    if (g.kind == GateKind::kJk) {
      // JK flop (003-jk-t-flops): the shared prelude resolved J (input 0)
      // into d and K (input 1) into clk, both validated Low/High above.
      // Resolve the clock (input 2) here; per the SEQ4 rule it is
      // validated, not gating. Excitation resolves from pre-edge Q.
      LogicValue jk_clk = LogicValue::kLow;
      if (!detail::resolve_step_input(nl, s, 2, gate_out, resolved, jk_clk)) {
        return core::Status::failure(core::StatusCode::kInvalidArgument);
      }
      if (jk_clk != LogicValue::kLow && jk_clk != LogicValue::kHigh) {
        return core::Status::failure(core::StatusCode::kInvalidArgument);
      }
      const bool j = (d == LogicValue::kHigh);
      const bool k = (clk == LogicValue::kHigh);
      if (!j && !k) {
        next_q[s] = st.q[s]; // hold
      } else if (j && !k) {
        next_q[s] = LogicValue::kHigh; // set
      } else if (!j && k) {
        next_q[s] = LogicValue::kLow; // reset
      } else if (st.q[s] == LogicValue::kHigh) {
        next_q[s] = LogicValue::kLow; // toggle
      } else {
        next_q[s] = LogicValue::kHigh; // toggle
      }
      continue;
    }
    if (g.kind == GateKind::kDff && g.input_count > 2) {
      LogicValue rst = LogicValue::kLow;
      LogicValue set = LogicValue::kLow;
      LogicValue en = LogicValue::kHigh;
      if (!detail::resolve_control(nl, s, 2, gate_out, resolved, rst) ||
          !detail::resolve_control(nl, s, 3, gate_out, resolved, set) ||
          !detail::resolve_control(nl, s, 4, gate_out, resolved, en)) {
        return core::Status::failure(core::StatusCode::kInvalidArgument);
      }
      if (rst == LogicValue::kHigh) {
        next_q[s] = LogicValue::kLow;
        continue;
      }
      if (set == LogicValue::kHigh) {
        next_q[s] = LogicValue::kHigh;
        continue;
      }
      if (en == LogicValue::kLow) {
        next_q[s] = st.q[s]; // synchronous hold: D ignored this edge
        continue;
      }
    }
    // Edge capture for bare DFFs (004: DLATCH/SRLATCH are level-exempt
    // and never reach here). CLK validated above; the edge itself is the
    // call. T/JK flops take their own branches earlier (003-jk-t-flops).
    next_q[s] = d;
  }

  // Commit: parallel Q update in slot order (members only for subsets;
  // level kinds already committed by the level settle and stay exempt).
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    if (nl.gates[s].live && is_sequential_kind(nl.gates[s].kind) && !detail::is_level_kind(nl.gates[s].kind) &&
        detail::is_step_member(members, n_members, s)) {
      st.q[s] = next_q[s];
      gate_out[s] = next_q[s]; // seed phase 2 with NEW Qs
    }
  }

  // Phase 2: zero-delay re-settle with NEW Qs for end-of-step outputs.
  LogicValue ins[kMaxInputs];
  for (std::size_t oi = 0; oi < n_comb; ++oi) {
    const std::uint16_t s = comb_order[oi];
    const Gate &g = nl.gates[s];
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      if (!detail::resolve_step_input(nl, s, i, gate_out, resolved, ins[i])) {
        return core::Status::failure(core::StatusCode::kInvalidArgument);
      }
    }
    gate_out[s] = eval_gate(g.kind, ins, g.input_count);
  }

  std::size_t out_cursor = 0;
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    const Gate &g = nl.gates[s];
    if (!g.live) {
      continue;
    }
    if (g.output_fanout == 0 && !g.ignore_output) {
      if (out_cursor >= block_outputs.size()) {
        return core::Status::failure(core::StatusCode::kInvalidArgument);
      }
      block_outputs[out_cursor++] = gate_out[s];
    }
    if (is_sequential_kind(g.kind) && g.nq_fanout == 0 && !g.ignore_nq_output) {
      if (out_cursor >= block_outputs.size()) {
        return core::Status::failure(core::StatusCode::kInvalidArgument);
      }
      block_outputs[out_cursor++] = apply_polarity(gate_out[s], 1);
    }
  }
  if (out_cursor != block_outputs.size()) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  return core::Status::success();
}

// Single rising edge over the whole netlist (global clock): every live
// flop steps. Exact SEQ4 semantics, via the filtered core.
[[nodiscard]] inline core::Status seq_step(Netlist &nl, SeqState &st, std::span<const LogicValue> block_inputs,
                                          std::span<LogicValue> block_outputs) noexcept {
  return seq_step_filtered(nl, st, block_inputs, block_outputs, nullptr, 0);
}

// Immediate async-apply for full-control DFFs (FR-002/FR-003): resolves
// RST/SET nets for every live 5-input DFF and forces Q per priority
// (RST→0, else SET→1), in slot order — taking effect with no clock
// advance. Session calls this after every control-net mutation
// (poke/wire/tie/load) and at step top; it never touches cycle counters,
// EN, or D, and is a success no-op on nets without full-control flops.
// Resolve-all-then-commit: X/Z on any resolved control fails loudly with
// state untouched. Same validation/size contract as seq_step_filtered.
[[nodiscard]] inline core::Status apply_async_controls(Netlist &nl, SeqState &st,
                                                      std::span<const LogicValue> block_inputs) noexcept {
  init_seq_state(st, nl);
  if (!validate_sequential(nl).ok()) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  const std::size_t n_in = block_input_count(nl);
  if (block_inputs.size() != n_in) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    if (st.valid[s] != 0 && st.q[s] != LogicValue::kLow && st.q[s] != LogicValue::kHigh) {
      return core::Status::failure(core::StatusCode::kInvalidArgument);
    }
  }
  LogicValue resolved[kMaxGates * kMaxInputs];
  if (!fill_resolved(nl, block_inputs, resolved).ok()) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  LogicValue gate_out[kMaxGates];
  std::uint16_t comb_order[kMaxGates];
  std::size_t n_comb = 0;
  if (!detail::eval_comb_phase1(nl, st, resolved, gate_out, comb_order, n_comb).ok()) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  LogicValue forced_q[kMaxGates];
  bool force[kMaxGates] = {};
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    const Gate &g = nl.gates[s];
    if (!g.live || !is_sequential_kind(g.kind) || g.kind != GateKind::kDff || g.input_count <= 2) {
      continue;
    }
    LogicValue rst = LogicValue::kLow;
    LogicValue set = LogicValue::kLow;
    if (!detail::resolve_control(nl, s, 2, gate_out, resolved, rst) ||
        !detail::resolve_control(nl, s, 3, gate_out, resolved, set)) {
      return core::Status::failure(core::StatusCode::kInvalidArgument);
    }
    if (rst == LogicValue::kHigh) {
      forced_q[s] = LogicValue::kLow;
      force[s] = true;
    } else if (set == LogicValue::kHigh) {
      forced_q[s] = LogicValue::kHigh;
      force[s] = true;
    }
  }
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    if (force[s]) {
      st.q[s] = forced_q[s];
    }
  }
  return core::Status::success();
}

// Immediate level-settle for DLATCH/SRLATCH (004-sr-d-latches): resolves
// latch nets and commits Q per the level rule (transparent D follows D
// while EN is High, frozen hold otherwise; SR holds in US1, set/reset
// plus S=R=1 refusal from US2), in slot order — taking effect with no
// clock advance. Session calls this after every control-net mutation
// (poke/wire/tie/load) and at step top; it never touches cycle counters
// or edge-flop state, and is a success no-op on nets without latches.
// Resolve-all-then-commit: X/Z on any resolved latch input fails loudly
// with state untouched. Same validation/size contract as
// apply_async_controls.
[[nodiscard]] inline core::Status apply_level_latches(Netlist &nl, SeqState &st,
                                                      std::span<const LogicValue> block_inputs) noexcept {
  init_seq_state(st, nl);
  if (!validate_sequential(nl).ok()) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  const std::size_t n_in = block_input_count(nl);
  if (block_inputs.size() != n_in) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    if (st.valid[s] != 0 && st.q[s] != LogicValue::kLow && st.q[s] != LogicValue::kHigh) {
      return core::Status::failure(core::StatusCode::kInvalidArgument);
    }
  }
  LogicValue resolved[kMaxGates * kMaxInputs];
  if (!fill_resolved(nl, block_inputs, resolved).ok()) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  LogicValue gate_out[kMaxGates];
  std::uint16_t comb_order[kMaxGates];
  std::size_t n_comb = 0;
  if (!detail::eval_comb_phase1(nl, st, resolved, gate_out, comb_order, n_comb).ok()) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  return detail::apply_level_resolve(nl, st, resolved, gate_out);
}

// Convenience: step with block outputs discarded (state observed via
// SeqState.q / find_seq_serial). Computes the same end-of-step values;
// only the caller-visible copy is dropped.
[[nodiscard]] inline core::Status seq_step(Netlist &nl, SeqState &st, std::span<const LogicValue> block_inputs) noexcept {
  const std::size_t n_out = block_output_count(nl);
  LogicValue scratch[kMaxGates];
  for (std::size_t i = 0; i < kMaxGates; ++i) {
    scratch[i] = LogicValue::kLow;
  }
  std::span<LogicValue> out_span(scratch, n_out);
  return seq_step(nl, st, block_inputs, out_span);
}

} // namespace wengine::logic
