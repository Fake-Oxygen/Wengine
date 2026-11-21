#pragma once

/**
 * @file
 * Opt-in delay-honoring evaluator (014) beside the zero-delay
 * evaluators: an event-driven transport-delay wheel that honors the
 * per-gate `propagation_delay_ps` annotations 013 shipped (whole
 * picoseconds; un-annotated gates take the mode's unit delay of one
 * picosecond) and reports ordered event traces.
 *
 * Discipline (contracts/timing-semantics.md, research D1–D9):
 * transport semantics (no cancellation, no inertial filtering),
 * integer-only picosecond time with checked adds and loud overflow
 * refusal, a total event order (time first, then slot order), and a
 * loud constant event cap (never silent truncation). The wheel never
 * mutates `Netlist` or any session state: state commits stay with the
 * unchanged stepper (`seq_step.hpp`), validation and settled outputs
 * with the unchanged `solve`/phase-2 paths — the adapters report
 * settled values from those paths, so the mode observes ordering
 * without altering outcomes.
 *
 * Fast-path guarantee (research D10): `eval.hpp` and `seq_step.hpp`
 * gain zero code; the default path reads no delay field and branches
 * on no mode. This header reads `propagation_delay_ps` only.
 *
 * Dependency rules: header-only, allocation-free, exception-free, no
 * strings, no I/O, no syscalls, `[[nodiscard]]` `core::Status`
 * returns; never includes CLI types (logic -> core/status only).
 * Consumers: the `solve`/`step`/`truth` CLI adapters and
 * `tests/test_logic_timing.cpp`.
 */

#include "wengine/core/status.hpp"
#include "wengine/logic/block_interface.hpp"
#include "wengine/logic/shared_net.hpp"
#include "wengine/logic/tie_set.hpp"
#include "wengine/logic/topo.hpp"
#include "wengine/logic/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace wengine::logic {

/**
 * @brief Loud event budget for one timing window (research D9).
 *
 * Transport propagation through reconvergent fan-out can multiply
 * (gate x arrival-time) pairs far beyond gate count, so the cap is a
 * genuine guard: breaching it refuses loudly (exit 1, workspace and
 * settled output untouched), never hangs and never truncates.
 */
inline constexpr std::size_t kMaxTimingEvents = 4096;

/**
 * @brief The mode's unit delay in whole picoseconds (research D2).
 *
 * The field's smallest storable nonzero value; every un-annotated
 * gate (stored 0, per 013's zero-is-un-annotated convention) takes
 * this. A mode-level default, never written back to storage. The
 * unit floor is also the well-foundedness proof (research D3): every
 * cause precedes its effect by >= 1 ps, so buckets never
 * self-interact and the wheel strictly advances.
 */
inline constexpr std::uint64_t kTimingUnitPs = 1;

/**
 * @brief Transport delay of one gate in this mode: its stored annotation exactly as annotated, else the unit.
 *
 * @param[in] nl Netlist holding the gate.
 * @param[in] slot Gate slot.
 * @return Stored `propagation_delay_ps`, or `kTimingUnitPs` when the field is 0 (un-annotated). Never 0.
 */
[[nodiscard]] inline std::uint64_t timing_gate_delay(const Netlist &nl, std::uint16_t slot) noexcept {
  const std::uint64_t stored = (slot < kMaxGates && nl.gates[slot].live) ? nl.gates[slot].propagation_delay_ps : 0;
  return (stored == 0) ? kTimingUnitPs : stored;
}

/**
 * @brief One net transition opening (or forcing inside) a window.
 *
 * Block-input seeds name a tie-class leader `(slot, input)` and land
 * at fire time 0 with a Low/High value. Flop seeds name a sequential
 * slot whose post-edge Q is forced at its clock-to-Q fire time
 * (annotation, else the unit); the NQ sibling shares the timestamp.
 */
struct TimingSeed {
  /** @brief Gate slot: leader gate for block inputs, flop for forces. */
  std::uint16_t slot = 0;
  /** @brief 0-based input index of the leader (block seeds only). */
  std::uint8_t input = 0;
  /** @brief True for a flop Q force, false for a block-input seed. */
  bool is_flop = false;
  /** @brief Whole-picosecond fire time (0 for block seeds). */
  std::uint64_t fire_ps = 0;
  /** @brief Forced/set value (Low/High; transients arise inside). */
  LogicValue new_value = LogicValue::kLow;
};

/**
 * @brief One observable transition in a window.
 *
 * Input-net events name the leader pin `(slot, input)`; gate-output
 * events name the driving pin (`nq` selects NQ). Values are
 * four-state: transients may be X, and Z appears only from a
 * released TBUF (research D5).
 */
struct TimingEvent {
  /** @brief Whole-picosecond offset from the window anchor. */
  std::uint64_t t_ps = 0;
  /** @brief Driving slot (or leader gate for input nets). */
  std::uint16_t slot = 0;
  /** @brief Leader input index (input-net events only). */
  std::uint8_t input = 0;
  /** @brief True for input-net events, false for gate outputs. */
  bool is_input = false;
  /** @brief True for the NQ sibling of a flop Q event. */
  bool nq = false;
  /** @brief Value before the transition. */
  LogicValue old_value = LogicValue::kLow;
  /** @brief Value after the transition. */
  LogicValue new_value = LogicValue::kLow;
};

/**
 * @brief A window's observable: fixed-capacity event list plus drain.
 *
 * Never serialized, never part of Snapshot/workspace/YAML (spec
 * FR-012): per-invocation scratch printed by one CLI helper, then discarded.
 */
struct TimingTrace {
  /** @brief Events in pinned order (time, then slot order). */
  std::array<TimingEvent, kMaxTimingEvents> events{};
  /** @brief Stored event count (<= kMaxTimingEvents). */
  std::size_t n_events = 0;
  /** @brief Last event time; 0 for an empty window. */
  std::uint64_t drain_ps = 0;
  /** @brief Live gates carrying a nonzero annotation (header line). */
  std::size_t n_annotated = 0;
  /** @brief Unit floor echoed in the header (always 1). */
  std::uint64_t unit_ps = kTimingUnitPs;
};

/**
 * @brief Caller-owned wheel working state (research D1).
 *
 * `cur` holds per-slot outputs, `blk` per-(slot,input) dangling-net
 * values (leaders written, members fanned out); the sink maps route
 * driver-slot and leader-flat changes to consumer (slot, input)
 * pairs in slot order. Sized to the existing fixed caps; no heap,
 * no exceptions. The adapter settles `cur`/`blk` to the window's
 * pre-state before calling `run_timing_window`.
 */
struct TimingScratch {
  /** @brief Current per-slot output (Q for sequential gates). */
  std::array<LogicValue, kMaxGates> cur{};
  /** @brief Current dangling-net values (4-state capable). */
  std::array<LogicValue, kMaxGates * kMaxInputs> blk{};
  /** @brief Pending recompute times (parallel to slots). */
  std::array<std::uint64_t, kMaxTimingEvents> pending_t{};
  /** @brief Pending recompute slots. */
  std::array<std::uint16_t, kMaxTimingEvents> pending_s{};
  /** @brief Pending force flags (flop Q forces vs recomputes). */
  std::array<std::uint8_t, kMaxTimingEvents> pending_force{};
  /** @brief Pending forced values (force entries only). */
  std::array<LogicValue, kMaxTimingEvents> pending_val{};
  /** @brief Pending entry count. */
  std::size_t n_pending = 0;
  /** @brief Consumer-slot heads per driver slot (kNoDriver = empty). */
  std::array<std::uint16_t, kMaxGates> out_head{};
  /** @brief Consumer-slot heads per leader flat (kNoDriver = empty). */
  std::array<std::uint16_t, kMaxGates * kMaxInputs> in_head{};
  /** @brief Intrusive next-links over the consumer pool. */
  std::array<std::uint16_t, kMaxGates * kMaxInputs> sink_next{};
  /** @brief Consumer slot per pool entry. */
  std::array<std::uint16_t, kMaxGates * kMaxInputs> sink_slot{};
  /** @brief Consumer input per pool entry. */
  std::array<std::uint8_t, kMaxGates * kMaxInputs> sink_input{};
  /** @brief Consumer pool fill. */
  std::size_t n_sink = 0;
};

/**
 * @brief NQ-side read of a flop value (X stays X).
 *
 * Unlike `apply_polarity` (whose non-High arm folds everything to
 * Low for the zero-delay consumer contract), the trace must show a
 * transient X on both pins — hence the separate helper.
 *
 * @param[in] q Flop Q value.
 * @return Low for High, High for Low, anything else untouched.
 */
[[nodiscard]] constexpr LogicValue timing_nq_of(LogicValue q) noexcept {
  if (q == LogicValue::kHigh) {
    return LogicValue::kLow;
  }
  if (q == LogicValue::kLow) {
    return LogicValue::kHigh;
  }
  return q;
}

/**
 * @brief Four-state gate evaluation with dominant-value tables
 *        (research D5, reference: IEEE 1364 gate-level semantics).
 *
 * AND/NAND output 0 if any input reads 0, else X if any input is
 * unknown, else 1 (NAND inverts, X stays X); OR/NOR dual with 1 as
 * the dominant value; XOR/XNOR yield X if any input is unknown
 * (parity has no dominant value); NOT/BUF pass X through; TBUF
 * drives Z only when released. Z at an input reads as X (Z is a net
 * state, not a gate operand — consistent with the shipped funnel
 * where Z is transparent and only consumers give it meaning).
 * Sequential kinds are never evaluated here (the wheel treats edge
 * flops as forces and latches via `timing_eval_latch`).
 *
 * @param[in] kind Gate kind to evaluate (combinational only).
 * @param[in] inputs Per-input four-state values.
 * @param[in] n Input count.
 * @param[out] out Evaluated output.
 * @return True for combinational kinds; false for sequential kinds (caller keeps the current value).
 */
[[nodiscard]] constexpr bool timing_eval_gate(GateKind kind, const LogicValue *inputs, std::uint8_t n, LogicValue &out) noexcept {
  if (inputs == nullptr && n > 0) {
    return false;
  }
  auto unknown = [](LogicValue v) noexcept { return v == LogicValue::kUnknown || v == LogicValue::kHighZ; };
  switch (kind) {
  case GateKind::kAnd:
  case GateKind::kNand: {
    bool any_unknown = false;
    for (std::uint8_t i = 0; i < n; ++i) {
      if (inputs[i] == LogicValue::kLow) {
        out = (kind == GateKind::kAnd) ? LogicValue::kLow : LogicValue::kHigh;
        return true;
      }
      any_unknown = any_unknown || unknown(inputs[i]);
    }
    if (any_unknown) {
      out = LogicValue::kUnknown;
      return true;
    }
    out = (kind == GateKind::kAnd) ? LogicValue::kHigh : LogicValue::kLow;
    return true;
  }
  case GateKind::kOr:
  case GateKind::kNor: {
    bool any_unknown = false;
    for (std::uint8_t i = 0; i < n; ++i) {
      if (inputs[i] == LogicValue::kHigh) {
        out = (kind == GateKind::kOr) ? LogicValue::kHigh : LogicValue::kLow;
        return true;
      }
      any_unknown = any_unknown || unknown(inputs[i]);
    }
    if (any_unknown) {
      out = LogicValue::kUnknown;
      return true;
    }
    out = (kind == GateKind::kOr) ? LogicValue::kLow : LogicValue::kHigh;
    return true;
  }
  case GateKind::kXor:
  case GateKind::kXnor: {
    bool odd = false;
    for (std::uint8_t i = 0; i < n; ++i) {
      if (unknown(inputs[i])) {
        out = LogicValue::kUnknown;
        return true;
      }
      if (inputs[i] == LogicValue::kHigh) {
        odd = !odd;
      }
    }
    const bool y = (kind == GateKind::kXor) ? odd : !odd;
    out = y ? LogicValue::kHigh : LogicValue::kLow;
    return true;
  }
  case GateKind::kNot:
    if (n < 1) {
      return false;
    }
    if (unknown(inputs[0])) {
      out = LogicValue::kUnknown;
      return true;
    }
    out = (inputs[0] == LogicValue::kHigh) ? LogicValue::kLow : LogicValue::kHigh;
    return true;
  case GateKind::kBuf:
    if (n < 1) {
      return false;
    }
    if (unknown(inputs[0])) {
      out = LogicValue::kUnknown;
      return true;
    }
    out = (inputs[0] == LogicValue::kHigh) ? LogicValue::kHigh : LogicValue::kLow;
    return true;
  case GateKind::kTbuf: {
    if (n < 2) {
      return false;
    }
    if (inputs[1] == LogicValue::kHigh) {
      out = unknown(inputs[0]) ? LogicValue::kUnknown : inputs[0];
      return true;
    }
    if (inputs[1] == LogicValue::kLow) {
      out = LogicValue::kHighZ;
      return true;
    }
    out = LogicValue::kUnknown;
    return true;
  }
  case GateKind::kDff:
  case GateKind::kDlatch:
  case GateKind::kJk:
  case GateKind::kT:
  case GateKind::kSrlatch:
    break;
  }
  return false;
}

/**
 * @brief Mid-window level-element response (contracts/timing-semantics.md).
 *
 * DLATCH: asserted enable tracks D (X passes through); anything
 * else holds. SRLATCH: S=R=1 renders X (the transient the settled
 * refusal would hide), S drives 1, R drives 0, S=R=0 holds, any
 * other unknown combination renders X. The annotation on a level
 * element is this response's delay; the settled contract is
 * untouched (settled S=R=1 still refuses on the unchanged path).
 *
 * @param[in] kind kDlatch or kSrlatch (anything else fails).
 * @param[in] a D (latch) or S (SR) value, Z already mapped to X.
 * @param[in] b EN (latch) or R (SR) value, Z already mapped to X.
 * @param[in] hold Current output (the hold source).
 * @param[out] out Recomputed output.
 * @return True for the two level kinds; false otherwise.
 */
[[nodiscard]] constexpr bool timing_eval_latch(GateKind kind, LogicValue a, LogicValue b, LogicValue hold, LogicValue &out) noexcept {
  if (kind == GateKind::kDlatch) {
    out = (b == LogicValue::kHigh) ? a : hold;
    return true;
  }
  if (kind == GateKind::kSrlatch) {
    if (a == LogicValue::kHigh && b == LogicValue::kHigh) {
      out = LogicValue::kUnknown;
      return true;
    }
    if (a == LogicValue::kHigh) {
      out = LogicValue::kHigh;
      return true;
    }
    if (b == LogicValue::kHigh) {
      out = LogicValue::kLow;
      return true;
    }
    if (a == LogicValue::kLow && b == LogicValue::kLow) {
      out = hold;
      return true;
    }
    out = LogicValue::kUnknown;
    return true;
  }
  return false;
}

/**
 * @brief Transient-tolerant twin of `resolve_input`.
 *
 * Assembles the same driver span (primary plus shared extras with
 * per-attachment NQ polarity) through the same `resolve_net` funnel,
 * maps pulls and ignored pins the same way — but never refuses:
 * X/Z flow through (Z at a gate input reads as X), so transient
 * contention windows and released-buffer windows stay visible in
 * the trace instead of failing. No contention counting (the settled
 * path owns SolveDiag).
 *
 * @param[in] nl Netlist holding drivers, marks, and polarities.
 * @param[in] slot Gate slot of the sink.
 * @param[in] input 0-based sink input index.
 * @param[in] cur Current per-slot outputs (flop slots hold Q).
 * @param[in] blk Current dangling-net values (leaders authoritative).
 * @param[out] out Resolved four-state input value.
 */
inline void timing_resolve_input(const Netlist &nl, std::uint16_t slot, std::uint8_t input, const LogicValue *cur, const LogicValue *blk, LogicValue &out) noexcept {
  const Gate &g = nl.gates[slot];
  const std::size_t flat = tie_flat(slot, input);
  auto polar = [](LogicValue v, std::uint8_t inv) noexcept {
    if (inv == 0) {
      return v;
    }
    return timing_nq_of(v);
  };
  auto gate_read = [&](LogicValue v) noexcept {
    return (v == LogicValue::kHighZ) ? LogicValue::kUnknown : v;
  };
  if (is_shared_flat(nl, flat)) {
    LogicValue span[1 + kMaxSharedExtra];
    std::size_t n = 0;
    const std::uint16_t drv = g.drivers[input];
    if (drv != kNoDriver && drv < kMaxGates && nl.gates[drv].live) {
      span[n++] = polar(cur[drv], nl.driver_inv[flat]);
    }
    for_each_extra(nl, flat, [&](std::uint8_t, std::uint16_t edrv, std::uint8_t einv) {
      if (n >= 1 + kMaxSharedExtra) {
        return;
      }
      if (edrv >= kMaxGates || !nl.gates[edrv].live) {
        return;
      }
      span[n++] = polar(cur[edrv], einv);
    });
    LogicValue sunk = LogicValue::kHighZ;
    if (!resolve_net(span, n, sunk)) {
      out = LogicValue::kUnknown; // ill-formed span: unreachable (n <= 1+cap, live drivers only)
      return;
    }
    if (sunk == LogicValue::kHighZ && is_pulled_flat(nl, flat)) {
      sunk = (pull_level_flat(nl, flat) == 1) ? LogicValue::kHigh : LogicValue::kLow;
    }
    out = gate_read(sunk);
    return;
  }
  const std::uint16_t drv = g.drivers[input];
  if (drv != kNoDriver) {
    LogicValue sunk = LogicValue::kHighZ;
    if (drv < kMaxGates && nl.gates[drv].live) {
      if (!resolve_net(&cur[drv], 1, sunk)) {
        out = LogicValue::kUnknown; // unreachable: single live driver
        return;
      }
    }
    if (sunk == LogicValue::kHighZ && is_pulled_flat(nl, flat)) {
      out = (pull_level_flat(nl, flat) == 1) ? LogicValue::kHigh : LogicValue::kLow;
      return;
    }
    out = gate_read(polar(sunk, nl.driver_inv[flat]));
    return;
  }
  if ((g.ignored_mask & static_cast<std::uint8_t>(1u << input)) != 0) {
    out = LogicValue::kLow;
    return;
  }
  const std::size_t leader = tie_leader_flat(nl, flat);
  LogicValue v = blk[leader];
  if (v == LogicValue::kHighZ && is_pulled_flat(nl, flat)) {
    v = (pull_level_flat(nl, flat) == 1) ? LogicValue::kHigh : LogicValue::kLow;
  }
  out = v;
}

/**
 * @brief Settle `scratch` to a window's pre-state (adapters call this first).
 *
 * Dangling prelude rides the single BlockInterface seam: `fill_resolved`
 * takes `block_inputs` (interface order, leaders only — the same order
 * `solve()` consumes; X/Z inputs fail loudly), terminates pulled
 * dangling inputs at their idle level, ties ignored pins to kLow, and
 * fans tied members out from their leader. One topo-order pass then
 * evaluates combinational gates with the 4-state tables. Sequential
 * slots keep whatever the adapter preloaded into `scratch.cur` (kLow
 * for solve/truth windows mirroring `eval_gate`'s fallback, post-edge
 * Q for step windows). On clean 0/1 nets the pass reproduces the
 * zero-delay gate outputs exactly — wheel end-state parity with
 * `solve()` is asserted by test, by construction rather than
 * hand-sync (the wheel holds no twin prelude).
 *
 * The topology filter must match the unchanged path the adapter
 * rides: `kAll` for solve/truth windows (like `solve`), `kCombOnly`
 * for step windows (like the stepper — clocked nets carry
 * flop-aware cycles that `kAll` would refuse).
 *
 * Validates the topology first (combinational cycles fail loudly);
 * the wheel itself never re-validates mid-window.
 *
 * Fast-path guarantee (research D10): `eval.hpp` and `seq_step.hpp`
 * gain zero code — the shared prelude lives in `block_interface.hpp`
 * and this adapter merely calls it, so the settled path's branches
 * and bytes are unchanged (flag-off byte-identity goldens pin this).
 *
 * @param[out] nl Netlist to settle (topology recompiled when dirty).
 * @param[in] block_inputs Block input values in interface order.
 * @param[out] scratch Scratch receiving `blk`/`cur` (sequential `cur` preserved).
 * @param[in] filter Topology edge set (`kAll` or `kCombOnly`).
 * @return kOk on success; kInvalidArgument on cycle/size/X/Z errors.
 */
[[nodiscard]] inline core::Status timing_settle_initial(Netlist &nl, std::span<const LogicValue> block_inputs, TimingScratch &scratch, EdgeFilter filter) noexcept {
  core::Status st = topo::rebuild(nl, filter);
  if (!st.ok()) {
    return st;
  }
  // Single dangling-idle prelude (BlockInterface seam): zero, consume
  // leaders, terminate pulls/ignores, fan out ties. The wheel holds no
  // twin — parity with `solve()` holds by construction.
  st = fill_resolved(nl, block_inputs, scratch.blk.data());
  if (!st.ok()) {
    return st;
  }
  LogicValue ins[kMaxInputs];
  for (std::uint16_t oi = 0; oi < nl.topo_size; ++oi) {
    const std::uint16_t s = nl.topo_order[oi];
    const Gate &g = nl.gates[s];
    if (is_sequential_kind(g.kind)) {
      continue;
    }
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      timing_resolve_input(nl, s, i, scratch.cur.data(), scratch.blk.data(), ins[i]);
    }
    LogicValue y = LogicValue::kLow;
    if (timing_eval_gate(g.kind, ins, g.input_count, y)) {
      scratch.cur[s] = y;
    }
  }
  return core::Status::success();
}

namespace timing_detail {

/**
 * @brief Checked schedule insert with same-(gate,time) collapse.
 *
 * A pending recompute for the same slot at the same instant already
 * queued collapses into one evaluation reading nets as of that time
 * (research D4): the second insert is silent. Insert beyond the
 * budget refuses loudly (research D9).
 */
[[nodiscard]] inline core::Status schedule_at(TimingScratch &scratch, std::uint64_t t, std::uint16_t slot, bool force, LogicValue val) noexcept {
  for (std::size_t i = 0; i < scratch.n_pending; ++i) {
    if (scratch.pending_t[i] == t && scratch.pending_s[i] == slot) {
      return core::Status::success();
    }
  }
  if (scratch.n_pending >= kMaxTimingEvents) {
    return core::Status::failure(core::StatusCode::kInternal);
  }
  scratch.pending_t[scratch.n_pending] = t;
  scratch.pending_s[scratch.n_pending] = slot;
  scratch.pending_force[scratch.n_pending] = force ? 1 : 0;
  scratch.pending_val[scratch.n_pending] = val;
  ++scratch.n_pending;
  return core::Status::success();
}

/**
 * @brief Checked time add: overflow refuses loudly, never wraps.
 */
[[nodiscard]] inline bool checked_fire_at(std::uint64_t now, std::uint64_t delay, std::uint64_t &at) noexcept {
  if (delay > 0xFFFFFFFFFFFFFFFFULL - now) {
    return false;
  }
  at = now + delay;
  return true;
}

/**
 * @brief Record one consumer's recompute after its driver's change.
 */
[[nodiscard]] inline core::Status schedule_consumers(const Netlist &nl, TimingScratch &scratch, const std::uint16_t *head, std::size_t key, std::uint64_t now) noexcept {
  for (std::uint16_t e = head[key]; e != kNoDriver; e = scratch.sink_next[e]) {
    const std::uint16_t c = scratch.sink_slot[e];
    const std::uint64_t delay = timing_gate_delay(nl, c);
    std::uint64_t at = 0;
    if (!checked_fire_at(now, delay, at)) {
      return core::Status::failure(core::StatusCode::kInternal);
    }
    const core::Status st = schedule_at(scratch, at, c, false, LogicValue::kLow);
    if (!st.ok()) {
      return st;
    }
  }
  return core::Status::success();
}

/**
 * @brief Append one event, counting the cap loudly.
 */
[[nodiscard]] inline core::Status emit_event(TimingTrace &trace, const TimingEvent &ev) noexcept {
  if (trace.n_events >= kMaxTimingEvents) {
    return core::Status::failure(core::StatusCode::kInternal);
  }
  trace.events[trace.n_events++] = ev;
  if (ev.t_ps > trace.drain_ps) {
    trace.drain_ps = ev.t_ps;
  }
  return core::Status::success();
}

} // namespace timing_detail

/**
 * @brief Run one transport-delay window to drain (research D4/D6).
 *
 * Precondition: the adapter settled `scratch` (`cur`/`blk`) to the
 * window's pre-state with `timing_settle_initial` and preloaded
 * sequential `cur` (post-step Q for step windows, kLow otherwise).
 * Seeds apply at their fire times in slot order (block seeds at 0,
 * flop forces at their clock-to-Q); every input-net transition
 * schedules each consuming gate at (transition + gate delay) with no
 * cancellation and no inertial filtering; same-(gate, time) doubles
 * collapse; no-op evaluations stay silent. Events append in the
 * pinned total order (time, then slot order; an NQ sibling follows
 * its Q immediately at the same timestamp).
 *
 * Only this module's two new failure vocabularies arise here: the
 * event-cap breach and the time-axis overflow, both
 * `kInternal` (CLI exit 1) with the netlist, state, and settled
 * output untouched (the adapter refuses before printing anything).
 * The trace is cleared on refusal (no partial trace escapes).
 *
 * @param[in] nl Netlist to observe (read-only; never mutated).
 * @param[in] seeds Seed span (slot order within each fire time).
 * @param[out] trace Filled trace (cleared on refusal).
 * @param[in,out] scratch Working state (sink maps rebuilt here).
 * @param[in] recompute_latches True in step windows: DLATCH/SRLATCH
 *            outputs recompute mid-window at their response delay;
 *            false in solve/truth windows (sequential gates are
 *            constants there, mirroring zero-delay `solve`).
 * @return kOk with a drained trace; kInternal on cap/overflow.
 */
[[nodiscard]] inline core::Status run_timing_window_inner(const Netlist &nl, std::span<const TimingSeed> seeds, TimingTrace &trace, TimingScratch &scratch,
                                                           bool recompute_latches) noexcept {
  trace.n_events = 0;
  trace.drain_ps = 0;
  trace.unit_ps = kTimingUnitPs;
  trace.n_annotated = 0;
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    if (nl.gates[s].live && nl.gates[s].propagation_delay_ps != 0) {
      ++trace.n_annotated;
    }
  }
    scratch.n_pending = 0;
  scratch.n_sink = 0;

  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    scratch.out_head[s] = kNoDriver;
  }
  for (std::size_t f = 0; f < kMaxGates * kMaxInputs; ++f) {
    scratch.in_head[f] = kNoDriver;
  }
  // Reverse sink map in slot order: every consumer reachable from
  // the net that changed (driver slot, shared extra, or dangling
  // leader flat). Static nets (ignored, pulled, unmarked-shared
  // empties) never change and take no entry.
  for (std::uint16_t c = 0; c < kMaxGates; ++c) {
    const Gate &g = nl.gates[c];
    if (!g.live) {
      continue;
    }
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      const std::size_t flat = static_cast<std::size_t>(c) * kMaxInputs + static_cast<std::size_t>(i);
      const std::uint16_t drv = g.drivers[i];
      bool dynamic = false;
      if (drv != kNoDriver) {
        if (drv < kMaxGates && nl.gates[drv].live && scratch.n_sink < kMaxGates * kMaxInputs) {
          const std::size_t e = scratch.n_sink++;
          scratch.sink_slot[e] = c;
          scratch.sink_input[e] = i;
          scratch.sink_next[e] = scratch.out_head[drv];
          scratch.out_head[drv] = static_cast<std::uint16_t>(e);
        }
        dynamic = true;
      }
      if (is_shared_flat(nl, flat)) {
        for_each_extra(nl, flat, [&](std::uint8_t, std::uint16_t edrv, std::uint8_t) {
          if (edrv < kMaxGates && nl.gates[edrv].live && scratch.n_sink < kMaxGates * kMaxInputs) {
            const std::size_t e = scratch.n_sink++;
            scratch.sink_slot[e] = c;
            scratch.sink_input[e] = i;
            scratch.sink_next[e] = scratch.out_head[edrv];
            scratch.out_head[edrv] = static_cast<std::uint16_t>(e);
          }
        });
        dynamic = true;
      }
      if (!dynamic && drv == kNoDriver && (g.ignored_mask & static_cast<std::uint8_t>(1u << i)) == 0 &&
          !is_pulled_flat(nl, flat)) {
        const std::size_t leader = tie_leader_flat(nl, flat);
        if (scratch.n_sink < kMaxGates * kMaxInputs) {
          const std::size_t e = scratch.n_sink++;
          scratch.sink_slot[e] = c;
          scratch.sink_input[e] = i;
          scratch.sink_next[e] = scratch.in_head[leader];
          scratch.in_head[leader] = static_cast<std::uint16_t>(e);
        }
      }
    }
  }
  // Seeds in span order (callers pass slot order): block nets apply
  // at 0, flop forces schedule at their fire time.
  for (const TimingSeed &seed : seeds) {
    if (seed.is_flop) {
      core::Status st = timing_detail::schedule_at(scratch, seed.fire_ps, seed.slot, true, seed.new_value);
      if (!st.ok()) {
        return st;
      }
      continue;
    }
    const std::size_t leader = tie_leader_flat(nl, static_cast<std::size_t>(seed.slot) * kMaxInputs + seed.input);
    if (scratch.blk[leader] == seed.new_value) {
      continue;
    }
    TimingEvent ev;
    ev.t_ps = 0;
    ev.slot = seed.slot;
    ev.input = seed.input;
    ev.is_input = true;
    ev.old_value = scratch.blk[leader];
    ev.new_value = seed.new_value;
    core::Status st = timing_detail::emit_event(trace, ev);
    if (!st.ok()) {
      return st;
    }
    scratch.blk[leader] = seed.new_value;
    for (std::uint16_t s = 0; s < kMaxGates; ++s) {
      const Gate &g = nl.gates[s];
      if (!g.live) {
        continue;
      }
      for (std::uint8_t i = 0; i < g.input_count; ++i) {
        if (g.drivers[i] != kNoDriver) {
          continue;
        }
        const std::size_t flat = static_cast<std::size_t>(s) * kMaxInputs + static_cast<std::size_t>(i);
        if (tie_leader_flat(nl, flat) == leader) {
          scratch.blk[flat] = seed.new_value;
        }
      }
    }
    st = timing_detail::schedule_consumers(nl, scratch, scratch.in_head.data(), leader, 0);
    if (!st.ok()) {
      return st;
    }
  }
  // Drain: strict time progress (every delay >= 1) over a finite
  // budget, so the loop always terminates (research D3/D9).
  LogicValue ins[kMaxInputs];
  while (scratch.n_pending > 0) {
    std::size_t best = 0;
    for (std::size_t i = 1; i < scratch.n_pending; ++i) {
      if (scratch.pending_t[i] < scratch.pending_t[best] || (scratch.pending_t[i] == scratch.pending_t[best] && scratch.pending_s[i] < scratch.pending_s[best])) {
        best = i;
      }
    }
    const std::uint64_t now = scratch.pending_t[best];
    const std::uint16_t slot = scratch.pending_s[best];
    const bool force = scratch.pending_force[best] != 0;
    const LogicValue forced = scratch.pending_val[best];
    scratch.pending_t[best] = scratch.pending_t[scratch.n_pending - 1];
    scratch.pending_s[best] = scratch.pending_s[scratch.n_pending - 1];
    scratch.pending_force[best] = scratch.pending_force[scratch.n_pending - 1];
    scratch.pending_val[best] = scratch.pending_val[scratch.n_pending - 1];
    --scratch.n_pending;
    if (!nl.gates[slot].live) {
      continue;
    }
    if (force) {
      if (scratch.cur[slot] == forced) {
        continue;
      }
      TimingEvent qev;
      qev.t_ps = now;
      qev.slot = slot;
      qev.old_value = scratch.cur[slot];
      qev.new_value = forced;
      core::Status st = timing_detail::emit_event(trace, qev);
      if (!st.ok()) {
        return st;
      }
      TimingEvent nqev;
      nqev.t_ps = now;
      nqev.slot = slot;
      nqev.nq = true;
      nqev.old_value = timing_nq_of(scratch.cur[slot]);
      nqev.new_value = timing_nq_of(forced);
      st = timing_detail::emit_event(trace, nqev);
      if (!st.ok()) {
        return st;
      }
      scratch.cur[slot] = forced;
      for (std::uint16_t e = scratch.out_head[slot]; e != kNoDriver; e = scratch.sink_next[e]) {
        const std::uint16_t c = scratch.sink_slot[e];
        const std::uint64_t cdelay = timing_gate_delay(nl, c);
        std::uint64_t cat = 0;
        if (!timing_detail::checked_fire_at(now, cdelay, cat)) {
          return core::Status::failure(core::StatusCode::kInternal);
        }
        st = timing_detail::schedule_at(scratch, cat, c, false, LogicValue::kLow);
        if (!st.ok()) {
          return st;
        }
      }
      continue;
    }
    const Gate &g = nl.gates[slot];
    const bool is_latch = (g.kind == GateKind::kDlatch || g.kind == GateKind::kSrlatch);
    if (is_sequential_kind(g.kind) && !(recompute_latches && is_latch)) {
      continue;
    }
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      timing_resolve_input(nl, slot, i, scratch.cur.data(), scratch.blk.data(), ins[i]);
    }
    LogicValue y = scratch.cur[slot];
    bool have = false;
    if (is_latch) {
      have = timing_eval_latch(g.kind, ins[0], (g.input_count > 1) ? ins[1] : LogicValue::kLow, scratch.cur[slot], y);
    } else {
      have = timing_eval_gate(g.kind, ins, g.input_count, y);
    }
    if (!have || y == scratch.cur[slot]) {
      continue;
    }
    TimingEvent ev;
    ev.t_ps = now;
    ev.slot = slot;
    ev.old_value = scratch.cur[slot];
    ev.new_value = y;
    core::Status st = timing_detail::emit_event(trace, ev);
    if (!st.ok()) {
      return st;
    }
    scratch.cur[slot] = y;
    st = timing_detail::schedule_consumers(nl, scratch, scratch.out_head.data(), slot, now);
    if (!st.ok()) {
      return st;
    }
  }
  return core::Status::success();
}

/**
 * @brief Run one transport-delay window to drain (public entry).
 *
 * Clears the trace on refusal so no partial trace escapes (the
 * adapter refuses before printing anything in that case).
 *
 * @param[in] nl Netlist to observe (read-only; never mutated).
 * @param[in] seeds Seed span (slot order within each fire time).
 * @param[out] trace Filled trace (cleared on refusal).
 * @param[in,out] scratch Working state (sink maps rebuilt here).
 * @param[in] recompute_latches True in step windows, false in
 *            solve/truth windows (see `run_timing_window_inner`).
 * @return kOk with a drained trace; kInternal on cap/overflow.
 */
[[nodiscard]] inline core::Status run_timing_window(const Netlist &nl, std::span<const TimingSeed> seeds, TimingTrace &trace, TimingScratch &scratch,
                                                    bool recompute_latches) noexcept {
  const core::Status st = run_timing_window_inner(nl, seeds, trace, scratch, recompute_latches);
  if (!st.ok()) {
    trace.n_events = 0;
    trace.drain_ps = 0;
  }
  return st;
}

} // namespace wengine::logic
