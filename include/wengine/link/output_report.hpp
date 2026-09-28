#pragma once

// Shared edge-side live-output health report (PHYSI-39).
//
// One formatting/accounting vocabulary for every live output (SHM/TCP/UDP)
// so CLI finite + --forever summaries, benchmarks, and future GUI panels
// separate each cause of dropped data identically. Publishers stay
// header-only-friendly: this header is pure + allocation-tolerant (cold
// edge path only, never the hot on_snapshot path).
//
// Causes, per docs/delivery.md §5 + PHYSI-39 acceptance:
// - attempted: every on_snapshot call (PublisherStats::attempted).
// - intentional stride/subset skips: routing wrappers (emit/routing.hpp),
//   passed in by the caller — never folded into PublisherStats.
// - over-bound: shared encode-bound refusal (sticky kOutOfRange).
// - queue-full: reserved 0 until the PHYSI-45 bounded owning queue lands.
// - socket-backpressure: transient kernel/peer refusal (dropped_send).
// - pending-blocked: TCP new frame dropped while the depth-1 pending tail
//   is still blocked (dropped_pending_blocked).
// - pending: current TCP tail state (has_pending_frame/pending_bytes).
// - abandoned-at-close: pending tail still held at finish/close time.
// - disabled-after-error: snapshots ignored once sticky (including the
//   frame that triggered the sticky failure).
//
// Labeling rule: publisher counts are LOCAL acceptance (bus / kernel).
// Remote receipt is unknown unless a receiver reports its own LagStats —
// every formatted line states this once via kLocalOnlyNote.

#include "wengine/core/status.hpp"
#include "wengine/link/protocol.hpp"

#include <cstdint>
#include <string>

namespace wengine::link {

// Appended to every publisher summary so local acceptance is never read
// as remote receipt (PHYSI-39 labeling rule).
inline constexpr const char *kLocalOnlyNote =
    "local acceptance only; remote receipt unknown without receiver LagStats";

[[nodiscard]] inline bool output_accounting_ok(const PublisherStats &s,
                                              std::uint64_t pending_frames) noexcept {
  return s.attempted == s.published + s.dropped_over_bound + s.dropped_send +
                            s.dropped_pending_blocked + s.dropped_queue_full +
                            s.disabled_after_error + pending_frames;
}

[[nodiscard]] inline const char *status_name(core::StatusCode code) noexcept {
  switch (code) {
    case core::StatusCode::kOk:
      return "ok";
    case core::StatusCode::kInvalidArgument:
      return "kInvalidArgument";
    case core::StatusCode::kOutOfRange:
      return "kOutOfRange";
    case core::StatusCode::kNotImplemented:
      return "kNotImplemented";
    case core::StatusCode::kInternal:
      return "kInternal";
  }
  return "?";
}

// Single-line publisher health: everyognised cause named exactly once.
// `pending_frames` is 0/1 (TCP tail present), `abandoned_frames` is the
// edge-observed tail at finish/close (normally == pending_frames then).
[[nodiscard]] inline std::string format_publisher_health(
    const PublisherStats &s, core::Status status, int last_errno,
    std::uint64_t pending_frames, std::uint64_t pending_bytes,
    std::uint64_t abandoned_frames) {
  std::string r;
  r += "attempted=" + std::to_string(s.attempted);
  r += " published_local=" + std::to_string(s.published);
  r += " over_bound=" + std::to_string(s.dropped_over_bound);
  r += " queue_full=" + std::to_string(s.dropped_queue_full);
  r += " backpressure=" + std::to_string(s.dropped_send);
  r += " pending_blocked=" + std::to_string(s.dropped_pending_blocked);
  r += " pending=" + std::to_string(pending_frames);
  r += "/" + std::to_string(pending_bytes) + "B";
  r += " abandoned=" + std::to_string(abandoned_frames);
  r += " disabled=" + std::to_string(s.disabled_after_error);
  r += " bytes=" + std::to_string(s.bytes_accepted);
  r += " status=";
  r += status_name(status.code);
  r += " errno=" + std::to_string(last_errno);
  if (!output_accounting_ok(s, pending_frames)) {
    r += " ACCOUNTING_MISMATCH";
  }
  r += " (";
  r += kLocalOnlyNote;
  r += ")";
  return r;
}

}  // namespace wengine::link
