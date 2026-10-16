// Private socket syscall helpers shared by the wengine-link POSIX endpoint
// TUs (socket_publisher.cpp, tcp_worker.cpp). src-private by design: never
// installed, never included from public headers — a Windows port replaces
// the TUs (and this file) with Winsock2, never the public headers.
//
// Moved verbatim from socket_publisher.cpp's anonymous namespace when the
// PHYSI-46 worker needed the same connect/send/buffer/run-id mechanics:
// one copy, no drift between the synchronous publisher and the worker.

#pragma once

#if defined(_WIN32)
#error "wengine-link socket helpers are POSIX-only. The Windows port replaces the endpoint TUs."
#endif

#include "wengine/link/socket_publisher.hpp"

#include <cerrno>
#include <cstdint>
#include <ctime>
#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <string>

namespace wengine::link::socket_internal {

// Edge-only (construction) connect timeout: bounds a hanging connect to a
// silent remote without ever touching the hot publish path.
inline constexpr int kConnectTimeoutMs = 3000;

inline bool set_nonblocking(int fd) noexcept {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return false;
  }
  return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

inline bool set_cloexec(int fd) noexcept {
  const int flags = ::fcntl(fd, F_GETFD, 0);
  if (flags < 0) {
    return false;
  }
  return ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

// Wait for a non-blocking connect to finish; false on timeout or refusal
// (errno set: ETIMEDOUT or the peer's verdict).
inline bool wait_connected(int fd, int timeout_ms) noexcept {
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
inline SendResult send_all(int fd, const unsigned char *buf, std::size_t len,
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
inline SendResult send_datagram(int fd, const unsigned char *buf, std::size_t len,
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
inline int open_connected(const HostPort &hp, int kind, int timeout_ms,
                          int &err_out) noexcept {
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

inline void tune_send_buffer(int fd) noexcept {
  int sndbuf = kSocketSendBuffer;
  ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)); // best-effort
}

// Publisher-run identity for v2 records: process/time mixed via splitmix64.
// Opaque to the codec (any u64 goes); distinct constructions must differ so
// receivers can reset cursors on restart (PHYSI-40 session semantics).
// Best-effort only — collisions degrade to one extra missed-span report,
// never to corrupt data.
inline std::uint64_t make_run_id() noexcept {
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

} // namespace wengine::link::socket_internal
