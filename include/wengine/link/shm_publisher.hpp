/**
 * @file
 * SHM publisher: engine-side endpoint of the wengine-link snapshot bus (class A latest-state viewing).
 * @note An emit::ISink publishing every emitted snapshot into a POSIX shared-memory seqlock
 *       ring (link/protocol.hpp layout) for realtime local consumers. Built and owned by the
 *       caller (CLI/GUI edge) and attached like any sink; sinks are NOT part of RunConfig.
 *       Publish path performs no syscalls and no I/O (adapt, encode into a member staging
 *       frame, seqlock copy); failures are sticky core::Status, never throws. Over-bound
 *       frames are dropped loudly and counted, never silently truncated. Header stays
 *       platform-free; the POSIX TU is src/link/shm_publisher.cpp under wengine_link_shm —
 *       a Windows port replaces that TU, never this header. Consumers: GUI subscriber, sample readers.
 */
#pragma once

// SHM publisher: engine-side endpoint of the wengine-link snapshot bus.
//
// An emit::ISink that publishes every emitted snapshot into a POSIX
// shared-memory seqlock ring (link/protocol.hpp layout) for realtime local
// consumers (GUI subscriber, sample readers — reader task). Built and owned
// by the caller (CLI/GUI edge) and attached like any sink: Runner::add_sink
// for interactive runs, run_headless(cfg, sinks, state) for headless batches
// (acceptance path), SimController::add_sink for GUI runs (re-attached on
// every rebuild automatically). Sinks are NOT part of RunConfig.
//
// LIFECYCLE (RAII, truncate semantics like CsvSink):
//   Construction creates a FRESH segment: any stale segment under the same
//   name is unlinked first, then O_CREAT|O_EXCL. Destruction unmaps, closes,
//   and unlinks — the segment exists exactly while the publisher lives, so
//   readers attach during the run. Name rules (POSIX): "/name", single
//   leading slash, non-empty. Domain is the solver/domain tag recorded in
//   the segment header (drives reader adapter choice); the per-snapshot
//   mapping itself is the FrameAdapter parameter (default: particle
//   adapter), so future solvers publish through the same class with their
//   own adapter + domain — no subclassing, no engine change.
//
// PUBLISH PATH (acceptance: "without slowing the loop"):
//   on_snapshot performs NO syscalls and NO I/O: adapt (edge-owned logic,
//   may allocate like take_snapshot does) -> encode into a member staging
//   frame -> seqlock copy into the ring slot -> publish counter. Bounded,
//   non-blocking, drop-oldest; a slow or absent reader never stalls the
//   sim or sibling sinks. Failures are sticky core::Status (never throws
//   out of on_snapshot): adapter throw -> kInternal; over-bound frame ->
//   kOutOfRange, counted in dropped_over_bound, frame skipped (loud, never
//   silently truncated); open failures (bad name/domain, shm_open/
//   ftruncate/mmap errno in last_errno()) leave an inert !ok() publisher.
//
// COUNTERS: stats().published == header publish_count (frames on the bus);
// stats().dropped_over_bound counts refused frames; see
// link/output_report.hpp for the full per-cause vocabulary (attempted,
// over-bound, queue-full, backpressure, pending-blocked, pending,
// abandoned-at-close, disabled-after-error). Readers derive exact
// loss from per-frame frame_seq gaps (reader_missed). Counts are local
// acceptance only; remote receipt needs a receiver LagStats.
//
// PORTABILITY: POSIX-only TU (shm_open/ftruncate/mmap, src/link/
// shm_publisher.cpp) — the one justified exception to the header-only
// engine rule. Windows port replaces that TU with CreateFileMapping/
// MapViewOfFile; this header is already platform-free. Not copyable (owns
// fd + mapping); movable.
//
// THREADING: single-writer model — call on_snapshot from one runner thread
// only (same constraint as every ISink driven by Runner).

#include "wengine/core/status.hpp"
#include "wengine/emit/sink.hpp"
#include "wengine/link/frame.hpp"
#include "wengine/link/protocol.hpp"
#include "wengine/world/state.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace wengine::link {

/**
 * @brief SHM seqlock publisher: copy-and-return, drop-oldest on full, non-throwing.
 * @note RAII: construction creates a FRESH segment (stale same-name segments unlinked first);
 *       destruction unmaps, closes, and unlinks — the segment exists exactly while the
 *       publisher lives. Not copyable (owns fd + mapping); movable. Single-writer model:
 *       call on_snapshot from one runner thread only.
 */
class ShmPublisher : public emit::ISink {
 public:
/**
 * @brief Create a fresh segment and publish into it.
 * @param[in] name POSIX segment name ("/name": single leading slash, non-empty; bad shapes leave an inert !ok() publisher).
 * @param[in] domain solver/domain tag recorded in the segment header (drives reader adapter choice).
 * @param[in] adapter per-snapshot mapping (default: particle adapter; future solvers publish through the same class with their own adapter + domain — no subclassing, no engine change).
 * @note Open failures (bad name/domain, shm_open/ftruncate/mmap errno in last_errno()) leave an inert !ok() publisher.
 */
  explicit ShmPublisher(std::string_view name, std::string_view domain,
                        FrameAdapter adapter = &frame_from_snapshot);
/** @brief Unmap, close, and unlink the segment (readers never unlink: the publisher owns the segment lifetime). */
  ~ShmPublisher() override;

/** @brief Non-copyable (owns fd + mapping). */
  ShmPublisher(const ShmPublisher &) = delete;
/** @brief Non-copyable (owns fd + mapping). */
  ShmPublisher &operator=(const ShmPublisher &) = delete;
/** @brief Move-construct. @param[in] other Source; left inert. */
  ShmPublisher(ShmPublisher &&other) noexcept;
/** @brief Move-assign. @param[in] other Source; left inert. */
  ShmPublisher &operator=(ShmPublisher &&other) noexcept;

/**
 * @brief Publish one emitted snapshot (Runner fan-out; synchronous, non-blocking).
 * @param[in] snapshot Runner-owned emission buffer (observed synchronously; the publisher copies out before returning and never retains the reference).
 * @note No syscalls, no I/O, no throws: adapt (edge-owned, may allocate like take_snapshot) -> encode
 *       into the member staging frame -> seqlock copy -> publish counter. Adapter throw is sticky
 *       kInternal; over-bound frame is sticky kOutOfRange counted in dropped_over_bound (frame skipped).
 *       A slow or absent reader never stalls the sim or sibling sinks (writer drops oldest on full).
 */
  void on_snapshot(const world::Snapshot &snapshot) override;

/** @brief Success predicate. @return True while no sticky failure latched. */
  [[nodiscard]] bool ok() const noexcept { return status_.ok(); }
/** @brief Latched sticky status. @return kOk, or the first sticky failure (kInternal/kOutOfRange). */
  [[nodiscard]] core::Status status() const noexcept { return status_; }
  // errno captured at the failing open syscall (0 when ok).
/** @brief Errno captured at the failing open syscall (0 when ok). @return Captured errno value. */
  [[nodiscard]] int last_errno() const noexcept { return last_errno_; }
/** @brief Monotonic publisher counters (stats().published == header publish_count; counts are local acceptance only — remote receipt needs a receiver LagStats). @return Counter snapshot. */
  [[nodiscard]] const PublisherStats &stats() const noexcept { return stats_; }
  // Live segment header, or nullptr while !ok() (never copy it: atomics).
/** @brief Live segment header, or nullptr while !ok() (never copy it: atomics). @return Header pointer or null. */
  [[nodiscard]] const SegmentHeader *header() const noexcept { return header_; }
/** @brief Configured segment name. @return Segment name. */
  [[nodiscard]] const std::string &name() const noexcept { return name_; }

 private:
  void cleanup() noexcept;

  std::string name_;
  FrameAdapter adapter_ = &frame_from_snapshot;
  int fd_ = -1;
  void *base_ = nullptr;
  std::size_t mapping_size_ = 0;
  SegmentHeader *header_ = nullptr;
  Slot *slots_ = nullptr;
  LinkFrame staging_{}; // encode target: no per-emit stack bloat, no alloc
  PublisherStats stats_{};
  core::Status status_{};
  int last_errno_ = 0;
};

} // namespace wengine::link
