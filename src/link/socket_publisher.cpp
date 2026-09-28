// POSIX endpoints of the wengine-link socket publishers: DNS/connect/send
// around the platform-free framing in socket_publisher.hpp. This TU is the
// same justified exception to the header-only engine rule as the SHM
// endpoint TUs (platform syscalls cannot live in headers). Publish paths
// (on_snapshot, service_pending) perform zero blocking calls: the sockets
// are non-blocking from construction, sends loop only while the kernel
// accepts bytes, and EAGAIN drops the attempted frame loudly (dropped_send)
// without stalling the sim loop or poisoning the publisher. The bounded
// blocking drain (TcpPublisher::finish) is application-edge only — never
// the hot path, never the destructor. Windows port: replace this file
// with Winsock2 (WSAStartup, getaddrinfo, ioctlsocket, WSAPoll); the header
// above stays untouched.

#if defined(_WIN32)
#error "wengine-link socket publishers are POSIX-only. Windows port uses Winsock2 — see socket_publisher.hpp."
#endif

#include "wengine/link/socket_publisher.hpp"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <string>
#include <utility>

namespace wengine::link {

namespace {

// Edge-only (construction) connect timeout: bounds a hanging connect to a
// silent remote without ever touching the hot publish path.
inline constexpr int kConnectTimeoutMs = 3000;

bool set_nonblocking(int fd) noexcept {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return false;
  }
  return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool set_cloexec(int fd) noexcept {
  const int flags = ::fcntl(fd, F_GETFD, 0);
  if (flags < 0) {
    return false;
  }
  return ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

// Wait for a non-blocking connect to finish; false on timeout or refusal
// (errno set: ETIMEDOUT or the peer's verdict).
bool wait_connected(int fd, int timeout_ms) noexcept {
  fd_set wfds{};
  FD_ZERO(&wfds);
  FD_SET(fd, &wfds);
  timeval tv{};
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  const int rc = ::select(fd + 1, nullptr, &wfds, nullptr, &tv);
  if (rc == 0) {
    errno = ETIMEDOUT;
    return false;
  }
  if (rc < 0) {
    return false;
  }
  int err = 0;
  socklen_t len = sizeof(err);
  if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0) {
    return false;
  }
  if (err != 0) {
    errno = err;
    return false;
  }
  return true;
}

enum class SendResult : std::uint8_t { kComplete, kBackpressure, kDead };

// Non-blocking send-all: loops while the kernel accepts bytes (partial
// writes included), stops at the first EAGAIN (transient backpressure) or
// hard error (dead peer). err carries errno on kBackpressure/kDead.
SendResult send_all(int fd, const unsigned char *buf, std::size_t len,
                    std::size_t &sent, int &err) noexcept {
  sent = 0;
  while (sent < len) {
    const ssize_t n =
        ::send(fd, buf + sent, len - sent, MSG_NOSIGNAL); // no SIGPIPE: sticky Status instead
    if (n < 0) {
      err = errno;
      if (err == EINTR) {
        continue;
      }
      if (err == EAGAIN || err == EWOULDBLOCK) {
        return SendResult::kBackpressure;
      }
      return SendResult::kDead;
    }
    if (n == 0) {
      err = EPIPE;
      return SendResult::kDead;
    }
    sent += static_cast<std::size_t>(n);
  }
  err = 0;
  return SendResult::kComplete;
}

// Datagram sends are atomic: a partial result is a transport failure, never
// a second datagram containing the remainder of the frame.
SendResult send_datagram(int fd, const unsigned char *buf, std::size_t len,
                         int &err) noexcept {
  for (;;) {
    const ssize_t n = ::send(fd, buf, len, MSG_NOSIGNAL);
    if (n < 0) {
      err = errno;
      if (err == EINTR) {
        continue;
      }
      if (err == EAGAIN || err == EWOULDBLOCK) {
        return SendResult::kBackpressure;
      }
      return SendResult::kDead;
    }
    if (static_cast<std::size_t>(n) != len) {
      err = EIO;
      return SendResult::kDead;
    }
    err = 0;
    return SendResult::kComplete;
  }
}

// Resolve + non-blocking connect (TCP: waits up to timeout_ms; UDP: connect
// only fixes the peer, returns immediately). Tries every addrinfo result in
// order; -1 with err_out set when nothing connects.
int open_connected(const HostPort &hp, int kind, int timeout_ms, int &err_out) noexcept {
  const std::string port = std::to_string(hp.port);
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = kind;
  addrinfo *list = nullptr;
  if (::getaddrinfo(hp.host.c_str(), port.c_str(), &hints, &list) != 0 ||
      list == nullptr) {
    err_out = 0; // resolution failure: no syscall errno (kInternal class)
    return -1;
  }
  int fd = -1;
  err_out = ECONNREFUSED;
  for (addrinfo *ai = list; ai != nullptr; ai = ai->ai_next) {
    fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) {
      err_out = errno;
      continue;
    }
    set_cloexec(fd); // best-effort: a leaked fd on fork/exec is harmless here
    if (!set_nonblocking(fd)) {
      err_out = errno;
      ::close(fd);
      fd = -1;
      continue;
    }
    if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
      break; // instant (loopback TCP, all UDP)
    }
    if (errno != EINPROGRESS) {
      err_out = errno;
      ::close(fd);
      fd = -1;
      continue;
    }
    if (kind == SOCK_STREAM && !wait_connected(fd, timeout_ms)) {
      err_out = errno;
      ::close(fd);
      fd = -1;
      continue;
    }
    break; // UDP with EINPROGRESS: no handshake to wait for
  }
  ::freeaddrinfo(list);
  if (fd < 0) {
    return -1;
  }
  err_out = 0;
  return fd;
}

void tune_send_buffer(int fd) noexcept {
  int sndbuf = kSocketSendBuffer;
  ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)); // best-effort
}

// Publisher-run identity for v2 records: process/time mixed via splitmix64.
// Opaque to the codec (any u64 goes); distinct constructions must differ so
// receivers can reset cursors on restart (PHYSI-40 session semantics).
// Best-effort only — collisions degrade to one extra missed-span report,
// never to corrupt data.
std::uint64_t make_run_id() noexcept {
  timespec now{};
  (void)::clock_gettime(CLOCK_MONOTONIC, &now);
  std::uint64_t z = static_cast<std::uint64_t>(::getpid()) * 0x9E3779B97F4A7C15ULL +
                    static_cast<std::uint64_t>(now.tv_sec) * 1000000000ULL +
                    static_cast<std::uint64_t>(now.tv_nsec);
  z += 0x9E3779B97F4A7C15ULL;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

} // namespace

TcpPublisher::TcpPublisher(std::string_view endpoint, std::string_view domain,
                           FrameAdapter adapter)
    : adapter_(adapter) {
  if (adapter_ == nullptr) {
    status_ = {core::StatusCode::kInvalidArgument};
    return;
  }
  const auto ep = parse_endpoint(endpoint);
  if (!ep.ok()) {
    status_ = {core::StatusCode::kInvalidArgument};
    return;
  }
  if (domain.empty() || domain.size() >= kDomainSize) {
    status_ = {core::StatusCode::kInvalidArgument};
    return;
  }
  endpoint_ = std::string(endpoint);
  domain_ = std::string(domain);
  run_id_ = make_run_id();
  int err = 0;
  fd_ = open_connected(ep.value, SOCK_STREAM, kConnectTimeoutMs, err);
  if (fd_ < 0) {
    last_errno_ = err;
    status_ = {core::StatusCode::kInternal};
    return;
  }
  tune_send_buffer(fd_);
  TcpHello hello_msg{};
  hello_msg.magic = kTcpMagic;
  hello_msg.version = kTcpVersion;
  for (std::size_t i = 0; i < domain.size(); ++i) {
    hello_msg.domain[i] = domain[i];
  }
  unsigned char hello[sizeof(hello_msg)]{};
  std::memcpy(hello, &hello_msg, sizeof(hello_msg));
  std::size_t sent = 0;
  const SendResult r = send_all(fd_, hello, sizeof(hello), sent, err);
  if (r != SendResult::kComplete) {
    last_errno_ = err;
    status_ = {core::StatusCode::kInternal}; // cannot even shake hands
    cleanup();
  }
}

TcpPublisher::~TcpPublisher() { cleanup(); }

TcpPublisher::TcpPublisher(TcpPublisher &&other) noexcept
    : endpoint_(std::move(other.endpoint_)),
      domain_(std::move(other.domain_)),
      adapter_(other.adapter_),
      run_id_(other.run_id_),
      fd_(other.fd_),
      wire_(other.wire_),
      pending_offset_(other.pending_offset_),
      pending_size_(other.pending_size_),
      frame_seq_(other.frame_seq_),
      stats_(other.stats_),
      status_(other.status_),
      last_errno_(other.last_errno_) {
  other.fd_ = -1;
  other.pending_offset_ = 0;
  other.pending_size_ = 0;
  other.frame_seq_ = 0;
  other.run_id_ = 0;
  other.stats_ = PublisherStats{};
  other.status_ = {core::StatusCode::kInternal}; // moved-from: inert
  other.last_errno_ = 0;
}

TcpPublisher &TcpPublisher::operator=(TcpPublisher &&other) noexcept {
  if (this != &other) {
    cleanup();
    endpoint_ = std::move(other.endpoint_);
    domain_ = std::move(other.domain_);
    adapter_ = other.adapter_;
    run_id_ = other.run_id_;
    fd_ = other.fd_;
    wire_ = other.wire_;
    pending_offset_ = other.pending_offset_;
    pending_size_ = other.pending_size_;
    frame_seq_ = other.frame_seq_;
    stats_ = other.stats_;
    status_ = other.status_;
    last_errno_ = other.last_errno_;
    other.fd_ = -1;
    other.pending_offset_ = 0;
    other.pending_size_ = 0;
    other.frame_seq_ = 0;
    other.run_id_ = 0;
    other.stats_ = PublisherStats{};
    other.status_ = {core::StatusCode::kInternal};
    other.last_errno_ = 0;
  }
  return *this;
}

void TcpPublisher::on_snapshot(const world::Snapshot &snapshot) {
  ++stats_.attempted;
  if (!status_.ok()) {
    ++stats_.disabled_after_error;
    return;
  }

  // TCP cannot discard a frame after some bytes have entered the stream:
  // doing so would make every later length prefix unparseable. Finish a
  // partial frame first; while it remains backpressured, drop this new
  // snapshot and preserve stream framing. The drain itself is
  // service_pending() so idle periods make identical progress without a
  // dummy snapshot.
  if (pending_size_ != 0) {
    if (!service_pending()) {
      if (!status_.ok()) {
        ++stats_.disabled_after_error;
        return; // dead peer: sticky, set by service_pending()
      }
      ++stats_.dropped_pending_blocked;
      ++frame_seq_;
      return;
    }
  }

  Frame frame;
  try {
    frame = adapter_(snapshot);
  } catch (...) {
    // Best-effort observer: sticky failure, never break the sim loop.
    status_ = {core::StatusCode::kInternal};
    ++stats_.disabled_after_error;
    return;
  }
  // frame_seq counts attempts (not just sends) so readers account publisher
  // drops and transport loss together via LagStats gaps.
  const std::uint64_t seq = ++frame_seq_;
  // Compact record under the same length prefix as always: used entries
  // only, explicit portable bytes. The compact bound verdict matches the
  // SHM LinkFrame bound verdict, so over-bound refusal stays identical
  // across every transport.
  std::size_t rec_len = 0;
  if (!compact::encode_record(frame, domain_, run_id_, seq, wire_.data() + 4,
                              wire_.size() - 4, rec_len)
           .ok()) {
    ++stats_.dropped_over_bound;
    status_ = {core::StatusCode::kOutOfRange}; // loud (S7), frame skipped
    return;
  }
  socket_detail::put_u32_le(wire_.data(), static_cast<std::uint32_t>(rec_len));
  const std::size_t wire_len = 4 + rec_len;
  int err = 0;
  std::size_t sent = 0;
  const SendResult r = send_all(fd_, wire_.data(), wire_len, sent, err);
  if (r == SendResult::kComplete) {
    ++stats_.published;
    stats_.bytes_accepted += static_cast<std::uint64_t>(wire_len);
    return;
  }
  if (r == SendResult::kBackpressure) {
    if (sent == 0) {
      ++stats_.dropped_send; // transient: counted, never sticky
    } else {
      pending_offset_ = sent;
      pending_size_ = wire_len;
    }
    return;
  }
  last_errno_ = err;
  status_ = {core::StatusCode::kInternal}; // dead peer: sticky, like SHM teardown
  ++stats_.disabled_after_error;
}

bool TcpPublisher::service_pending() noexcept {
  if (!status_.ok()) {
    return false;
  }
  if (pending_size_ == 0) {
    return true;
  }
  if (fd_ < 0) {
    last_errno_ = EBADF;
    status_ = {core::StatusCode::kInternal};
    return false;
  }
  std::size_t sent = 0;
  int err = 0;
  const SendResult r =
      send_all(fd_, wire_.data() + pending_offset_, pending_size_ - pending_offset_, sent, err);
  pending_offset_ += sent;
  if (r == SendResult::kComplete) {
    ++stats_.published;
    stats_.bytes_accepted += static_cast<std::uint64_t>(pending_size_);
    pending_size_ = 0;
    pending_offset_ = 0;
    return true;
  }
  if (r == SendResult::kDead) {
    last_errno_ = err;
    status_ = {core::StatusCode::kInternal};
    return false;
  }
  return false; // transient backpressure: pending preserved, no drop counted
}

core::Status TcpPublisher::finish(int timeout_ms) noexcept {
  if (!status_.ok()) {
    return status_;
  }
  if (pending_size_ == 0) {
    return core::Status::success();
  }
  if (timeout_ms < 0) {
    return {core::StatusCode::kInvalidArgument};
  }
  if (fd_ < 0) {
    last_errno_ = EBADF;
    status_ = {core::StatusCode::kInternal};
    return status_;
  }
  timespec start{};
  if (::clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
    last_errno_ = errno;
    status_ = {core::StatusCode::kInternal};
    return status_;
  }
  const std::int64_t deadline_ns = static_cast<std::int64_t>(start.tv_sec) * 1000000000LL +
                                   static_cast<std::int64_t>(start.tv_nsec) +
                                   static_cast<std::int64_t>(timeout_ms) * 1000000LL;
  for (;;) {
    if (service_pending()) {
      return core::Status::success();
    }
    if (!status_.ok()) {
      return status_; // dead peer: sticky, pending preserved for reporting
    }
    timespec now{};
    if (::clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
      last_errno_ = errno;
      status_ = {core::StatusCode::kInternal};
      return status_;
    }
    const std::int64_t now_ns = static_cast<std::int64_t>(now.tv_sec) * 1000000000LL +
                                static_cast<std::int64_t>(now.tv_nsec);
    const std::int64_t remain_ns = deadline_ns - now_ns;
    if (remain_ns <= 0) {
      return {core::StatusCode::kInternal}; // deadline: pending kept, not sticky
    }
    fd_set wfds{};
    FD_ZERO(&wfds);
    FD_SET(fd_, &wfds);
    timeval tv{};
    tv.tv_sec = static_cast<decltype(tv.tv_sec)>(remain_ns / 1000000000LL);
    tv.tv_usec = static_cast<decltype(tv.tv_usec)>((remain_ns % 1000000000LL) / 1000LL);
    const int rc = ::select(fd_ + 1, nullptr, &wfds, nullptr, &tv);
    if (rc == 0) {
      return {core::StatusCode::kInternal}; // select timeout: same as deadline
    }
    if (rc < 0) {
      const int e = errno;
      if (e == EINTR) {
        continue;
      }
      last_errno_ = e;
      status_ = {core::StatusCode::kInternal};
      return status_;
    }
  }
}

void TcpPublisher::cleanup() noexcept {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

UdpPublisher::UdpPublisher(std::string_view endpoint, std::string_view domain,
                           FrameAdapter adapter)
    : adapter_(adapter) {
  if (adapter_ == nullptr) {
    status_ = {core::StatusCode::kInvalidArgument};
    return;
  }
  const auto ep = parse_endpoint(endpoint);
  if (!ep.ok()) {
    status_ = {core::StatusCode::kInvalidArgument};
    return;
  }
  if (domain.empty() || domain.size() >= kDomainSize) {
    status_ = {core::StatusCode::kInvalidArgument};
    return;
  }
  endpoint_ = std::string(endpoint);
  domain_ = std::string(domain);
  run_id_ = make_run_id();
  int err = 0;
  fd_ = open_connected(ep.value, SOCK_DGRAM, 0, err);
  if (fd_ < 0) {
    last_errno_ = err;
    status_ = {core::StatusCode::kInternal};
    return;
  }
  tune_send_buffer(fd_);
}

UdpPublisher::~UdpPublisher() { cleanup(); }

UdpPublisher::UdpPublisher(UdpPublisher &&other) noexcept
    : endpoint_(std::move(other.endpoint_)),
      domain_(std::move(other.domain_)),
      adapter_(other.adapter_),
      run_id_(other.run_id_),
      fd_(other.fd_),
      frame_seq_(other.frame_seq_),
      stats_(other.stats_),
      status_(other.status_),
      last_errno_(other.last_errno_) {
  other.fd_ = -1;
  other.frame_seq_ = 0;
  other.run_id_ = 0;
  other.stats_ = PublisherStats{};
  other.status_ = {core::StatusCode::kInternal}; // moved-from: inert
  other.last_errno_ = 0;
}

UdpPublisher &UdpPublisher::operator=(UdpPublisher &&other) noexcept {
  if (this != &other) {
    cleanup();
    endpoint_ = std::move(other.endpoint_);
    domain_ = std::move(other.domain_);
    adapter_ = other.adapter_;
    run_id_ = other.run_id_;
    fd_ = other.fd_;
    frame_seq_ = other.frame_seq_;
    stats_ = other.stats_;
    status_ = other.status_;
    last_errno_ = other.last_errno_;
    other.fd_ = -1;
    other.frame_seq_ = 0;
    other.run_id_ = 0;
    other.stats_ = PublisherStats{};
    other.status_ = {core::StatusCode::kInternal};
    other.last_errno_ = 0;
  }
  return *this;
}

void UdpPublisher::on_snapshot(const world::Snapshot &snapshot) {
  ++stats_.attempted;
  if (!status_.ok()) {
    ++stats_.disabled_after_error;
    return;
  }
  Frame frame;
  try {
    frame = adapter_(snapshot);
  } catch (...) {
    // Best-effort observer: sticky failure, never break the sim loop.
    status_ = {core::StatusCode::kInternal};
    ++stats_.disabled_after_error;
    return;
  }
  const std::uint64_t seq = ++frame_seq_;
  // One shared verdict for every transport: the compact bound (counts,
  // names, datagram fit) refuses over-bound frames identically on
  // TCP/SHM/UDP alike — loud (S7), never silently truncated.
  std::size_t datagram_len = 0;
  if (!compact::encode_record(frame, domain_, run_id_, seq, datagram_.data(),
                              datagram_.size(), datagram_len)
           .ok()) {
    ++stats_.dropped_over_bound;
    status_ = {core::StatusCode::kOutOfRange}; // loud (S7), frame skipped
    return;
  }
  int err = 0;
  const SendResult r = send_datagram(fd_, datagram_.data(), datagram_len, err);
  if (r == SendResult::kComplete) {
    ++stats_.published;
    stats_.bytes_accepted += static_cast<std::uint64_t>(datagram_len);
    return;
  }
  if (r == SendResult::kBackpressure) {
    ++stats_.dropped_send; // transient: counted, never sticky
    return;
  }
  last_errno_ = err;
  status_ = {core::StatusCode::kInternal}; // socket dead: sticky
  ++stats_.disabled_after_error;
}

void UdpPublisher::cleanup() noexcept {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

} // namespace wengine::link
