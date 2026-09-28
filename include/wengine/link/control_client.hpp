#pragma once

// Blocking TCP line client for the SIG-10 generic control plane (SIG-6
// per-source family: `sig list/add/set/start/stop/drop`).
//
// Domain-free transport: this client moves newline-terminated text lines
// and knows nothing about signal verbs. Callers build command lines (the
// ADD/SET/... vocabulary in signal/control.hpp), send them, and read the
// single-line verdicts (plus LIST's trailing SRC lines) back.
//
// Model (CLI edge only):
// - Connect to a `host:port` endpoint (same shape as --emit-tcp and the
//   --control-tcp server bind endpoint; shape failures are kInvalidArgument
//   so the CLI reports exit 2, like every other endpoint shape error).
// - Connect/read failures are sticky kInternal with last_errno() (CLI
//   exit 1) — a refused control connection is a runtime failure, never a
//   usage error.
// - Blocking I/O with a construction-time timeout (default 3 s, same class
//   as the publisher connect timeout): this client never runs on the sim
//   thread, so blocking here cannot stall a run. The destructor never
//   blocks (it only closes the socket).
// - Lines longer than signal::control::kMaxLineBytes are refused locally
//   (never sent); received lines past the cap fail the read loudly.
//
// POSIX-only TU (src/link/control_client.cpp) like the socket publishers
// and the control server; the Windows port replaces that TU, never this
// header.

#include "wengine/core/status.hpp"
#include "wengine/link/socket_publisher.hpp" // parse_endpoint (endpoint shape)
#include "wengine/signal/control.hpp" // kMaxLineBytes (single source of truth)

#include <cstddef>
#include <string>
#include <string_view>

namespace wengine::link {

// Edge-only connect + read timeout (bounds a hanging peer without ever
// touching the sim thread — this client only runs at the CLI edge).
inline constexpr int kControlClientTimeoutMs = 3000;

class ControlClient {
public:
  explicit ControlClient(std::string_view endpoint,
                         int timeout_ms = kControlClientTimeoutMs);
  ~ControlClient();

  ControlClient(const ControlClient &) = delete;
  ControlClient &operator=(const ControlClient &) = delete;
  ControlClient(ControlClient &&other) noexcept;
  ControlClient &operator=(ControlClient &&other) noexcept;

  // Send one command line (`\n` appended here). False when !ok() or the
  // peer is dead (sticky kInternal); overlong lines are refused locally
  // (sticky kInvalidArgument, nothing sent).
  bool send_line(std::string_view line) noexcept;

  // Read one `\n`-terminated response line (trailing `\r` stripped, no
  // newline kept). False on timeout, disconnect, or an overlong line
  // (sticky kInternal, line discarded). LIST answers need 1 + n calls
  // (the `OK list <n>` verdict first, then exactly n `SRC` lines).
  bool recv_line(std::string &line) noexcept;

  void close() noexcept;

  [[nodiscard]] bool ok() const noexcept { return status_.ok(); }
  [[nodiscard]] core::Status status() const noexcept { return status_; }
  [[nodiscard]] int last_errno() const noexcept { return last_errno_; }
  [[nodiscard]] const std::string &endpoint() const noexcept { return endpoint_; }

private:
  void cleanup() noexcept;

  std::string endpoint_;
  int timeout_ms_ = kControlClientTimeoutMs;
  int fd_ = -1;
  std::string inbuf_;
  core::Status status_{};
  int last_errno_ = 0;
};

} // namespace wengine::link
