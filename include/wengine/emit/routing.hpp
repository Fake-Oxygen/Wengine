#pragma once

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

class SignalSubsetSink : public ISink {
public:
  explicit SignalSubsetSink(ISink &inner) : inner_(inner) {}

  // Replace the subscription (source serials, unique; empty = muted).
  // Takes effect on the next snapshot — reconfigurable mid-run without
  // a rebuild.
  void set_subscription(std::vector<std::uint64_t> serials) {
    serials_ = std::move(serials);
  }

  [[nodiscard]] const std::vector<std::uint64_t> &subscription() const noexcept {
    return serials_;
  }
  [[nodiscard]] std::uint64_t matched() const noexcept { return matched_; }
  [[nodiscard]] std::uint64_t dropped() const noexcept { return dropped_; }
  [[nodiscard]] std::uint64_t forwarded_rows() const noexcept { return forwarded_rows_; }

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

class StrideSink : public ISink {
public:
  explicit StrideSink(ISink &inner, std::uint64_t stride = 1) : inner_(inner), stride_(stride) {}

  // New stride takes effect on the next snapshot. 1 = every frame,
  // 0 = muted (never forwards, counts everything as dropped).
  void set_stride(std::uint64_t stride) noexcept { stride_ = stride; }

  [[nodiscard]] std::uint64_t stride() const noexcept { return stride_; }
  [[nodiscard]] std::uint64_t matched() const noexcept { return matched_; }
  [[nodiscard]] std::uint64_t dropped() const noexcept { return dropped_; }

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
