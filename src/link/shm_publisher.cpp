// POSIX endpoint of the wengine-link SHM publisher: segment create/map/
// destroy around the header-only protocol. This TU is the one justified
// exception to the header-only engine rule (platform syscalls cannot live
// in headers). Publish path (on_snapshot) performs zero syscalls.
// Windows port: replace this file with CreateFileMappingW/MapViewOfFile;
// the header above stays untouched.

#if defined(_WIN32)
#error "wengine-link SHM publisher is POSIX-only (shm_open/mmap). Windows port uses CreateFileMapping/MapViewOfFile — see shm_publisher.hpp."
#endif

#include "wengine/link/shm_publisher.hpp"

#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <new>
#include <utility>

namespace wengine::link {

ShmPublisher::ShmPublisher(std::string_view name, std::string_view domain,
                           FrameAdapter adapter)
    : adapter_(adapter) {
  if (adapter_ == nullptr) {
    status_ = {core::StatusCode::kInvalidArgument};
    return;
  }
  if (!valid_segment_name(name)) {
    status_ = {core::StatusCode::kInvalidArgument};
    return;
  }
  name_ = std::string(name);
  // Truncate semantics (like CsvSink): stale runs never poison a new one.
  ::shm_unlink(name_.c_str());
  fd_ = ::shm_open(name_.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
  if (fd_ < 0) {
    last_errno_ = errno;
    status_ = {core::StatusCode::kInternal};
    return;
  }
  static_assert(sizeof(SegmentHeader) % alignof(Slot) == 0);
  mapping_size_ = sizeof(SegmentHeader) + kSlotCount * sizeof(Slot);
  if (::ftruncate(fd_, static_cast<off_t>(mapping_size_)) != 0) {
    last_errno_ = errno;
    status_ = {core::StatusCode::kInternal};
    cleanup();
    return;
  }
  base_ = ::mmap(nullptr, mapping_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
  if (base_ == MAP_FAILED) {
    base_ = nullptr;
    last_errno_ = errno;
    status_ = {core::StatusCode::kInternal};
    cleanup();
    return;
  }
  header_ = new (base_) SegmentHeader();
  slots_ = reinterpret_cast<Slot *>(static_cast<char *>(base_) + sizeof(SegmentHeader));
  for (std::size_t i = 0; i < kSlotCount; ++i) {
    new (&slots_[i]) Slot();
  }
  if (!init_header(*header_, domain).ok()) {
    status_ = {core::StatusCode::kInvalidArgument};
    cleanup();
  }
}

ShmPublisher::~ShmPublisher() { cleanup(); }

ShmPublisher::ShmPublisher(ShmPublisher &&other) noexcept
    : name_(std::move(other.name_)),
      adapter_(other.adapter_),
      fd_(other.fd_),
      base_(other.base_),
      mapping_size_(other.mapping_size_),
      header_(other.header_),
      slots_(other.slots_),
      stats_(other.stats_),
      status_(other.status_),
      last_errno_(other.last_errno_) {
  other.fd_ = -1;
  other.base_ = nullptr;
  other.mapping_size_ = 0;
  other.header_ = nullptr;
  other.slots_ = nullptr;
  other.stats_ = PublisherStats{};
  other.status_ = {core::StatusCode::kInternal}; // moved-from: inert
  other.last_errno_ = 0;
}

ShmPublisher &ShmPublisher::operator=(ShmPublisher &&other) noexcept {
  if (this != &other) {
    cleanup();
    name_ = std::move(other.name_);
    adapter_ = other.adapter_;
    fd_ = other.fd_;
    base_ = other.base_;
    mapping_size_ = other.mapping_size_;
    header_ = other.header_;
    slots_ = other.slots_;
    stats_ = other.stats_;
    status_ = other.status_;
    last_errno_ = other.last_errno_;
    other.fd_ = -1;
    other.base_ = nullptr;
    other.mapping_size_ = 0;
    other.header_ = nullptr;
    other.slots_ = nullptr;
    other.stats_ = PublisherStats{};
    other.status_ = {core::StatusCode::kInternal};
    other.last_errno_ = 0;
  }
  return *this;
}

void ShmPublisher::on_snapshot(const world::Snapshot &snapshot) {
  if (!status_.ok()) {
    return;
  }
  Frame frame;
  try {
    frame = adapter_(snapshot);
  } catch (...) {
    // Best-effort observer: sticky failure, never break the sim loop.
    status_ = {core::StatusCode::kInternal};
    return;
  }
  const std::uint64_t seq = stats_.published + 1;
  if (!encode_frame(frame, staging_, seq).ok()) {
    ++stats_.dropped_over_bound;
    status_ = {core::StatusCode::kOutOfRange}; // loud (S7), frame skipped
    return;
  }
  Slot &slot = slots_[static_cast<std::size_t>(
      ring_slot(stats_.published, static_cast<std::uint64_t>(kSlotCount)))];
  slot_begin_write(slot);
  slot_store_frame(slot, staging_);
  slot_end_write(slot);
  header_note_publish(*header_);
  ++stats_.published;
}

void ShmPublisher::cleanup() noexcept {
  if (base_ != nullptr) {
    ::munmap(base_, mapping_size_);
    base_ = nullptr;
  }
  mapping_size_ = 0;
  header_ = nullptr;
  slots_ = nullptr;
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  if (!name_.empty()) {
    ::shm_unlink(name_.c_str());
  }
}

} // namespace wengine::link
