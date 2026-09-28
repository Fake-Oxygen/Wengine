// POSIX UART serial publisher + control pump for the SIG-8 Zephyr HIL
// track (see include/wengine/link/uart_publisher.hpp). Platform TU by the
// same justification as socket_publisher.cpp: termios/open(2) syscalls
// live here, never in the header (a Windows port replaces this file).

#include "wengine/link/uart_publisher.hpp"
#include "wengine/signal/types.hpp" // quantity_suffix (stamped line field)

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <fcntl.h>
#include <string>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace wengine::link::uart {

namespace {

// Response byte cap per control pump (mirrors the TCP control server:
// past it, new responses are refused and counted, never grown).
inline constexpr std::size_t kControlOutCap = 64 * 1024;

// Control-run identity for HELLO provenance (same process/time mixing as
// the stream publishers and the TCP control server).
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

bool baud_to_speed(unsigned baud, speed_t &speed) noexcept {
  switch (baud) {
  case 9600u:
    speed = B9600;
    return true;
  case 19200u:
    speed = B19200;
    return true;
  case 38400u:
    speed = B38400;
    return true;
  case 57600u:
    speed = B57600;
    return true;
  case 115200u:
    speed = B115200;
    return true;
  case 230400u:
    speed = B230400;
    return true;
  case 460800u:
    speed = B460800;
    return true;
  case 921600u:
    speed = B921600;
    return true;
  default:
    return false;
  }
}

} // namespace

UartPort::UartPort(std::string_view device, unsigned baud)
    : device_(device.data(), device.size()), baud_(baud) {
  if (device_.empty() || !baud_supported(baud_)) {
    status_ = {core::StatusCode::kInvalidArgument};
    return;
  }
  speed_t speed = B115200;
  if (!baud_to_speed(baud_, speed)) {
    status_ = {core::StatusCode::kInvalidArgument};
    return;
  }
  const int fd = ::open(device_.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (fd < 0) {
    status_ = {core::StatusCode::kInternal};
    last_errno_ = errno;
    return;
  }
  if (::isatty(fd) == 0) {
    // UART semantics need termios; a silent regular-file fallback would
    // fake the baud budget the docs promise.
    last_errno_ = ENOTTY;
    ::close(fd);
    status_ = {core::StatusCode::kInvalidArgument};
    return;
  }
  termios tio{};
  if (::tcgetattr(fd, &tio) != 0) {
    last_errno_ = errno;
    ::close(fd);
    status_ = {core::StatusCode::kInternal};
    return;
  }
  ::cfmakeraw(&tio);
  tio.c_cflag |= static_cast<unsigned>(CLOCAL) | static_cast<unsigned>(CREAD);
  tio.c_cflag &= ~static_cast<unsigned>(CSIZE);
  tio.c_cflag |= static_cast<unsigned>(CS8); // 8N1: 8 data, no parity, 1 stop
  tio.c_cc[VMIN] = 0;
  tio.c_cc[VTIME] = 0;
  if (::cfsetspeed(&tio, speed) != 0 || ::tcsetattr(fd, TCSANOW, &tio) != 0) {
    last_errno_ = errno;
    ::close(fd);
    status_ = {core::StatusCode::kInternal};
    return;
  }
  fd_ = fd;
}

UartPort::~UartPort() { cleanup(); }

UartPort::UartPort(UartPort &&other) noexcept
    : device_(std::move(other.device_)), baud_(other.baud_), fd_(other.fd_),
      status_(other.status_), last_errno_(other.last_errno_) {
  other.fd_ = -1;
  other.baud_ = kDefaultUartBaud;
  other.last_errno_ = 0;
}

UartPort &UartPort::operator=(UartPort &&other) noexcept {
  if (this != &other) {
    cleanup();
    device_ = std::move(other.device_);
    baud_ = other.baud_;
    fd_ = other.fd_;
    status_ = other.status_;
    last_errno_ = other.last_errno_;
    other.fd_ = -1;
    other.baud_ = kDefaultUartBaud;
    other.last_errno_ = 0;
  }
  return *this;
}

void UartPort::cleanup() noexcept {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

UartPort::WriteOutcome UartPort::write_nowait(const char *data, std::size_t len,
                                              std::size_t &accepted) noexcept {
  accepted = 0;
  if (fd_ < 0) {
    return WriteOutcome::kDead;
  }
  while (accepted < len) {
    const ssize_t n = ::write(fd_, data + accepted, len - accepted);
    if (n < 0) {
      const int e = errno;
      if (e == EINTR) {
        continue;
      }
      if (e == EAGAIN || e == EWOULDBLOCK) {
        return WriteOutcome::kBackpressure;
      }
      return WriteOutcome::kDead;
    }
    if (n == 0) {
      return WriteOutcome::kDead;
    }
    accepted += static_cast<std::size_t>(n);
  }
  return WriteOutcome::kComplete;
}

int UartPort::read_nowait(char *dst, std::size_t cap, bool &dead) noexcept {
  dead = false;
  if (fd_ < 0 || cap == 0) {
    dead = fd_ < 0;
    return 0;
  }
  for (;;) {
    const ssize_t n = ::read(fd_, dst, cap);
    if (n < 0) {
      const int e = errno;
      if (e == EINTR) {
        continue;
      }
      if (e == EAGAIN || e == EWOULDBLOCK) {
        return 0;
      }
      dead = true; // hung up / unplugged (EIO): sticky upstream
      return -1;
    }
    // n == 0 is "nothing available", NOT hangup: VMIN=0/VTIME=0 timed
    // reads return zero on empty by termios definition (unlike sockets,
    // where 0 is EOF). A dead serial line surfaces as EIO above.
    return static_cast<int>(n);
  }
}

UartPublisher::UartPublisher(UartPort &port, UartMode mode) : port_(port), mode_(mode) {
  if (!port_.ok()) {
    status_ = port_.status();
    last_errno_ = port_.last_errno();
  }
}

void UartPublisher::on_snapshot(const world::Snapshot &snapshot) {
  ++stats_.attempted;
  if (!status_.ok()) {
    ++stats_.disabled_after_error;
    return;
  }
  // One write() per line; a short write abandons the rest of THAT line
  // (UDP-style: the next line resyncs, the reader counts one malformed
  // row). No pending tail, no stall, no poison — UART never
  // back-pressures the sim.
  bool complete = true;
  for (std::size_t k = 0; k < snapshot.signal_count(); ++k) {
    const auto qty = static_cast<signal::QuantityId>(snapshot.sig_quantity[k]);
    const std::string line = build_stream_line(snapshot.step_index, snapshot.sim_time,
                                               snapshot.sig_serial[k], snapshot.sig_value[k],
                                               signal::quantity_suffix(qty));
    wire_.clear();
    if (mode_ == UartMode::kAscii) {
      wire_.append(line.data(), line.size());
      wire_.push_back('\n');
    } else {
      wire_.push_back(kFrameStream);
      wire_.append(line.data(), line.size());
      std::string framed;
      framed.reserve(wire_.size() + wire_.size() / 254 + 2);
      if (!cobs_encode(wire_, framed).ok()) {
        status_ = {core::StatusCode::kInternal};
        ++stats_.disabled_after_error;
        return;
      }
      wire_ = std::move(framed);
      wire_.push_back('\0');
    }
    std::size_t accepted = 0;
    const UartPort::WriteOutcome w = port_.write_nowait(wire_.data(), wire_.size(), accepted);
    stats_.bytes_accepted += static_cast<std::uint64_t>(accepted);
    if (w == UartPort::WriteOutcome::kComplete) {
      ++lines_written_;
    } else {
      ++lines_dropped_;
      complete = false;
      if (w == UartPort::WriteOutcome::kDead) {
        status_ = {core::StatusCode::kInternal};
        last_errno_ = 0; // errno observed inside the port; sticky class suffices
        ++stats_.disabled_after_error;
        return;
      }
    }
  }
  if (complete) {
    ++stats_.published;
  } else {
    ++stats_.dropped_send;
  }
}

UartControl::UartControl(UartPort &port, UartMode mode)
    : port_(port), mode_(mode), run_id_(make_run_id()) {
  if (!port_.ok()) {
    status_ = port_.status();
  }
}

void UartControl::poll_lines(std::vector<std::string> &out) {
  out.clear();
  if (!status_.ok()) {
    return;
  }
  // Drain everything available (non-blocking loop over fixed chunks).
  // A dead peer does not discard already-received bytes: they split
  // below before the pump goes sticky.
  bool dead = false;
  for (;;) {
    char buf[512];
    bool chunk_dead = false;
    const int n = port_.read_nowait(buf, sizeof(buf), chunk_dead);
    if (chunk_dead) {
      dead = true;
      break;
    }
    if (n <= 0) {
      break;
    }
    inbuf_.append(buf, static_cast<std::size_t>(n));
  }
  // A delimiter-free buffer past the frame cap can never become valid.
  const char delim = (mode_ == UartMode::kAscii) ? '\n' : '\0';
  if (inbuf_.find(delim) == std::string::npos && inbuf_.size() > kMaxFrameBytes + 64) {
    inbuf_.clear();
    ++malformed_;
  }
  std::size_t begin = 0;
  for (;;) {
    const std::size_t at = inbuf_.find(delim, begin);
    if (at == std::string::npos) {
      break;
    }
    std::string unit = inbuf_.substr(begin, at - begin);
    begin = at + 1;
    if (mode_ == UartMode::kAscii) {
      if (!unit.empty() && unit.back() == '\r') {
        unit.pop_back();
      }
      if (unit.empty()) {
        continue; // line noise: ignore blank lines silently
      }
      if (unit[0] != kControlPrefix) {
        ++malformed_; // stream is transmit-only; devices speak control
        continue;
      }
      if (unit.size() == 1) {
        ++malformed_; // bare '!' carries no command
        continue;
      }
      out.push_back(unit.substr(1));
      ++commands_rx_;
    } else {
      if (unit.empty()) {
        continue; // stray delimiter: ignore silently
      }
      const auto decoded =
          cobs_decode(std::string_view(unit.data(), unit.size()));
      if (!decoded.ok() || decoded.value.empty()) {
        ++malformed_;
        continue;
      }
      const std::string &payload = decoded.value;
      if (payload[0] != kFrameCommand || payload.size() == 1) {
        ++malformed_; // responses/stream echo back at the host: not commands
        continue;
      }
      out.push_back(payload.substr(1));
      ++commands_rx_;
    }
  }
  inbuf_.erase(0, begin);
  if (dead) {
    status_ = {core::StatusCode::kInternal}; // peer gone: buffered lines kept
  }
}

void UartControl::push_response(std::string_view line) {
  if (!status_.ok()) {
    ++responses_dropped_;
    return;
  }
  std::string msg;
  if (mode_ == UartMode::kAscii) {
    msg.reserve(line.size() + 2);
    msg.push_back(kControlPrefix);
    msg.append(line.data(), line.size());
    msg.push_back('\n');
  } else {
    std::string payload;
    payload.reserve(line.size() + 1);
    payload.push_back(kFrameResponse);
    payload.append(line.data(), line.size());
    std::string framed;
    if (!cobs_encode(payload, framed).ok()) {
      ++responses_dropped_;
      return;
    }
    msg = std::move(framed);
    msg.push_back('\0');
  }
  if (outbuf_.size() + msg.size() > kControlOutCap) {
    ++responses_dropped_;
    return;
  }
  outbuf_ += msg;
  ++responses_tx_;
}

bool UartControl::flush() noexcept {
  if (!status_.ok()) {
    return false;
  }
  while (!outbuf_.empty()) {
    std::size_t accepted = 0;
    const UartPort::WriteOutcome w =
        port_.write_nowait(outbuf_.data(), outbuf_.size(), accepted);
    outbuf_.erase(0, accepted);
    if (w == UartPort::WriteOutcome::kDead) {
      status_ = {core::StatusCode::kInternal};
      return false;
    }
    if (w == UartPort::WriteOutcome::kBackpressure) {
      return false; // transient: retry next flush (response completes)
    }
  }
  return true;
}

} // namespace wengine::link::uart
