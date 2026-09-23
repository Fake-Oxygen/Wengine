#pragma once

// SHM reader: read-only endpoint of the wengine-link snapshot bus.
//
// Attaches to a live publisher segment by name and pulls the latest clean
// frame (seqlock torn-read retry is inside slot_try_read; this class only
// surfaces kOk frames). Pull model for external apps: read_latest scans
// the ring for the highest frame_seq, poll_newer tracks one reader's
// cursor with exact missed-frame accounting, read_latest_frame adds
// decode. The GUI subscriber mode (gui-integration epic) will build on
// poll_newer + adapt_frame; the sample app builds on read_latest_frame.
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
  [[nodiscard]] bool poll_newer(std::uint64_t &last_seq, LinkFrame &out) const;

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
