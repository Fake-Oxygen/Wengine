#pragma once

/**
 * @file
 * N:M routing wrappers: subscriber-side plumbing composing with every sink
 * without changing it. Owns SignalSubsetSink (serial allowlists,
 * mid-run reconfig, tick preservation) and StrideSink (step-aligned
 * per-sink decimation). Depends on emit/sink and world state; never the
 * reverse. Consumed at the app edge wherever one stream fans out to many
 * sinks or many sources narrow to one.
 */
// N:M routing wrappers (SIG-5): subscriber-side plumbing that composes
// with every existing sink/adapter with zero changes to them.
//
// Model: the Snapshot carries ALL live sources; selection happens at the
// edge. `SignalSubsetSink` forwards a per-snapshot serial-filtered copy to
// one inner sink (1 source -> N sinks = N wrappers with different lists;
// N sources -> 1 sink = one wrapper holding N serials; combine both for
// full N:M). `StrideSink` decimates any stream step-aligned
// (`step % stride == 0`), so transports needing lower rates (UART budgets,
// slow GUIs) subscribe cheaply without touching Runner's global
// `emit_every` (see the StrideSink note in link/protocol.hpp).
//
// Both are best-effort and non-throwing like all sinks: a sink failure
// never alters sim state or timing. Routing reconfigures at runtime
// (`set_subscription`/`set_stride`) with no Runner rebuild. Counters make
// drops loud: `dropped()` counts undelivered signal rows (subset: unknown
// or despawned serials; stride: decimated-away frames; muted: everything),
// `matched()` counts forwarded snapshots, `forwarded_rows()` counts signal
// rows actually delivered.
//
// Tick preservation: a non-muted subset forwards EVERY snapshot (possibly
// with zero rows), so downstream SHM readers observe every emitted tick
// and their LagStats stay exact — only rows are filtered, never ticks.

#include "wengine/emit/sink.hpp"
#include "wengine/world/state.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace wengine::emit {

/**
 * @brief Serial-allowlist wrapper: forwards a per-snapshot filtered copy to one inner sink.
 * @note 1 source -> N sinks is N wrappers with different lists; N sources ->
 * 1 sink is one wrapper holding N serials; combine both for full N:M.
 * A non-muted subset forwards EVERY snapshot (possibly with zero rows), so
 * downstream readers observe every emitted tick and lag accounting stays
 * exact; only rows are filtered, never ticks. Mid-run reconfigurable with
 * no Runner rebuild. Drops are loud via counters, never silent.
 */
class SignalSubsetSink : public ISink {
public:
/**
 * @brief Attach to an inner sink.
 * @param[in,out] inner downstream sink receiving the filtered copies (must outlive this wrapper;
 * this wrapper never owns it).
 */
  explicit SignalSubsetSink(ISink &inner) : inner_(inner) {}

  // Replace the subscription (source serials, unique; empty = muted).
  // Takes effect on the next snapshot — reconfigurable mid-run without
  // a rebuild.
/**
 * @brief Replace the subscription (source serials, unique; empty = muted).
 * @param[in] serials new allowlist; takes effect on the next snapshot, no rebuild.
 * @note Mid-run reconfigurable; muted snapshots count as deliberate drops.
 */
  void set_subscription(std::vector<std::uint64_t> serials) {
    serials_ = std::move(serials);
  }

/**
 * @brief Current allowlist.
 * @return The subscribed source serials.
 */
  [[nodiscard]] const std::vector<std::uint64_t> &subscription() const noexcept {
    return serials_;
  }
/** @brief Forwarded snapshots, ever. @return Count of snapshots passed to the inner sink. */
  [[nodiscard]] std::uint64_t matched() const noexcept { return matched_; }
/** @brief Undelivered rows, ever. @return Unknown or despawned serials plus muted-snapshot drops. */
  [[nodiscard]] std::uint64_t dropped() const noexcept { return dropped_; }
/** @brief Signal rows actually delivered, ever. @return Count of delivered signal rows. */
  [[nodiscard]] std::uint64_t forwarded_rows() const noexcept { return forwarded_rows_; }

/**
 * @brief Filter one snapshot by serial and forward the copy.
 * @param[in] s emitted snapshot (the inner sink gets the same
 * valid-during-the-call reference the Runner hands out; it must copy before returning).
 * @note Reused scratch buffer: steady same-shape traffic performs no
 * per-snapshot allocation after warmup.
 */
  void on_snapshot(const world::Snapshot &s) override {
    if (serials_.empty()) {
      ++dropped_; // muted sink: every snapshot is a deliberate drop
      return;
    }
    // Reused scratch: steady same-shape traffic performs no per-snapshot
    // allocation after warmup. The inner sink gets the same
    // valid-during-the-call reference the Runner hands out (see
    // emit/sink.hpp) — it must copy before returning.
    world::select_signals_into(s, std::span(serials_.data(), serials_.size()), scratch_);
    ++matched_;
    forwarded_rows_ += static_cast<std::uint64_t>(scratch_.signal_count());
    dropped_ += static_cast<std::uint64_t>(serials_.size()) - static_cast<std::uint64_t>(scratch_.signal_count());
    inner_.on_snapshot(scratch_);
  }

private:
  ISink &inner_;
  world::Snapshot scratch_; // reused subset buffer (see on_snapshot)
  std::vector<std::uint64_t> serials_{};
  std::uint64_t matched_{0};
  std::uint64_t dropped_{0};
  std::uint64_t forwarded_rows_{0};
};

/**
 * @brief Step-aligned decimation wrapper: forwards only steps with step_index % stride == 0.
 * @note Lets transports needing lower rates (UART budgets, slow GUIs) subscribe
 * cheaply without touching the Runner's global emit_every. 0 = muted (never
 * forwards, counts everything as dropped). Drops are loud via counters, never silent.
 */
class StrideSink : public ISink {
public:
/**
 * @brief Attach to an inner sink with a decimation stride.
 * @param[in,out] inner downstream sink (must outlive this wrapper; never owned).
 * @param[in] stride forward every stride-th step; 1 = every frame, 0 = muted.
 */
  explicit StrideSink(ISink &inner, std::uint64_t stride = 1) : inner_(inner), stride_(stride) {}

  // New stride takes effect on the next snapshot. 1 = every frame,
  // 0 = muted (never forwards, counts everything as dropped).
/**
 * @brief Replace the stride.
 * @param[in] stride new stride; takes effect on the next snapshot.
 */
  void set_stride(std::uint64_t stride) noexcept { stride_ = stride; }

/** @brief Current stride. @return The active decimation stride (0 = muted). */
  [[nodiscard]] std::uint64_t stride() const noexcept { return stride_; }
/** @brief Forwarded snapshots, ever. @return Count of snapshots passed to the inner sink. */
  [[nodiscard]] std::uint64_t matched() const noexcept { return matched_; }
/** @brief Decimated-away snapshots, ever. @return Count of snapshots not forwarded. */
  [[nodiscard]] std::uint64_t dropped() const noexcept { return dropped_; }

/**
 * @brief Forward one snapshot when its step aligns with the stride.
 * @param[in] s emitted snapshot.
 */
  void on_snapshot(const world::Snapshot &s) override {
    if (stride_ == 0 || (s.step_index % stride_ != 0)) {
      ++dropped_;
      return;
    }
    ++matched_;
    inner_.on_snapshot(s);
  }

private:
  ISink &inner_;
  std::uint64_t stride_;
  std::uint64_t matched_{0};
  std::uint64_t dropped_{0};
};

} // namespace wengine::emit
