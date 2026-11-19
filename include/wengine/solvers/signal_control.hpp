#pragma once

/**
 * @file
 * SIG-10 generic control plane, driver half: ControlBus queues raw
 * command lines per channel, drains them in fixed registration order at
 * deterministic step boundaries, answers through per-channel outboxes,
 * and logs every applied mutating verb for replay. Owns the
 * state-mutating half only — transports (TCP, UART, CLI) move lines and
 * live outside; this header depends on core/signal/world, never CLI
 * types. Consumed by the CLI/REPL control commands, the paced-run
 * before-chunk drain, and tests/test_signal_control.cpp.
 */

// SIG-10 generic control plane: state-mutating half (driver side).
//
// ControlBus owns per-channel command queues drained at deterministic step
// boundaries in fixed channel order (registration order = drain order), a
// per-channel response outbox, and the replay log. Transports (TCP server,
// UART framer, CLI) only move lines: they enqueue raw command lines and
// collect response lines — all verbs execute here, between steps, on the
// single driver thread (never mid-step, never blocking, never throwing).
//
// Determinism contract (locked 2026-09-23):
// - Any number of channels may attach (Zephyr via UART + PC via TCP +
//   local CLI). Channels drain in fixed registration order, each FIFO.
// - Last-writer-wins per (source, param) within a step falls out of the
//   sequential apply: later channels overwrite earlier ones. Idempotent
//   verbs (START+START) commute; conflicting verbs (START vs STOP in one
//   step) resolve by channel order, never by arrival timing.
// - Every applied MUTATING command (ADD/SET/START/STOP/DROP) is appended
//   to the replay log as (step, channel, verb line). Read-only verbs
//   (HELLO/PING/LIST) answer without logging; rejected lines are counted,
//   never logged. Replaying the log against a fresh run (same channel
//   order, same steps) reproduces the session exactly.
//
// Pause/resume exactness: STOP freezes the source by shifting its time
// base (`t0 -= sim_time`, so t0 encodes the frozen local time) and
// clearing `running` (the solver skips stopped slots: value and noise RNG
// frozen). START re-bases (`t0 += sim_time`) and resumes stepping, so the
// waveform continues phase-exactly across the stopped interval — including
// across a save/load round-trip in between (raw doubles round-trip, the
// resume math only needs the loaded clock). No pool growth, no Snapshot
// growth, no serialization change for this: the encoding is contained in
// these two verbs and proven by test (sine continuity, noise-sequence
// continuity, stopped save/load/resume).
//
// SET SEED restarts the noise sequence (seed AND live RNG take the new
// value): deterministic by construction. Quantity changes re-tag the
// source; the SI value is untouched. DROP detaches the pool slot; the
// entity handle stays alive (harmless — entities may outlive components)
// and the serial is never reused, so "source gone" stays unambiguous.
// ADD table/lua and SELECT are parsed by signal/control.hpp but refused
// here as not_implemented (bulk payloads ride batch paths; mux needs its
// combinator) — loud, never silent.

#include "wengine/core/status.hpp"
#include "wengine/signal/control.hpp"
#include "wengine/signal/types.hpp"
#include "wengine/world/state.hpp"

#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace wengine::solvers {

// One applied mutating command, in drain order (replay: re-enqueue each
// line into the same channel before the recorded step, then step once).
/**
 * @brief One applied mutating command, in drain order.
 */
struct AppliedCommand {
  std::uint64_t step = 0; /**< @brief state.step_index at drain (the step about to run). */
  /** @brief Channel the line arrived on (registration index). */
  std::uint32_t channel = 0;
  /** @brief The raw verb line as received (replay re-enqueues it verbatim). */
  std::string line;
};

/** @brief Per-drain verdict counts (mutating vs read-only vs rejected). */
struct DrainStats {
  std::uint64_t applied = 0; /**< @brief Mutating verbs, logged for replay. */
  std::uint64_t readonly = 0; /**< @brief HELLO/PING/LIST successes (answered, unlogged). */
  std::uint64_t rejected = 0; /**< @brief Malformed or refused lines (ERR, unlogged). */
};

namespace control_detail {

// Pool slot -> params copy for SET validation (mirrors the solver's view;
// sums carry their op_count so validate_params sees the live shape).
/**
 * @brief Copy a pool slot's effective parameters for SET validation.
 *
 * @param[in] pool live signal pool (read-only view).
 * @param[in] slot pool slot index to read.
 * @return The slot's SignalParams, including expression program and sum
 *         op_count, so validate_params sees the live shape.
 */
[[nodiscard]] inline signal::SignalParams params_of(const world::Signals &pool,
                                                    std::uint32_t slot) noexcept {
  signal::SignalParams p;
  p.kind = static_cast<signal::SignalKind>(pool.kind[slot]);
  p.quantity = static_cast<signal::QuantityId>(pool.quantity[slot]);
  p.freq_hz = pool.freq[slot];
  p.amp = pool.amp[slot];
  p.offset = pool.offset[slot];
  p.phase_rad = pool.phase[slot];
  p.duty = pool.duty[slot];
  p.seed = pool.seed[slot];
  p.op_count = pool.op_count[slot];
  if (p.kind == signal::SignalKind::kExpression) {
    p.expr.size = pool.expr_len[slot];
    for (std::size_t k = 0; k < signal::kMaxExprNodes; ++k) {
      p.expr.nodes[k].op = static_cast<signal::ExprOp>(
          pool.expr_op[static_cast<std::size_t>(slot) * signal::kMaxExprNodes + k]);
      p.expr.nodes[k].value =
          pool.expr_arg[static_cast<std::size_t>(slot) * signal::kMaxExprNodes + k];
    }
  }
  return p;
}

} // namespace control_detail

// Apply one parsed command to `s`. Responses append to `outbox`; applied
// mutating commands append to `log`. `step` is the about-to-run step index
// (for the log), `t_now` the current sim_time (STOP/START time base).
// Never throws; pool is untouched on any refusal.
/**
 * @brief Apply one parsed command to the world state.
 *
 * Responses append to `outbox`; applied mutating commands append to
 * `log`. Never throws; the pool is untouched on any refusal.
 *
 * @param[in,out] s world state; the signal pool is mutated only after
 *                  full validation.
 * @param[in] cmd the parsed verb (ADD/SET/START/STOP/DROP/HELLO/PING/
 *            LIST/SELECT).
 * @param[in] line the raw verb line, logged verbatim on mutating success.
 * @param[in] step the about-to-run step index (replay log stamp).
 * @param[in] channel registration index the line arrived on.
 * @param[in] run_id run identifier echoed by HELLO.
 * @param[in] t_now current simulation time, in seconds (STOP/START
 *            time-base math: t0 -= t_now freezes, t0 += t_now resumes).
 * @param[out] log replay log; one AppliedCommand per applied mutating verb.
 * @param[out] outbox response lines (OK/ERR verdicts, LIST rows).
 * @note Failures answer ERR lines (kInvalidArgument / kNotImplemented /
 *       kInternal in the verdict) rather than returning Status — the
 *       pool is untouched on any refusal, and rejected lines are
 *       counted, never logged. ADD table/lua and SELECT are refused as
 *       kNotImplemented (bulk payloads ride batch paths; mux needs its
 *       combinator) — loud, never silent.
 */
inline void apply_command(world::State &s, const signal::control::Command &cmd,
                          const std::string &line, std::uint64_t step, std::uint32_t channel,
                          std::uint64_t run_id, double t_now,
                          std::vector<AppliedCommand> &log,
                          std::vector<std::string> &outbox) {
  namespace sc = signal::control;
  const auto err = [&](core::Status st, std::string_view what) {
    sc::err_status(st, what, outbox);
  };
  switch (cmd.verb) {
  case sc::Verb::kHello: {
    if (cmd.hello_version != sc::kProtoVersion) {
      err({core::StatusCode::kInvalidArgument},
          "version (want 1, got " + std::to_string(cmd.hello_version) + ")");
      return;
    }
    sc::ok_hello(run_id, outbox);
    return;
  }
  case sc::Verb::kPing: {
    sc::ok_pong(outbox);
    return;
  }
  case sc::Verb::kList: {
    std::size_t n = 0;
    for (std::uint32_t i = 0; i < s.signals.capacity(); ++i) {
      n += (s.signals.has(i) ? 1 : 0);
    }
    sc::ok_list_begin(n, outbox);
    for (std::uint32_t i = 0; i < s.signals.capacity(); ++i) {
      if (!s.signals.has(i)) {
        continue;
      }
      const std::size_t kind_idx = s.signals.kind[i];
      const std::string_view kind_name =
          (kind_idx < signal::kKindNames.size()) ? signal::kKindNames[kind_idx] : "?";
      const auto qty = static_cast<signal::QuantityId>(s.signals.quantity[i]);
      outbox.push_back(sc::format_src_row(
          s.signals.serial[i], kind_name, signal::quantity_suffix(qty), s.signals.freq[i],
          s.signals.amp[i], s.signals.offset[i], s.signals.phase[i], s.signals.duty[i],
          s.signals.seed[i], s.signals.running[i] != 0));
    }
    return;
  }
  case sc::Verb::kSelect: {
    (void)channel;
    err({core::StatusCode::kNotImplemented}, "mux select (no mux combinator yet)");
    return;
  }
  case sc::Verb::kAdd: {
    if (cmd.kind == signal::SignalKind::kTable || cmd.kind == signal::SignalKind::kLua) {
      err({core::StatusCode::kNotImplemented},
          "table/lua need bulk payloads (use batch runs)");
      return;
    }
    signal::SignalParams p = cmd.params;
    if (cmd.kind == signal::SignalKind::kExpression) {
      const auto prog = signal::parse_expression(cmd.expr);
      if (!prog.ok()) {
        err({core::StatusCode::kInvalidArgument},
            "bad EXPR (" + std::string(prog.error) + ")");
        return;
      }
      p.expr = prog.value;
      if (!signal::eval_expression(p.expr, 0.0).ok()) {
        err({core::StatusCode::kInvalidArgument}, "expression fails at t=0");
        return;
      }
    }
    if (cmd.kind == signal::SignalKind::kSum) {
      for (std::uint8_t k = 0; k < cmd.op_count; ++k) {
        const auto slot = world::find_serial(s.signals, cmd.ops[k]);
        if (!slot.has_value()) {
          err({core::StatusCode::kInvalidArgument}, "unknown operand serial");
          return;
        }
        if (s.signals.kind[*slot] == static_cast<std::uint8_t>(signal::SignalKind::kSum)) {
          err({core::StatusCode::kInvalidArgument}, "sum operands must be non-sum");
          return;
        }
        const auto op_q = static_cast<signal::QuantityId>(s.signals.quantity[*slot]);
        if (!signal::is_valid_quantity(op_q) || op_q != p.quantity) {
          err({core::StatusCode::kInvalidArgument}, "sum operands must match sum quantity");
          return;
        }
      }
      p.op_count = cmd.op_count;
      for (std::size_t k = 0; k < signal::SignalParams::kMaxOps; ++k) {
        p.ops[k] = (k < cmd.op_count) ? cmd.ops[k] : 0;
      }
    }
    if (!signal::validate_params(p).ok()) {
      err({core::StatusCode::kInvalidArgument}, "bad params (freq > 0, duty in 0..1)");
      return;
    }
    // Pre-validation above mirrors configure_source, so the spawn below
    // cannot fail on a single-threaded driver; the guard stays for safety.
    const world::Entity e = s.spawn_entity();
    if (!world::configure_source(e, s.signals, p, t_now).ok()) {
      s.despawn_entity(e);
      err({core::StatusCode::kInternal}, "add failed");
      return;
    }
    log.push_back(AppliedCommand{step, channel, line});
    sc::ok_added(s.signals.serial[e.index], outbox);
    return;
  }
  case sc::Verb::kSet: {
    const auto slot = world::find_serial(s.signals, cmd.serial);
    if (!slot.has_value()) {
      err({core::StatusCode::kInvalidArgument}, "unknown source");
      return;
    }
    signal::SignalParams p = control_detail::params_of(s.signals, *slot);
    switch (cmd.param) {
    case sc::Param::kFreq:
      p.freq_hz = cmd.number;
      break;
    case sc::Param::kAmp:
      p.amp = cmd.number;
      break;
    case sc::Param::kOffset:
      p.offset = cmd.number;
      break;
    case sc::Param::kPhase:
      p.phase_rad = cmd.number;
      break;
    case sc::Param::kDuty:
      p.duty = cmd.number;
      break;
    case sc::Param::kSeed:
      p.seed = cmd.seed;
      break;
    case sc::Param::kQuantity:
      p.quantity = cmd.set_quantity;
      break;
    case sc::Param::kUnknown:
      err({core::StatusCode::kInvalidArgument}, "unknown param");
      return;
    }
    if (!signal::validate_params(p).ok()) {
      err({core::StatusCode::kInvalidArgument}, "bad value (freq > 0, duty in 0..1)");
      return;
    }
    // Dimensional rule for sums (no per-quantity switch — QuantityId
    // equality only): a sum retag must still match all its live operands;
    // an operand retag must still match every live dependent sum. Either
    // mismatch fails before pool mutation, so reconfiguration cannot
    // bypass the configure/load rule.
    if (cmd.param == sc::Param::kQuantity) {
      const auto new_q = cmd.set_quantity;
      const auto target_kind = static_cast<signal::SignalKind>(s.signals.kind[*slot]);
      if (target_kind == signal::SignalKind::kSum) {
        for (std::uint8_t k = 0; k < s.signals.op_count[*slot]; ++k) {
          const std::uint64_t want = s.signals.ops[*slot * signal::SignalParams::kMaxOps + k];
          const auto oslot = world::find_serial(s.signals, want);
          if (!oslot.has_value() ||
              s.signals.kind[*oslot] == static_cast<std::uint8_t>(signal::SignalKind::kSum)) {
            err({core::StatusCode::kInvalidArgument}, "sum operands must be live non-sum");
            return;
          }
          const auto op_q = static_cast<signal::QuantityId>(s.signals.quantity[*oslot]);
          if (!signal::is_valid_quantity(op_q) || op_q != new_q) {
            err({core::StatusCode::kInvalidArgument}, "sum quantity must match operands");
            return;
          }
        }
      } else {
        const std::uint64_t target_serial = s.signals.serial[*slot];
        for (std::uint32_t j = 0; j < s.signals.capacity(); ++j) {
          if (!s.signals.has(j) || s.signals.kind[j] != static_cast<std::uint8_t>(signal::SignalKind::kSum)) {
            continue;
          }
          bool uses = false;
          for (std::uint8_t k = 0; k < s.signals.op_count[j]; ++k) {
            if (s.signals.ops[j * signal::SignalParams::kMaxOps + k] == target_serial) {
              uses = true;
              break;
            }
          }
          if (uses && static_cast<signal::QuantityId>(s.signals.quantity[j]) != new_q) {
            err({core::StatusCode::kInvalidArgument}, "quantity must match dependent sums");
            return;
          }
        }
      }
    }
    // Validated: commit the single field (pool untouched on any refusal).
    switch (cmd.param) {
    case sc::Param::kFreq:
      s.signals.freq[*slot] = cmd.number;
      break;
    case sc::Param::kAmp:
      s.signals.amp[*slot] = cmd.number;
      break;
    case sc::Param::kOffset:
      s.signals.offset[*slot] = cmd.number;
      break;
    case sc::Param::kPhase:
      s.signals.phase[*slot] = cmd.number;
      break;
    case sc::Param::kDuty:
      s.signals.duty[*slot] = cmd.number;
      break;
    case sc::Param::kSeed:
      // Restart the noise sequence deterministically (live RNG follows).
      s.signals.seed[*slot] = cmd.seed;
      s.signals.rng[*slot] = cmd.seed;
      break;
    case sc::Param::kQuantity:
      s.signals.quantity[*slot] = static_cast<std::uint8_t>(cmd.set_quantity);
      break;
    case sc::Param::kUnknown:
      return; // unreachable: rejected above
    }
    log.push_back(AppliedCommand{step, channel, line});
    sc::ok_set(cmd.serial, cmd.param, outbox);
    return;
  }
  case sc::Verb::kStart: {
    const auto slot = world::find_serial(s.signals, cmd.serial);
    if (!slot.has_value()) {
      err({core::StatusCode::kInvalidArgument}, "unknown source");
      return;
    }
    if (s.signals.running[*slot] == 0) {
      s.signals.t0[*slot] += t_now; // resume: frozen local time continues
      s.signals.running[*slot] = 1;
    }
    log.push_back(AppliedCommand{step, channel, line});
    sc::ok_started(cmd.serial, outbox);
    return;
  }
  case sc::Verb::kStop: {
    const auto slot = world::find_serial(s.signals, cmd.serial);
    if (!slot.has_value()) {
      err({core::StatusCode::kInvalidArgument}, "unknown source");
      return;
    }
    if (s.signals.running[*slot] != 0) {
      s.signals.t0[*slot] -= t_now; // freeze: t0 now encodes -t_frozen
      s.signals.running[*slot] = 0;
    }
    log.push_back(AppliedCommand{step, channel, line});
    sc::ok_stopped(cmd.serial, outbox);
    return;
  }
  case sc::Verb::kDrop: {
    const auto slot = world::find_serial(s.signals, cmd.serial);
    if (!slot.has_value()) {
      err({core::StatusCode::kInvalidArgument}, "unknown source");
      return;
    }
    // Detach only: the entity handle stays (entities may outlive their
    // components), the serial is never reused — "source gone" is final.
    s.signals.detach(*slot);
    log.push_back(AppliedCommand{step, channel, line});
    sc::ok_dropped(cmd.serial, outbox);
    return;
  }
  }
  err({core::StatusCode::kInternal}, "unreachable verb");
}

// Step-boundary command bus: fixed-order channels, response outboxes, and
// the replay log. Single driver thread only (poll + drain + step).
/**
 * @brief Step-boundary command bus: fixed-order channels, response
 *        outboxes, and the replay log.
 *
 * Single driver thread only (poll + drain + step): channels drain in
 * fixed registration order, each FIFO; conflicting verbs resolve by
 * channel order, never by arrival timing. Replaying the log against a
 * fresh run (same channel order, same steps) reproduces the session
 * exactly.
 *
 * @note Ownership/lifetime: raw lines are moved in by transports; the
 *       outboxes are taken (moved out) by their transports; the log is
 *       exposed by const reference and lives as long as the bus.
 */
class ControlBus {
public:
  /**
   * @brief Construct a bus bound to one run.
   * @param[in] run_id run identifier echoed by HELLO (channel identity).
   */
  explicit ControlBus(std::uint64_t run_id) : run_id_(run_id) {}

  // Register a channel; ids are dense in registration order and that order
  // IS the drain order (documented at attach: default local CLI, then TCP
  // slots, then UART slots — SIG-8 owns its slots, same rule).
  /**
   * @brief Register a channel and return its id.
   *
   * @return Dense channel id; registration order IS the drain order
   *         (documented at attach: default local CLI, then TCP slots,
   *         then UART slots).
   */
  std::uint32_t add_channel() {
    channels_.emplace_back();
    return static_cast<std::uint32_t>(channels_.size() - 1);
  }

  /** @brief Number of registered channels. @return The dense channel count. */
  [[nodiscard]] std::uint32_t channel_count() const noexcept {
    return static_cast<std::uint32_t>(channels_.size());
  }

  /** @brief Run identifier bound at construction. @return The run id. */
  [[nodiscard]] std::uint64_t run_id() const noexcept { return run_id_; }

  // Enqueue a raw command line (false = unknown channel, line dropped).
  /**
   * @brief Enqueue one raw command line for the next drain.
   *
   * @param[in] channel destination channel id (from add_channel()).
   * @param[in] line raw verb line; moved into the channel queue.
   * @return true when enqueued; false for an unknown channel (line
   *         dropped — callers surface that loudly).
   */
  bool enqueue(std::uint32_t channel, std::string line) {
    if (channel >= channels_.size()) {
      return false;
    }
    channels_[channel].queue.push_back(std::move(line));
    return true;
  }

  // Drain every channel in registration order, each FIFO, applying lines
  // to `s` at (`step`, `s.sim_time`). Malformed and refused lines answer
  // ERR and count as rejected; successes answer OK. Mutating successes are
  // logged for replay; read-only successes are only counted. Returns all
  // three counts.
  /**
   * @brief Drain every channel in registration order, each FIFO.
   *
   * Applies lines to `s` at (step, s.sim_time): malformed and refused
   * lines answer ERR and count as rejected; successes answer OK;
   * mutating successes are logged for replay; read-only successes are
   * only counted.
   *
   * @param[in,out] s world state mutated by applied verbs.
   * @param[in] step the about-to-run step index (log stamp).
   * @return applied / readonly / rejected counts for this drain.
   */
  DrainStats drain(world::State &s, std::uint64_t step) {
    DrainStats stats;
    for (std::uint32_t c = 0; c < channels_.size(); ++c) {
      Channel &ch = channels_[c];
      while (!ch.queue.empty()) {
        std::string line = std::move(ch.queue.front());
        ch.queue.pop_front();
        const auto parsed = signal::control::parse_line(line);
        if (!parsed.ok()) {
          signal::control::err_status({core::StatusCode::kInvalidArgument}, parsed.error,
                                      ch.outbox);
          ++stats.rejected;
          ++rejected_;
          continue;
        }
        const std::size_t log_before = log_.size();
        const std::size_t out_before = ch.outbox.size();
        apply_command(s, parsed.value, line, step, c, run_id_, s.sim_time, log_, ch.outbox);
        if (log_.size() > log_before) {
          ++stats.applied;
          ++applied_;
        } else if (ch.outbox.size() > out_before && !ch.outbox[out_before].empty() &&
                   ch.outbox[out_before][0] == 'O') {
          // Read-only success: the first response line is the OK verdict
          // (LIST appends SRC detail lines after it — hence first, not last).
          ++stats.readonly;
          ++readonly_;
        } else {
          ++stats.rejected;
          ++rejected_;
        }
      }
    }
    return stats;
  }

  // Collect and clear a channel's pending response lines.
  /**
   * @brief Collect and clear a channel's pending response lines.
   *
   * @param[in] channel id whose outbox to take.
   * @return All pending response lines (OK/ERR verdicts, LIST rows),
   *         moved out; the outbox is left empty. Unknown channel id
   *         yields an empty vector.
   */
  [[nodiscard]] std::vector<std::string> take_responses(std::uint32_t channel) {
    std::vector<std::string> out;
    if (channel < channels_.size()) {
      out = std::move(channels_[channel].outbox);
      channels_[channel].outbox.clear();
    }
    return out;
  }

  /** @brief Replay log of applied mutating commands, in drain order. @return Const reference to the log. */
  [[nodiscard]] const std::vector<AppliedCommand> &log() const noexcept { return log_; }
  /** @brief Total applied (mutating, logged) verbs, lifetime of the bus. @return The counter. */
  [[nodiscard]] std::uint64_t applied() const noexcept { return applied_; }
  /** @brief Total read-only successes (answered, unlogged), lifetime of the bus. @return The counter. */
  [[nodiscard]] std::uint64_t readonly() const noexcept { return readonly_; }
  /** @brief Total rejected lines (malformed or refused), lifetime of the bus. @return The counter. */
  [[nodiscard]] std::uint64_t rejected() const noexcept { return rejected_; }

private:
  struct Channel {
    std::deque<std::string> queue;
    std::vector<std::string> outbox;
  };
  std::vector<Channel> channels_;
  std::vector<AppliedCommand> log_;
  std::uint64_t run_id_ = 0;
  std::uint64_t applied_ = 0;
  std::uint64_t readonly_ = 0;
  std::uint64_t rejected_ = 0;
};

} // namespace wengine::solvers
