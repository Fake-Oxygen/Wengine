// POSIX TCP line client for the SIG-10 control plane (see
// include/wengine/link/control_client.hpp). Platform TU by the same
// justification as socket_publisher.cpp and control_server.cpp: syscalls
// live here, never in the header (a Windows port replaces this file with
// Winsock2).
//
// Unlike the publishers and the server, this client performs blocking I/O
// with timeouts: it only ever runs at the CLI edge (`sig list/add/...`),
// never on the sim thread, so blocking here cannot stall a run.

#if defined(_WIN32)
#error "wengine-link control client is POSIX-only. Windows port uses Winsock2 — see control_client.hpp."
#endif

#include "wengine/link/control_client.hpp"

#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <netdb.h>
#include <string>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <utility>

namespace wengine::link {

namespace {

// Wait for a non-blocking connect to finish; false on timeout or refusal
// (errno set: ETIMEDOUT or the peer's verdict). Same shape as the stream
// publishers' edge-only connect wait.
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

// Resolve + connect with a bounded wait. -1 with err_out set when nothing
// connects (resolution failures carry err_out == 0: no syscall errno
// exists for those — same convention as the stream publishers).
int open_client(const HostPort &hp, int timeout_ms, int &err_out) noexcept {
  const std::string port = std::to_string(hp.port);
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo *list = nullptr;
  if (::getaddrinfo(hp.host.c_str(), port.c_str(), &hints, &list) != 0 ||
      list == nullptr) {
    err_out = 0;
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
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
      (void)::fcntl(fd, F_SETFL, flags | O_NONBLOCK); // best-effort
    }
    const int clo = ::fcntl(fd, F_GETFD, 0);
    if (clo >= 0) {
      (void)::fcntl(fd, F_SETFD, clo | FD_CLOEXEC); // best-effort
    }
    if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
      break; // instant (loopback)
    }
    if (errno != EINPROGRESS) {
      err_out = errno;
      ::close(fd);
      fd = -1;
      continue;
    }
    if (!wait_connected(fd, timeout_ms)) {
      err_out = errno;
      ::close(fd);
      fd = -1;
      continue;
    }
    break;
  }
  ::freeaddrinfo(list);
  if (fd < 0) {
    return -1;
  }
  // Blocking I/O from here on (bounded by SO_RCVTIMEO reads): this client
  // only runs at the CLI edge, never on the sim thread. Non-blocking was
  // only needed for the bounded connect wait above — leaving it set would
  // make recv() fail with EAGAIN instead of waiting for the verdict.
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags >= 0) {
    (void)::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK); // best-effort
  }
  err_out = 0;
  return fd;
}

bool set_recv_timeout(int fd, int timeout_ms) noexcept {
  timeval tv{};
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  return ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0;
}

} // namespace

ControlClient::ControlClient(std::string_view endpoint, int timeout_ms)
    : endpoint_(endpoint.data(), endpoint.size()),
      timeout_ms_(timeout_ms < 0 ? 0 : timeout_ms) {
  const auto ep = parse_endpoint(endpoint);
  if (!ep.ok()) {
    status_ = {core::StatusCode::kInvalidArgument};
    return;
  }
  int err = 0;
  fd_ = open_client(ep.value, timeout_ms_, err);
  if (fd_ < 0) {
    status_ = {core::StatusCode::kInternal};
    last_errno_ = err;
    return;
  }
  if (!set_recv_timeout(fd_, timeout_ms_)) {
    last_errno_ = errno;
    cleanup();
    status_ = {core::StatusCode::kInternal};
    return;
  }
  status_ = core::Status::success();
}

ControlClient::~ControlClient() { cleanup(); }

ControlClient::ControlClient(ControlClient &&other) noexcept
    : endpoint_(std::move(other.endpoint_)), timeout_ms_(other.timeout_ms_),
      fd_(other.fd_), inbuf_(std::move(other.inbuf_)), status_(other.status_),
      last_errno_(other.last_errno_) {
  other.fd_ = -1;
  other.status_ = {core::StatusCode::kInternal};
  other.last_errno_ = 0;
}

ControlClient &ControlClient::operator=(ControlClient &&other) noexcept {
  if (this != &other) {
    cleanup();
    endpoint_ = std::move(other.endpoint_);
    timeout_ms_ = other.timeout_ms_;
    fd_ = other.fd_;
    inbuf_ = std::move(other.inbuf_);
    status_ = other.status_;
    last_errno_ = other.last_errno_;
    other.fd_ = -1;
    other.status_ = {core::StatusCode::kInternal};
    other.last_errno_ = 0;
  }
  return *this;
}

void ControlClient::cleanup() noexcept {
  if (fd_ >= 0) {
    (void)::close(fd_);
    fd_ = -1;
  }
  inbuf_.clear();
}

void ControlClient::close() noexcept {
  cleanup();
  if (status_.ok()) {
    status_ = {core::StatusCode::kInternal};
    last_errno_ = 0;
  }
}

bool ControlClient::send_line(std::string_view line) noexcept {
  if (!status_.ok() || fd_ < 0) {
    return false;
  }
  if (line.size() > signal::control::kMaxLineBytes) {
    status_ = {core::StatusCode::kInvalidArgument};
    last_errno_ = 0;
    return false;
  }
  std::string wire(line.data(), line.size());
  wire.push_back('\n');
  std::size_t sent = 0;
  while (sent < wire.size()) {
    const ssize_t n =
        ::send(fd_, wire.data() + sent, wire.size() - sent, MSG_NOSIGNAL);
    if (n < 0) {
      const int err = errno;
      if (err == EINTR) {
        continue;
      }
      last_errno_ = err;
      status_ = {core::StatusCode::kInternal};
      return false;
    }
    if (n == 0) {
      last_errno_ = EPIPE;
      status_ = {core::StatusCode::kInternal};
      return false;
    }
    sent += static_cast<std::size_t>(n);
  }
  return true;
}

bool ControlClient::recv_line(std::string &line) noexcept {
  line.clear();
  if (!status_.ok() || fd_ < 0) {
    return false;
  }
  for (;;) {
    const std::size_t nl = inbuf_.find('\n');
    if (nl != std::string::npos) {
      line.assign(inbuf_.data(), nl);
      inbuf_.erase(0, nl + 1);
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      return true;
    }
    if (inbuf_.size() > signal::control::kMaxLineBytes) {
      inbuf_.clear();
      status_ = {core::StatusCode::kInternal};
      last_errno_ = 0;
      return false;
    }
    char buf[512];
    const ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
    if (n < 0) {
      const int err = errno;
      if (err == EINTR) {
        continue;
      }
      last_errno_ = err; // EAGAIN/EWOULDBLOCK: read timeout
      status_ = {core::StatusCode::kInternal};
      return false;
    }
    if (n == 0) {
      last_errno_ = ECONNRESET; // peer closed mid-response
      status_ = {core::StatusCode::kInternal};
      return false;
    }
    inbuf_.append(buf, static_cast<std::size_t>(n));
  }
}

} // namespace wengine::link
