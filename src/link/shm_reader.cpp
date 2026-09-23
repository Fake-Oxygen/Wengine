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

namespace {

[[nodiscard]] bool valid_segment_name(std::string_view name) noexcept {
  if (name.size() < 2 || name.front() != '/') {
    return false;
  }
  return name.find('/', 1) == std::string_view::npos;
}

} // namespace

ShmReader::ShmReader(std::string_view name) {
  if (!valid_segment_name(name)) {
    status_ = {core::StatusCode::kInvalidArgument};
    return;
  }
  name_ = std::string(name);
  fd_ = ::shm_open(name_.c_str(), O_RDONLY, 0600);
  if (fd_ < 0) {
    last_errno_ = errno;
    status_ = {core::StatusCode::kInternal};
    return;
  }
  mapping_size_ = sizeof(SegmentHeader) + kSlotCount * sizeof(Slot);
  struct stat st{};
  if (::fstat(fd_, &st) != 0) {
    last_errno_ = errno;
    status_ = {core::StatusCode::kInternal};
    cleanup();
    return;
  }
  // Half-constructed publisher segment (exists, not yet sized/inited):
  // loud reject, the caller retries after the publisher is ready.
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
    cleanup();
    return;
  }
  base_ = base;
  header_ = static_cast<const SegmentHeader *>(base_);
  slots_ = reinterpret_cast<const Slot *>(static_cast<const char *>(base_) +
                                          sizeof(SegmentHeader));
  if (!check_header(*header_).ok()) {
    status_ = {core::StatusCode::kInvalidArgument};
    cleanup();
  }
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
      last_errno_(other.last_errno_) {
  other.fd_ = -1;
  other.base_ = nullptr;
  other.mapping_size_ = 0;
  other.header_ = nullptr;
  other.slots_ = nullptr;
  other.status_ = {core::StatusCode::kInternal}; // moved-from: inert
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
    last_errno_ = other.last_errno_;
    other.fd_ = -1;
    other.base_ = nullptr;
    other.mapping_size_ = 0;
    other.header_ = nullptr;
    other.slots_ = nullptr;
    other.status_ = {core::StatusCode::kInternal};
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
  bool found = false;
  std::uint64_t best = 0;
  for (std::size_t i = 0; i < kSlotCount; ++i) {
    LinkFrame got{};
    if (slot_try_read(slots_[i], got) != SlotRead::kOk) {
      continue;
    }
    if (!found || got.frame_seq > best) {
      best = got.frame_seq;
      out = got;
      found = true;
    }
  }
  return found;
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
