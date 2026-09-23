// POSIX endpoint of the wengine-link SHM reader: read-only attach around
// the header-only protocol. Companions the publisher TU in the same lib.
// Windows port: replace this file with OpenFileMappingW/MapViewOfFile;
// the header above stays untouched.

#if defined(_WIN32)
#error "wengine-link SHM reader is POSIX-only (shm_open/mmap). Windows port uses OpenFileMapping/MapViewOfFile — see shm_reader.hpp."
#endif

#include "wengine/link/shm_reader.hpp"

#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <utility>

namespace wengine::link {

ShmReader::ShmReader(std::string_view name) {
  state_ = AttachState::kBadName;
  if (!valid_segment_name(name)) {
    status_ = {core::StatusCode::kInvalidArgument};
    return;
  }
  name_ = std::string(name);
  state_ = AttachState::kMissing;
  fd_ = ::shm_open(name_.c_str(), O_RDONLY, 0600);
  if (fd_ < 0) {
    last_errno_ = errno;
    status_ = {core::StatusCode::kInternal};
    if (last_errno_ != ENOENT) {
      state_ = AttachState::kError; // present but unopenable: fail fast
    }
    return;
  }
  mapping_size_ = sizeof(SegmentHeader) + kSlotCount * sizeof(Slot);
  struct stat st{};
  if (::fstat(fd_, &st) != 0) {
    last_errno_ = errno;
    status_ = {core::StatusCode::kInternal};
    state_ = AttachState::kError;
    cleanup();
    return;
  }
  // Opened but half-sized: publisher between shm_open and ftruncate.
  // Transient by construction — the caller retries.
  state_ = AttachState::kUnready;
  if (st.st_size != static_cast<off_t>(mapping_size_)) {
    status_ = {core::StatusCode::kInvalidArgument};
    cleanup();
    return;
  }
  void *base =
      ::mmap(nullptr, mapping_size_, PROT_READ, MAP_SHARED, fd_, 0);
  if (base == MAP_FAILED) {
    last_errno_ = errno;
    status_ = {core::StatusCode::kInternal};
    state_ = AttachState::kError;
    cleanup();
    return;
  }
  base_ = base;
  header_ = static_cast<const SegmentHeader *>(base_);
  slots_ = reinterpret_cast<const Slot *>(static_cast<const char *>(base_) +
                                          sizeof(SegmentHeader));
  // Commit word unset: publisher mapped but not yet initialized header.
  if (header_->version.load(std::memory_order_acquire) == 0) {
    status_ = {core::StatusCode::kInvalidArgument};
    cleanup();
    return;
  }
  report_ = inspect_header(*header_);
  if (!report_.status.ok()) {
    status_ = report_.status;
    state_ = AttachState::kRefused; // complete but incompatible: fail fast
    cleanup();
    return;
  }
  state_ = AttachState::kAttached;
}

ShmReader::~ShmReader() { cleanup(); }

ShmReader::ShmReader(ShmReader &&other) noexcept
    : name_(std::move(other.name_)),
      fd_(other.fd_),
      base_(other.base_),
      mapping_size_(other.mapping_size_),
      header_(other.header_),
      slots_(other.slots_),
      status_(other.status_),
      state_(other.state_),
      report_(other.report_),
      last_errno_(other.last_errno_) {
  other.fd_ = -1;
  other.base_ = nullptr;
  other.mapping_size_ = 0;
  other.header_ = nullptr;
  other.slots_ = nullptr;
  other.status_ = {core::StatusCode::kInternal}; // moved-from: inert
  other.state_ = AttachState::kError;
  other.report_ = HeaderReport{};
  other.report_.status = {core::StatusCode::kInternal};
  other.last_errno_ = 0;
}

ShmReader &ShmReader::operator=(ShmReader &&other) noexcept {
  if (this != &other) {
    cleanup();
    name_ = std::move(other.name_);
    fd_ = other.fd_;
    base_ = other.base_;
    mapping_size_ = other.mapping_size_;
    header_ = other.header_;
    slots_ = other.slots_;
    status_ = other.status_;
    state_ = other.state_;
    report_ = other.report_;
    last_errno_ = other.last_errno_;
    other.fd_ = -1;
    other.base_ = nullptr;
    other.mapping_size_ = 0;
    other.header_ = nullptr;
    other.slots_ = nullptr;
    other.status_ = {core::StatusCode::kInternal};
    other.state_ = AttachState::kError;
    other.report_ = HeaderReport{};
    other.report_.status = {core::StatusCode::kInternal};
    other.last_errno_ = 0;
  }
  return *this;
}

std::uint64_t ShmReader::published() const noexcept {
  if (header_ == nullptr) {
    return 0;
  }
  return header_published(*header_);
}

bool ShmReader::read_latest(LinkFrame &out) const {
  if (slots_ == nullptr) {
    return false;
  }
  // Two-pass scan: peek frame_seq words first (cheap acquire loads, no
  // copies), then a single guarded copy of the newest slot. Copying every
  // clean slot would move up to kSlotCount full frames per poll (~0.5 MB);
  // this moves one. NOTE: the seqlock seq words are per-slot write parity,
  // not ordering — only frame_seq orders frames across slots. The peek is
  // verified (seq unchanged across it) and the copy is fully guarded by
  // slot_try_read; a writer recycling mid-read just costs a bounded rescan
  // — never corrupt data, never a misreported frame.
  for (int attempt = 0; attempt < 4; ++attempt) {
    std::uint64_t best = 0;
    std::size_t index = 0;
    bool found = false;
    for (std::size_t i = 0; i < kSlotCount; ++i) {
      const std::uint64_t before = slots_[i].seq.load(std::memory_order_acquire);
      if (before == std::uint64_t{0} ||
          (before & std::uint64_t{1}) != std::uint64_t{0}) {
        continue; // never written, or writer active
      }
      const std::uint64_t candidate = slots_[i].frame.frame_seq;
      const std::uint64_t after = slots_[i].seq.load(std::memory_order_acquire);
      if (before != after || candidate == std::uint64_t{0}) {
        continue; // recycled mid-peek: skip this pass
      }
      if (!found || candidate > best) {
        best = candidate;
        index = i;
        found = true;
      }
    }
    if (!found) {
      return false;
    }
    LinkFrame got{};
    if (slot_try_read(slots_[index], got) == SlotRead::kOk) {
      out = got;
      return true;
    }
  }
  return false;
}

bool ShmReader::read_latest_frame(Frame &out) const {
  LinkFrame wire{};
  if (!read_latest(wire)) {
    return false;
  }
  return decode_frame(wire, out).ok();
}

bool ShmReader::poll_newer(std::uint64_t &last_seq, LinkFrame &out) const {
  LinkFrame wire{};
  if (!read_latest(wire) || wire.frame_seq <= last_seq) {
    return false;
  }
  last_seq = wire.frame_seq;
  out = wire;
  return true;
}

void ShmReader::cleanup() noexcept {
  // Reader never unlinks: the publisher owns the segment name.
  if (base_ != nullptr) {
    ::munmap(const_cast<void *>(base_), mapping_size_);
    base_ = nullptr;
  }
  mapping_size_ = 0;
  header_ = nullptr;
  slots_ = nullptr;
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

} // namespace wengine::link
