// POSIX TCP line server for the SIG-10 control plane (see
// include/wengine/link/control_server.hpp). Platform TU by the same
// justification as socket_publisher.cpp: syscalls live here, never in the
// header (a Windows port replaces this file with Winsock2).

#include "wengine/link/control_server.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <fcntl.h>
#include <netdb.h>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace wengine::link {

namespace {

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

// Server-run identity for HELLO provenance: process/time mixed via
// splitmix64 (same rationale as the stream publishers — distinct servers
// must differ so clients can reset sessions on restart; collisions only
// cost one extra session reset, never corrupt data).
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

// Bind + listen on the first addrinfo result that works. -1 with err_out
// set when nothing binds (resolution failures carry err_out == 0, like the
// stream publishers: no syscall errno exists for those).
int open_listener(const HostPort &hp, int &err_out) noexcept {
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
  err_out = EADDRNOTAVAIL;
  for (addrinfo *ai = list; ai != nullptr; ai = ai->ai_next) {
    fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) {
      err_out = errno;
      continue;
    }
    int one = 1;
    (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)); // best-effort
    set_cloexec(fd); // best-effort
    if (::bind(fd, ai->ai_addr, ai->ai_addrlen) != 0) {
      err_out = errno;
      ::close(fd);
      fd = -1;
      continue;
    }
    if (::listen(fd, 8) != 0) {
      err_out = errno;
      ::close(fd);
      fd = -1;
      continue;
    }
    if (!set_nonblocking(fd)) {
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
  err_out = 0;
  return fd;
}

} // namespace

ControlServer::ControlServer(std::string_view bind_endpoint, std::uint32_t max_clients)
    : endpoint_(bind_endpoint.data(), bind_endpoint.size()), max_clients_(max_clients),
      run_id_(make_run_id()) {
  if (max_clients_ < 1 || max_clients_ > 64) {
    status_ = {core::StatusCode::kInvalidArgument};
    return;
  }
  const auto ep = parse_endpoint(bind_endpoint);
  if (!ep.ok()) {
    status_ = {core::StatusCode::kInvalidArgument};
    return;
  }
  int err = 0;
  listen_fd_ = open_listener(ep.value, err);
  if (listen_fd_ < 0) {
    status_ = {core::StatusCode::kInternal};
    last_errno_ = err;
    return;
  }
  slots_.resize(max_clients_);
}

ControlServer::~ControlServer() { cleanup(); }

ControlServer::ControlServer(ControlServer &&other) noexcept
    : endpoint_(std::move(other.endpoint_)), max_clients_(other.max_clients_),
      run_id_(other.run_id_), listen_fd_(other.listen_fd_),
      slots_(std::move(other.slots_)), live_clients_(other.live_clients_),
      stats_(other.stats_), status_(other.status_), last_errno_(other.last_errno_) {
  other.listen_fd_ = -1;
  other.live_clients_ = 0;
  other.last_errno_ = 0;
}

ControlServer &ControlServer::operator=(ControlServer &&other) noexcept {
  if (this != &other) {
    cleanup();
    endpoint_ = std::move(other.endpoint_);
    run_id_ = other.run_id_;
    max_clients_ = other.max_clients_;
    listen_fd_ = other.listen_fd_;
    slots_ = std::move(other.slots_);
    live_clients_ = other.live_clients_;
    stats_ = other.stats_;
    status_ = other.status_;
    last_errno_ = other.last_errno_;
    other.listen_fd_ = -1;
    other.live_clients_ = 0;
    other.last_errno_ = 0;
  }
  return *this;
}

void ControlServer::cleanup() noexcept {
  for (Slot &s : slots_) {
    if (s.fd >= 0) {
      ::close(s.fd);
      s.fd = -1;
    }
  }
  slots_.clear();
  if (listen_fd_ >= 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
  live_clients_ = 0;
}

void ControlServer::close_slot(std::uint32_t slot) noexcept {
  if (slot < slots_.size() && slots_[slot].fd >= 0) {
    ::close(slots_[slot].fd);
    slots_[slot].fd = -1;
    slots_[slot].inbuf.clear();
    slots_[slot].outbuf.clear();
    --live_clients_;
    ++stats_.closed;
  }
}

void ControlServer::poll(std::vector<ControlSlotLines> &out) {
  out.clear();
  if (!status_.ok() || listen_fd_ < 0) {
    return;
  }
  // Accept everything pending (non-blocking loop).
  for (;;) {
    const int fd = ::accept(listen_fd_, nullptr, nullptr);
    if (fd < 0) {
      const int e = errno;
      if (e == EINTR) {
        continue;
      }
      break; // EAGAIN/EWOULDBLOCK: drained; anything else: retry next poll
    }
    set_cloexec(fd); // best-effort
    if (!set_nonblocking(fd)) {
      ::close(fd);
      continue;
    }
    std::uint32_t slot = max_clients_;
    for (std::uint32_t i = 0; i < slots_.size(); ++i) {
      if (slots_[i].fd < 0) {
        slot = i;
        break;
      }
    }
    if (slot >= slots_.size()) {
      ::close(fd); // full: loud refusal at accept, counted
      ++stats_.refused_full;
      continue;
    }
    slots_[slot].fd = fd;
    slots_[slot].inbuf.clear();
    slots_[slot].outbuf.clear();
    ++live_clients_;
    ++stats_.accepted;
  }
  // Recv everything available, slot order (drain order never observes this
  // loop's timing — only which lines are queued before the bus drain).
  for (std::uint32_t i = 0; i < slots_.size(); ++i) {
    Slot &s = slots_[i];
    if (s.fd < 0) {
      continue;
    }
    bool dead = false;
    for (;;) {
      char buf[4096];
      const ssize_t n = ::recv(s.fd, buf, sizeof(buf), 0);
      if (n < 0) {
        const int e = errno;
        if (e == EINTR) {
          continue;
        }
        break; // EAGAIN/EWOULDBLOCK: drained; else: retry/resurface next poll
      }
      if (n == 0) {
        dead = true; // orderly EOF
        break;
      }
      s.inbuf.append(buf, static_cast<std::size_t>(n));
    }
    if (dead) {
      close_slot(i);
      continue;
    }
    // A line-less buffer past the cap can never become valid: refuse it
    // now instead of growing without bound.
    if (s.inbuf.find('\n') == std::string::npos &&
        s.inbuf.size() > signal::control::kMaxLineBytes) {
      s.inbuf.clear();
      ++stats_.overlong_dropped;
      const char *msg = "ERR invalid_argument line too long\n";
      s.outbuf.append(msg);
      continue;
    }
    // Split complete lines.
    std::size_t begin = 0;
    bool produced = false;
    for (;;) {
      const std::size_t nl = s.inbuf.find('\n', begin);
      if (nl == std::string::npos) {
        break;
      }
      std::string line = s.inbuf.substr(begin, nl - begin);
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      begin = nl + 1;
      if (line.empty()) {
        continue; // telnet-friendly: ignore blank lines silently
      }
      if (line.size() > signal::control::kMaxLineBytes) {
        ++stats_.overlong_dropped;
        s.outbuf.append("ERR invalid_argument line too long\n");
        continue;
      }
      if (!produced) {
        out.push_back(ControlSlotLines{});
        out.back().slot = i;
        produced = true;
      }
      out.back().lines.push_back(std::move(line));
      ++stats_.lines;
    }
    s.inbuf.erase(0, begin);
  }
}

void ControlServer::push_response(std::uint32_t slot, std::string_view line) {
  if (!status_.ok() || slot >= slots_.size() || slots_[slot].fd < 0) {
    ++stats_.response_dropped;
    return;
  }
  Slot &s = slots_[slot];
  if (s.outbuf.size() + line.size() + 1 > kControlSendCap) {
    // Bounded memory wins over this client (see header): close loudly.
    // Applied commands stay applied; the client reconciles via LIST.
    ++stats_.response_dropped;
    close_slot(slot);
    return;
  }
  s.outbuf.append(line.data(), line.size());
  s.outbuf.push_back('\n');
  ++stats_.responses;
}

bool ControlServer::flush() noexcept {
  if (!status_.ok()) {
    return false;
  }
  bool empty = true;
  for (std::uint32_t i = 0; i < slots_.size(); ++i) {
    Slot &s = slots_[i];
    if (s.fd < 0) {
      continue;
    }
    while (!s.outbuf.empty()) {
      const ssize_t n =
          ::send(s.fd, s.outbuf.data(), s.outbuf.size(), MSG_NOSIGNAL); // no SIGPIPE
      if (n < 0) {
        const int e = errno;
        if (e == EINTR) {
          continue;
        }
        if (e == EAGAIN || e == EWOULDBLOCK) {
          break; // transient: retry next flush
        }
        close_slot(i); // dead peer
        break;
      }
      if (n == 0) {
        close_slot(i);
        break;
      }
      s.outbuf.erase(0, static_cast<std::size_t>(n));
      stats_.bytes_sent += static_cast<std::uint64_t>(n);
    }
    if (s.fd >= 0 && !s.outbuf.empty()) {
      empty = false;
    }
  }
  return empty;
}

} // namespace wengine::link
