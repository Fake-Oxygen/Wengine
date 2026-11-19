/**
 * @file
 * Blocking TCP line client for the SIG-10 generic control plane (CLI edge only).
 * @note Domain-free transport: moves newline-terminated text lines, knows no signal verbs.
 *       Header stays platform-free; the POSIX TU is src/link/control_client.cpp —
 *       a Windows port replaces that TU, never this header. Consumers: CLI control-plane tools.
 */
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
/** @brief Edge-only connect + read timeout in ms (bounds a hanging peer; never runs on the sim thread). */
inline constexpr int kControlClientTimeoutMs = 3000;

/**
 * @brief Blocking TCP line client for SIG-10 control verbs (send lines, read single-line verdicts).
 * @note CLI edge only: blocking I/O here can never stall a run. Copying is deleted (owns fd + buffers); movable. The destructor never blocks (closes only).
 */
class ControlClient {
public:
/**
 * @brief Connect to a host:port endpoint with a construction-time timeout.
 * @param[in] endpoint host:port text (same shape as --emit-tcp; shape failures are kInvalidArgument, CLI exit 2).
 * @param[in] timeout_ms connect/read timeout in ms.
 * @note Connect/read failures are sticky kInternal with last_errno() (CLI exit 1): a refused control connection is a runtime failure, never a usage error.
 */
  explicit ControlClient(std::string_view endpoint,
                         int timeout_ms = kControlClientTimeoutMs);
/** @brief Close the socket; never blocks. */
  ~ControlClient();

/** @brief Non-copyable (owns fd + buffers). */
  ControlClient(const ControlClient &) = delete;
/** @brief Non-copyable (owns fd + buffers). */
  ControlClient &operator=(const ControlClient &) = delete;
/** @brief Move-construct. @param[in] other Source; left inert. */
  ControlClient(ControlClient &&other) noexcept;
/** @brief Move-assign. @param[in] other Source; left inert. */
  ControlClient &operator=(ControlClient &&other) noexcept;

  // Send one command line (`\n` appended here). False when !ok() or the
  // peer is dead (sticky kInternal); overlong lines are refused locally
  // (sticky kInvalidArgument, nothing sent).
/**
 * @brief Send one command line (newline appended here).
 * @param[in] line command text without trailing newline (over signal::control::kMaxLineBytes refused locally, nothing sent).
 * @return False when !ok() or the peer is dead (sticky kInternal); overlong lines fail sticky kInvalidArgument.
 */
  bool send_line(std::string_view line) noexcept;

  // Read one `\n`-terminated response line (trailing `\r` stripped, no
  // newline kept). False on timeout, disconnect, or an overlong line
  // (sticky kInternal, line discarded). LIST answers need 1 + n calls
  // (the `OK list <n>` verdict first, then exactly n `SRC` lines).
/**
 * @brief Read one newline-terminated response line (trailing CR stripped, newline not kept).
 * @param[out] line received line (discarded on failure).
 * @return False on timeout, disconnect, or an overlong line (sticky kInternal, line discarded). LIST answers need 1 + n calls (verdict first, then exactly n SRC lines).
 */
  bool recv_line(std::string &line) noexcept;

/** @brief Close the connection; idempotent, never blocks. */
  void close() noexcept;

/** @brief Success predicate: true while no sticky failure latched. @return True when status is kOk. */
  [[nodiscard]] bool ok() const noexcept { return status_.ok(); }
/** @brief Latched sticky status. @return kOk, kInvalidArgument (local refusal), or kInternal (peer/timeout). */
  [[nodiscard]] core::Status status() const noexcept { return status_; }
/** @brief Errno captured at the failing syscall (0 when ok). @return Captured errno value. */
  [[nodiscard]] int last_errno() const noexcept { return last_errno_; }
/** @brief Configured host:port endpoint text. @return Endpoint string. */
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
