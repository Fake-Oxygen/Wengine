#pragma once

// SHM reader: read-only endpoint of the wengine-link snapshot bus.
//
// Attaches to a live publisher segment by name and pulls clean frames
// (seqlock torn-read retry is inside slot_try_read; only kOk frames
// surface). Two read styles share one wire:
//
//   * Latest-only (live displays): read_latest scans the ring for the
//     highest frame_seq, poll_newer tracks one reader's cursor with
//     exact missed-frame accounting, read_latest_frame adds decode.
//     Skips here are intentional coalescing, not loss to fix.
//   * Ordered drain (bounded jitter cover): poll_next / read_batch walk
//     retained sequences in ascending order with a caller bound on work.
//     Bursts smaller than the ring drain with zero loss; bursts larger
//     than the ring report the exact overwritten interval (skipped) and
//     resume from the oldest retained frame. Overrun skips are real loss
//     (writer drop-oldest), distinct from latest-only intentional skips.
//
// The GUI subscriber mode (gui-integration epic) builds on poll_newer +
// adapt_frame; the sample app builds on read_latest_frame; paced
// telemetry/record paths that need every retained sample build on
// poll_next / read_batch + decode_frame.
//
// ORDERED DRAIN CONTRACT:
//   * Cursor-based: the caller owns a uint64_t cursor (0 = nothing
//     consumed yet; frame_seq starts at 1). Only kOk advances it.
//   * Caller buffers: poll_next fills one caller LinkFrame, read_batch
//     fills a caller array (no allocations, no internal queue).
//   * Bounded work: poll_next performs a bounded rescan (like
//     read_latest); read_batch stops at the caller capacity and reports
//     whether more is retained (capped) or a transient writer race cut
//     the batch short (busy).
//   * Non-blocking: the publisher is never waited on. kBusy means "a
//     slot was being written/torn during the scan — retry later"; kEmpty
//     means "no retained sequence beyond the cursor right now".
//   * `out` is valid only on kOk (a torn read may clobber it, matching
//     slot_try_read).
//   * This absorbs bounded jitter only (up to kSlotCount retained
//     frames, ~130 ms at 60 Hz emit). A persistently slower reader still
//     overruns: each overrun reports its exact skipped span loudly via
//     `skipped`, never silently. Complete capture needs a paced driver
//     or an explicit recording budget — faster publishing alone is not a
//     metric (see docs/delivery.md).
//
// ATTACH GATES = the v1 negotiation (in order): name shape (POSIX
// "/name") -> shm_open read-only -> fstat size == publisher layout ->
// version commit word set -> inspect_header exact match. Tools retry the
// transient states (kMissing/kUnready) until their timeout and fail fast
// otherwise: refusal carries the segment's version via report(), syscall
// failures via last_errno(). A refusal is never retried — same input
// decides the same way every time (deterministic negotiation).
//
// LIFECYCLE: the reader NEVER unlinks (the publisher owns the name).
// Attach after the publisher starts; if the publisher exits and recreates
// the segment, re-attach — a stale mapping keeps showing the old run's
// frozen frames (POSIX fd semantics), which poll_newer reports as "no new
// frames", never as corrupt data. Not a sink; not copyable (owns fd +
// mapping); movable. One cursor (last_seq) per object: share-nothing
// across reader threads, mappings themselves are multi-reader safe.

#include "wengine/core/status.hpp"
#include "wengine/link/frame.hpp"
#include "wengine/link/protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace wengine::link {

// Attach outcome: the negotiation verdict. Only kAttached means ok().
enum class AttachState : std::uint8_t {
  kAttached, // success
  kBadName, // name shape invalid — usage-error class, validate upfront
  kMissing, // shm_open ENOENT — publisher not started yet, retry
  kUnready, // opened but version commit word unset — mid-construction, retry
  kRefused, // complete header, incompatible — fail fast, see report()
  kError, // syscall failure — fail fast, see last_errno()
};

class ShmReader {
 public:
  explicit ShmReader(std::string_view name);
  ~ShmReader();

  ShmReader(const ShmReader &) = delete;
  ShmReader &operator=(const ShmReader &) = delete;
  ShmReader(ShmReader &&other) noexcept;
  ShmReader &operator=(ShmReader &&other) noexcept;

  [[nodiscard]] bool ok() const noexcept { return status_.ok(); }
  [[nodiscard]] core::Status status() const noexcept { return status_; }
  [[nodiscard]] AttachState state() const noexcept { return state_; }
  // Mismatch verdict (meaningful for kRefused; ok-report when attached).
  [[nodiscard]] const HeaderReport &report() const noexcept { return report_; }
  // errno captured at the failing attach syscall (0 when ok).
  [[nodiscard]] int last_errno() const noexcept { return last_errno_; }
  [[nodiscard]] const std::string &name() const noexcept { return name_; }
  // Live segment header, or nullptr while !ok() (never copy it: atomics).
  [[nodiscard]] const SegmentHeader *header() const noexcept { return header_; }
  // Total frames published into the segment (0 while !ok()).
  [[nodiscard]] std::uint64_t published() const noexcept;

  // Highest clean frame on the ring; false when nothing published yet.
  // Only kOk reads surface — Busy/Torn/Empty slots are skipped silently.
  [[nodiscard]] bool read_latest(LinkFrame &out) const;
  // read_latest + decode; false when nothing available (corrupt wire data
  // is unreachable after the attach gates + seqlock filtering).
  [[nodiscard]] bool read_latest_frame(Frame &out) const;
  // Newest frame strictly after last_seq; updates last_seq on success.
  // Exact loss for the skipped span: reader_missed(old, out.frame_seq).
  // Latest-only coalescing: the skip is intentional display behavior,
  // distinct from ordered-drain overrun (see poll_next skipped).
  [[nodiscard]] bool poll_newer(std::uint64_t &last_seq, LinkFrame &out) const;

  // Ordered-drain outcome: only kOk yields a usable frame (`out` valid,
  // cursor advanced). kEmpty = nothing retained beyond the cursor;
  // kBusy = transient writer race, retry later. Cursor unchanged unless
  // kOk. `out` valid only on kOk.
  enum class OrderedOutcome : std::uint8_t { kOk, kEmpty, kBusy };

  // Next retained sequence after cursor, ascending. Zero-loss while the
  // unread span fits the ring; on overrun returns the oldest retained
  // frame with `skipped` = sequences lost between the incoming cursor
  // and out.frame_seq (reader_missed(incoming, out.frame_seq)), then
  // subsequent calls continue in order with skipped == 0. In-order next
  // has skipped == 0. Reuses the caller frame; bounded rescan, never
  // blocks the publisher.
  [[nodiscard]] OrderedOutcome poll_next(std::uint64_t &cursor, LinkFrame &out,
                                        std::uint64_t &skipped) const;
  // poll_next + decode. Cold reader side: may allocate like decode_frame,
  // never call inside the sim loop. Decode failure is unreachable (same
  // filtering as read_latest_frame); it reports kBusy without consuming
  // the sequence so a retry can re-observe it.
  [[nodiscard]] OrderedOutcome poll_next_frame(std::uint64_t &cursor, Frame &out,
                                              std::uint64_t &skipped) const;

  // Bounded ordered batch: fills out[0..capacity) via poll_next until
  // empty, busy, or full. Caller owns the array (no allocations).
  struct OrderedBatch {
    std::size_t returned = 0; // frames stored in out[0..returned)
    std::uint64_t skipped = 0; // total overrun across the batch
    bool busy = false; // stopped early on a transient race; retry to continue
    bool capped = false; // stopped on capacity with more retained (call again)
  };
  [[nodiscard]] OrderedBatch read_batch(std::uint64_t &cursor, LinkFrame *out,
                                       std::size_t capacity) const;

 private:
  void cleanup() noexcept;

  std::string name_;
  int fd_ = -1;
  const void *base_ = nullptr;
  std::size_t mapping_size_ = 0;
  const SegmentHeader *header_ = nullptr;
  const Slot *slots_ = nullptr;
  core::Status status_{};
  AttachState state_ = AttachState::kError;
  HeaderReport report_{};
  int last_errno_ = 0;
};

} // namespace wengine::link
