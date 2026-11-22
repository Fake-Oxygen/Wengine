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
#include "wengine/logic/four_state.hpp"
#include "wengine/logic/input_resolution.hpp"
#include "wengine/logic/shared_net.hpp"
#include "wengine/logic/tie_set.hpp"
#include "wengine/logic/topo.hpp"
#include "wengine/logic/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

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
[[nodiscard]] inline std::uint64_t timing_gate_delay(const Netlist &nl, Slot slot) noexcept {
  const std::uint64_t stored = (slot < slot_count(nl) && nl.slots[slot].live) ? nl.slots[slot].propagation_delay_ps : 0;
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
  Slot slot = 0;
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
  Slot slot = 0;
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
 * pairs in slot order. Pool-sized vectors grown by
 * `ensure_timing_scratch` (no fixed caps; growth preserves existing
 * contents, value-init zeros for the new tail); the pending schedule
 * stays a fixed `kMaxTimingEvents` event-budget array (pool-
 * independent, see `kMaxTimingEvents`). The adapter settles
 * `cur`/`blk` to the window's pre-state before calling
 * `run_timing_window`, and builds the topology-invariant sink maps
 * once with `timing_build_sink_maps` (rebuilt only after connectivity
 * changes; per-window drains reuse them untouched).
 */
struct TimingScratch {
  /** @brief Current per-slot output (Q for sequential gates; `n` entries). */
  std::vector<LogicValue> cur{};
  /** @brief Current dangling-net values, 4-state capable (`n * kMaxInputs` entries). */
  std::vector<LogicValue> blk{};
  /** @brief Pending recompute times (parallel to slots; fixed event budget). */
  std::array<std::uint64_t, kMaxTimingEvents> pending_t{};
  /** @brief Pending recompute slots (fixed event budget). */
  std::array<Slot, kMaxTimingEvents> pending_s{};
  /** @brief Pending force flags (flop Q forces vs recomputes; fixed event budget). */
  std::array<std::uint8_t, kMaxTimingEvents> pending_force{};
  /** @brief Pending forced values, force entries only (fixed event budget). */
  std::array<LogicValue, kMaxTimingEvents> pending_val{};
  /** @brief Pending entry count. */
  std::size_t n_pending = 0;
  /** @brief Consumer-slot heads per driver slot (`n` entries; kNoDriver = empty). */
  std::vector<Slot> out_head{};
  /** @brief Consumer-slot heads per leader flat (`n * kMaxInputs` entries; kNoDriver = empty). */
  std::vector<Slot> in_head{};
  /** @brief Intrusive next-links over the consumer pool (`n * kMaxInputs` entries). */
  std::vector<Slot> sink_next{};
  /** @brief Consumer slot per pool entry (`n * kMaxInputs` entries). */
  std::vector<Slot> sink_slot{};
  /** @brief Consumer input per pool entry (`n * kMaxInputs` entries). */
  std::vector<std::uint8_t> sink_input{};
  /** @brief Consumer pool fill. */
  std::size_t n_sink = 0;
  /**
   * @brief Loud size validation before any work (engine-seam D3 rule).
   *
   * @param[in] n Addressable slot count the vectors must cover
   *            (`n * kMaxInputs` for the flat/pool vectors).
   * @return kOk when every pool vector holds at least its share.
   */
  [[nodiscard]] core::Status validate(std::size_t n) const noexcept {
    const std::size_t nflat = n * kMaxInputs;
    if (cur.size() < n || blk.size() < nflat || out_head.size() < n || in_head.size() < nflat ||
        sink_next.size() < nflat || sink_slot.size() < nflat || sink_input.size() < nflat) {
      return core::Status::failure(core::StatusCode::kInvalidArgument);
    }
    return core::Status::success();
  }
};

/**
 * @brief Grow scratch pool vectors to cover `n` slots.
 *
 * Sizes `cur`/`out_head` to `n` and `blk`/`in_head`/`sink_next`/
 * `sink_slot`/`sink_input` to `n * kMaxInputs`. No-op when already
 * sized; never shrinks; existing contents are preserved (growth
 * value-inits the new tail). The pending schedule is untouched (fixed
 * event budget, pool-independent).
 *
 * (No noexcept: vector growth may allocate.)
 *
 * @param[out] scratch Working state receiving pool-sized vectors.
 * @param[in] n Addressable slot count the vectors must cover.
 */
inline void ensure_timing_scratch(TimingScratch &scratch, std::size_t n) {
  const std::size_t nflat = n * kMaxInputs;
  if (scratch.cur.size() < n) {
    scratch.cur.resize(n);
  }
  if (scratch.blk.size() < nflat) {
    scratch.blk.resize(nflat);
  }
  if (scratch.out_head.size() < n) {
    scratch.out_head.resize(n);
  }
  if (scratch.in_head.size() < nflat) {
    scratch.in_head.resize(nflat);
  }
  if (scratch.sink_next.size() < nflat) {
    scratch.sink_next.resize(nflat);
  }
  if (scratch.sink_slot.size() < nflat) {
    scratch.sink_slot.resize(nflat);
  }
  if (scratch.sink_input.size() < nflat) {
    scratch.sink_input.resize(nflat);
  }
};

/**
 * @brief Transient-tolerant twin of `resolve_input`.
 *
 * TransientResolver adapter over the InputResolution seam: span
 * assembly, per-attachment polarity, and the pull-map come from
 * `input_res` (the same assembly the scalar adapter folds loudly) —
 * but never refuses: dead attachments are transparent, X/Z flow
 * through (Z at a gate input reads as X), so transient contention
 * windows and released-buffer windows stay visible in the trace
 * instead of failing. No contention counting (the settled path owns
 * SolveDiag).
 *
 * @param[in] nl Netlist holding drivers, marks, and polarities.
 * @param[in] slot Gate slot of the sink.
 * @param[in] input 0-based sink input index.
 * @param[in] cur Current per-slot outputs (flop slots hold Q).
 * @param[in] blk Current dangling-net values (leaders authoritative).
 * @param[out] out Resolved four-state input value.
 */
inline void timing_resolve_input(const Netlist &nl, Slot slot, std::uint8_t input, const LogicValue *cur, const LogicValue *blk, LogicValue &out) noexcept {
  auto gate_read = [](LogicValue v) noexcept {
    return (v == LogicValue::kHighZ) ? LogicValue::kUnknown : v;
  };
  if (slot >= slot_count(nl) || input >= kMaxInputs) {
    out = LogicValue::kUnknown;
    return;
  }
  input_res::InputSpan span{};
  input_res::assemble_input_span(nl, slot, input, span);
  if (span.is_shared || span.n > 0) {
    // Shared span and single-driver wired share one fold: polarize each
    // live attachment (seam polarity, 4-state-preserving), funnel once,
    // pull-map once, then the transient gate read (Z -> X). Dead
    // attachments are transparent here by design (the scalar adapter
    // refuses them loudly on the same assembly) — the wheel never
    // refuses mid-window.
    LogicValue vals[1 + kMaxSharedExtra];
    std::size_t n = 0;
    for (std::size_t k = 0; k < span.n; ++k) {
      const Slot d = span.drv[k];
      if (d >= slot_count(nl) || !nl.slots[d].live) {
        continue; // transparent: contributes nothing to the funnel
      }
      vals[n++] = input_res::apply_span_polarity(cur[d], span.inv[k]);
    }
    LogicValue sunk = LogicValue::kHighZ;
    if (!resolve_net(vals, n, sunk)) {
      out = LogicValue::kUnknown; // ill-formed span: unreachable (n <= 1+cap)
      return;
    }
    sunk = input_res::pull_map_idle(nl, span.flat, sunk);
    out = gate_read(sunk);
    return;
  }
  if (input_res::input_is_ignored(nl, slot, input)) {
    out = LogicValue::kLow;
    return;
  }
  const std::size_t leader = tie_leader_flat(nl, span.flat);
  LogicValue v = blk[leader];
  v = input_res::pull_map_idle(nl, span.flat, v);
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
 * @param[out] scratch Scratch receiving `blk`/`cur` (sequential `cur` preserved;
 *            pool vectors grown to the pool size first, so adapters may
 *            preload sequential `cur` only after sizing via
 *            `ensure_timing_scratch`).
 * @param[in] filter Topology edge set (`kAll` or `kCombOnly`).
 * @return kOk on success; kInvalidArgument on cycle/size/X/Z errors.
 *
 * (No noexcept: pool workspace and scratch growth may allocate.)
 */
[[nodiscard]] inline core::Status timing_settle_initial(Netlist &nl, std::span<const LogicValue> block_inputs, TimingScratch &scratch, EdgeFilter filter) {
  core::Status st = topo::rebuild(nl, filter);
  if (!st.ok()) {
    return st;
  }
  // Single dangling-idle prelude (BlockInterface seam): zero, consume
  // leaders, terminate pulls/ignores, fan out ties. The wheel holds no
  // twin — parity with `solve()` holds by construction.
  ensure_timing_scratch(scratch, slot_count(nl));
  st = fill_resolved(nl, block_inputs, std::span<LogicValue>(scratch.blk.data(), scratch.blk.size()));
  if (!st.ok()) {
    return st;
  }
  LogicValue ins[kMaxInputs];
  for (std::size_t oi = 0; oi < nl.topo_order.size(); ++oi) {
    const Slot s = nl.topo_order[oi];
    const Gate &g = nl.slots[s];
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
[[nodiscard]] inline core::Status schedule_at(TimingScratch &scratch, std::uint64_t t, Slot slot, bool force, LogicValue val) noexcept {
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
[[nodiscard]] inline core::Status schedule_consumers(const Netlist &nl, TimingScratch &scratch, const Slot *head, std::size_t key, std::uint64_t now) noexcept {
  for (Slot e = head[key]; e != kNoDriver; e = scratch.sink_next[e]) {
    const Slot c = scratch.sink_slot[e];
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
 * @brief Build the topology-invariant reverse sink maps once (research D1).
 *
 * Routes driver-slot and leader-flat changes to consumer (slot,
 * input) pairs in slot order: every consumer reachable from the net
 * that changed (driver slot, shared extra, or dangling leader
 * flat). Static nets (ignored, pulled, unmarked-shared empties)
 * never change and take no entry. Depends only on connectivity
 * (drivers, shared marks/extras, ties, ignored, pulls) — never on
 * `cur`/`blk`, the pending schedule, or the trace — so adapters
 * build once after the last connectivity change and reuse the maps
 * across windows (`truth` rows, `step` edges). Rebuild after any
 * wiring change; `timing_settle_initial` (topo order + `cur`/`blk`)
 * never touches the maps and never invalidates them.
 *
 * @param[in] nl Netlist to observe (read-only; never mutated).
 * @param[out] scratch Working state receiving `out_head`/`in_head`/
 *            `sink_next`/`sink_slot`/`sink_input`/`n_sink` (pool vectors
 *            grown to the pool size first; pending schedule, `cur`, and
 *            `blk` untouched).
 *
 * (No noexcept: scratch growth may allocate.)
 */
inline void timing_build_sink_maps(const Netlist &nl, TimingScratch &scratch) {
  const std::size_t n = slot_count(nl);
  const std::size_t nflat = n * kMaxInputs;
  ensure_timing_scratch(scratch, n);
  scratch.n_sink = 0;

  for (Slot s = 0; s < n; ++s) {
    scratch.out_head[s] = kNoDriver;
  }
  for (std::size_t f = 0; f < nflat; ++f) {
    scratch.in_head[f] = kNoDriver;
  }
  // Reverse sink map in slot order: every consumer reachable from
  // the net that changed (driver slot, shared extra, or dangling
  // leader flat). Static nets (ignored, pulled, unmarked-shared
  // empties) never change and take no entry.
  for (Slot c = 0; c < n; ++c) {
    const Gate &g = nl.slots[c];
    if (!g.live) {
      continue;
    }
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      const std::size_t flat = tie_flat(c, i);
      const Slot drv = g.drivers[i];
      bool dynamic = false;
      if (drv != kNoDriver) {
        if (drv < n && nl.slots[drv].live && scratch.n_sink < nflat) {
          const std::size_t e = scratch.n_sink++;
          scratch.sink_slot[e] = c;
          scratch.sink_input[e] = i;
          scratch.sink_next[e] = scratch.out_head[drv];
          scratch.out_head[drv] = static_cast<Slot>(e);
        }
        dynamic = true;
      }
      if (is_shared_flat(nl, flat)) {
        for_each_extra(nl, flat, [&](std::uint8_t, Slot edrv, std::uint8_t) {
          if (edrv < n && nl.slots[edrv].live && scratch.n_sink < nflat) {
            const std::size_t e = scratch.n_sink++;
            scratch.sink_slot[e] = c;
            scratch.sink_input[e] = i;
            scratch.sink_next[e] = scratch.out_head[edrv];
            scratch.out_head[edrv] = static_cast<Slot>(e);
          }
        });
        dynamic = true;
      }
      if (!dynamic && drv == kNoDriver && (g.ignored_mask & static_cast<std::uint8_t>(1u << i)) == 0 &&
          !is_pulled_flat(nl, flat)) {
        const std::size_t leader = tie_leader_flat(nl, flat);
        if (scratch.n_sink < nflat) {
          const std::size_t e = scratch.n_sink++;
          scratch.sink_slot[e] = c;
          scratch.sink_input[e] = i;
          scratch.sink_next[e] = scratch.in_head[leader];
          scratch.in_head[leader] = static_cast<Slot>(e);
        }
      }
    }
  }
}

/**
 * @brief Run one transport-delay window to drain (research D4/D6).
 *
 * Precondition: the adapter settled `scratch` (`cur`/`blk`) to the
 * window's pre-state with `timing_settle_initial` and preloaded
 * sequential `cur` (post-step Q for step windows, kLow otherwise),
 * and built the sink maps once with `timing_build_sink_maps` after
 * the last connectivity change. This is the pure drain: it resets
 * only the pending schedule and the trace, then applies seeds and
 * propagates to drain reusing the prebuilt maps untouched.
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
 * @param[in,out] scratch Working state with prebuilt sink maps
 *            (pending schedule reset here; maps reused untouched;
 *            pool vectors grown to the pool size first, preserving
 *            map contents).
 * @param[in] recompute_latches True in step windows: DLATCH/SRLATCH
 *            outputs recompute mid-window at their response delay;
 *            false in solve/truth windows (sequential gates are
 *            constants there, mirroring zero-delay `solve`).
 * @return kOk with a drained trace; kInternal on cap/overflow;
 *         kInvalidArgument on a seed naming an out-of-pool slot
 *         (block seeds: out-of-range input).
 *
 * (No noexcept: scratch growth may allocate.)
 */
[[nodiscard]] inline core::Status run_timing_window_inner(const Netlist &nl, std::span<const TimingSeed> seeds, TimingTrace &trace, TimingScratch &scratch, bool recompute_latches) {
  const std::size_t n = slot_count(nl);
  ensure_timing_scratch(scratch, n);
  trace.n_events = 0;
  trace.drain_ps = 0;
  trace.unit_ps = kTimingUnitPs;
  trace.n_annotated = 0;
  for (Slot s = 0; s < n; ++s) {
    if (nl.slots[s].live && nl.slots[s].propagation_delay_ps != 0) {
      ++trace.n_annotated;
    }
  }
  scratch.n_pending = 0;
  // Seeds in span order (callers pass slot order): block nets apply
  // at 0, flop forces schedule at their fire time.
  for (const TimingSeed &seed : seeds) {
    if (seed.slot >= n) {
      return core::Status::failure(core::StatusCode::kInvalidArgument);
    }
    if (seed.is_flop) {
      core::Status st = timing_detail::schedule_at(scratch, seed.fire_ps, seed.slot, true, seed.new_value);
      if (!st.ok()) {
        return st;
      }
      continue;
    }
    if (seed.input >= kMaxInputs) {
      return core::Status::failure(core::StatusCode::kInvalidArgument);
    }
    const std::size_t leader = tie_leader_flat(nl, tie_flat(seed.slot, seed.input));
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
    for (Slot s = 0; s < n; ++s) {
      const Gate &g = nl.slots[s];
      if (!g.live) {
        continue;
      }
      for (std::uint8_t i = 0; i < g.input_count; ++i) {
        if (g.drivers[i] != kNoDriver) {
          continue;
        }
        const std::size_t flat = tie_flat(s, i);
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
    const Slot slot = scratch.pending_s[best];
    const bool force = scratch.pending_force[best] != 0;
    const LogicValue forced = scratch.pending_val[best];
    scratch.pending_t[best] = scratch.pending_t[scratch.n_pending - 1];
    scratch.pending_s[best] = scratch.pending_s[scratch.n_pending - 1];
    scratch.pending_force[best] = scratch.pending_force[scratch.n_pending - 1];
    scratch.pending_val[best] = scratch.pending_val[scratch.n_pending - 1];
    --scratch.n_pending;
    if (slot >= n || !nl.slots[slot].live) {
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
      for (Slot e = scratch.out_head[slot]; e != kNoDriver; e = scratch.sink_next[e]) {
        const Slot c = scratch.sink_slot[e];
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
    const Gate &g = nl.slots[slot];
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
 * Single-shot convenience: builds the sink maps, then drains via
 * `run_timing_window_inner`. Hot loops (`truth` rows, `step` edges)
 * build once with `timing_build_sink_maps` and call
 * `run_timing_window_inner` per window instead. Clears the trace on
 * refusal so no partial trace escapes (the adapter refuses before
 * printing anything in that case).
 *
 * @param[in] nl Netlist to observe (read-only; never mutated).
 * @param[in] seeds Seed span (slot order within each fire time).
 * @param[out] trace Filled trace (cleared on refusal).
 * @param[in,out] scratch Working state (sink maps built here, pool
 *            vectors grown to the pool size first).
 * @param[in] recompute_latches True in step windows, false in
 *            solve/truth windows (see `run_timing_window_inner`).
 * @return kOk with a drained trace; kInternal on cap/overflow;
 *         kInvalidArgument on a seed naming an out-of-pool slot
 *         (block seeds: out-of-range input).
 *
 * (No noexcept: sink-map and scratch growth may allocate.)
 */
[[nodiscard]] inline core::Status run_timing_window(const Netlist &nl, std::span<const TimingSeed> seeds, TimingTrace &trace, TimingScratch &scratch, bool recompute_latches) {
  timing_build_sink_maps(nl, scratch);
  const core::Status st = run_timing_window_inner(nl, seeds, trace, scratch, recompute_latches);
  if (!st.ok()) {
    trace.n_events = 0;
    trace.drain_ps = 0;
  }
  return st;
}

} // namespace wengine::logic
