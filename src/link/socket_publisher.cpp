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

#include "socket_common.hpp"

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

// Shared POSIX connect/send/buffer/run-id mechanics live in the src-private
// socket_common.hpp so the PHYSI-46 worker reuses the exact same behavior.
using namespace socket_internal;

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
                           FrameAdapter adapter, std::size_t payload_budget)
    : adapter_(adapter), payload_budget_(payload_budget) {
  if (adapter_ == nullptr) {
    status_ = {core::StatusCode::kInvalidArgument};
    return;
  }
  if (payload_budget_ == 0 || payload_budget_ > kMaxUdpPayload) {
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
      payload_budget_(other.payload_budget_),
      last_over_bound_bytes_(other.last_over_bound_bytes_),
      frame_seq_(other.frame_seq_),
      stats_(other.stats_),
      status_(other.status_),
      last_errno_(other.last_errno_) {
  other.fd_ = -1;
  other.frame_seq_ = 0;
  other.run_id_ = 0;
  other.payload_budget_ = kDefaultUdpPayloadBudget;
  other.last_over_bound_bytes_ = 0;
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
    payload_budget_ = other.payload_budget_;
    last_over_bound_bytes_ = other.last_over_bound_bytes_;
    frame_seq_ = other.frame_seq_;
    stats_ = other.stats_;
    status_ = other.status_;
    last_errno_ = other.last_errno_;
    other.fd_ = -1;
    other.frame_seq_ = 0;
    other.run_id_ = 0;
    other.payload_budget_ = kDefaultUdpPayloadBudget;
    other.last_over_bound_bytes_ = 0;
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
  // One shared verdict for every transport, plus the UDP payload budget
  // (PHYSI-41): the encode capacity is the configured budget, so every
  // emitted datagram satisfies bytes <= budget, headers included (the
  // datagram IS the record). An over-budget record is indivisible here —
  // refused loudly (S7) with the needed size recorded, never truncated.
  std::size_t datagram_len = 0;
  if (!compact::encode_record(frame, domain_, run_id_, seq, datagram_.data(),
                              payload_budget_, datagram_len)
           .ok()) {
    last_over_bound_bytes_ = compact::encoded_size(frame);
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
