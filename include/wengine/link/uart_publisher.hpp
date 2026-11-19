/**
 * @file
 * UART serial publisher + control pump for the SIG-8 Zephyr HIL track.
 * @note UartPort owns one serial fd (termios 8N1 raw, O_NONBLOCK, CLOCAL). UartPublisher is
 *       an emit::ISink like every other transport (signal-row lines in the signals-CSV row
 *       shape, so CSV goldens and the Zephyr parser never drift; writes never block the sim
 *       loop; backpressure is a loud drop, never a stall). UartControl is the D4b-A control
 *       interleave on the SAME wire (domain-free SIG-10 text, like ControlServer moves TCP
 *       lines); control delivery is correctness-first (one pending response completes before
 *       newer ones, TCP-style). Stats reuse link::PublisherStats, so the shared health report
 *       and the attempted == published + dropped + ... accounting hold unchanged. Header stays
 *       platform-free; the POSIX TU is src/link/uart_publisher.cpp under wengine_link_shm —
 *       a Windows port replaces that TU, never this header. Consumers: CLI --emit-uart /
 *       --control-uart glued to solvers::ControlBus at the step boundary.
 */
#pragma once

// UART serial publisher + control pump for the SIG-8 Zephyr HIL track
// (second physical consumer after Zephyr-over-IP; independent track).
//
// UartPort owns one serial fd (termios 8N1 raw, O_NONBLOCK, CLOCAL):
// opened on a device path (`/dev/ttyUSB0`, pty symlinks, ...) at an
// explicit baud (115200 default). Non-tty paths are refused loudly —
// UART semantics need termios, and a silent file fallback would fake
// the baud budget the docs promise.
//
// UartPublisher is an emit::ISink like every other transport: it renders
// each snapshot's signal rows as uart_codec lines (the signals-CSV row
// shape, so CSV goldens and the Zephyr parser never drift) and writes
// them without ever blocking the sim loop:
//
//   ascii: raw lines (one write() per line; short writes abandon the rest
//          of that line UDP-style — freshness beats staleness on a HIL
//          display, and the next line resyncs; counted, never silent)
//   cobs:  COBS('S' + line) + 0x00 frames (same bytes, resyncable)
//
// Backpressure is a loud drop (dropped_send), never a stall and never a
// poisoned publisher: UART has no handshake (like UDP) and no length
// framing to corrupt (unlike TCP), so there is no pending tail to finish.
// Stats reuse link::PublisherStats, so the shared health report and the
// attempted == published + dropped + ... accounting hold unchanged.
//
// UartControl is the D4b-A control interleave on the SAME wire: it moves
// SIG-10 text lines without knowing the verbs (like ControlServer moves
// TCP lines). Device->host commands arrive as `!line\n` (ascii) or
// `COBS('Q' + line)` (cobs); host->device responses leave as `!line\n`
// or `COBS('R' + line)`. Stream lines are transmit-only here (a device
// sending stream bytes is malformed and counted). Unlike the stream
// publisher, control delivery is correctness-first: one pending response
// buffer completes before newer ones (TCP-style), because a dropped
// response breaks request-response pairing invisibly.
//
// The driver (CLI `--emit-uart` / `--control-uart`) glues both to
// solvers::ControlBus exactly like the TCP control path: poll_lines ->
// bus.enqueue(uart_channel, line) -> step-boundary drain ->
// take_responses -> push_response -> flush. Channel order is documented
// at attach: local CLI, TCP slots, then the UART channel.
//
// PORTABILITY: POSIX-only TU (src/link/uart_publisher.cpp) — termios,
// open(2), non-blocking I/O; the Windows port replaces that TU, never
// this header. Not copyable (owns fd); movable. Single driver thread
// only (poll + bus drain + step + flush), like every Runner-driven sink.

#include "wengine/core/status.hpp"
#include "wengine/emit/sink.hpp"
#include "wengine/link/protocol.hpp" // PublisherStats (shared accounting)
#include "wengine/link/uart_codec.hpp"
#include "wengine/world/state.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace wengine::link::uart {

/**
 * @brief Owned serial-port handle (one fd, termios 8N1 raw, O_NONBLOCK, CLOCAL).
 * @note Non-tty paths are refused loudly — UART semantics need termios, and a silent file fallback would fake the baud budget. Not copyable (owns fd); movable.
 */
class UartPort {
public:
/** @brief Default-construct; closed until open() succeeds. */
  UartPort() = default;
/**
 * @brief Open a serial device at an explicit baud.
 * @param[in] device serial device path.
 * @param[in] baud rate in bits/s (kSupportedBauds allowlist; default kDefaultUartBaud).
 */
  explicit UartPort(std::string_view device, unsigned baud);
/** @brief Close the fd. */
  ~UartPort();

/** @brief Non-copyable (owns fd). */
  UartPort(const UartPort &) = delete;
/** @brief Non-copyable (owns fd). */
  UartPort &operator=(const UartPort &) = delete;
/** @brief Move-construct. @param[in] other Source; left inert. */
  UartPort(UartPort &&other) noexcept;
/** @brief Move-assign. @param[in] other Source; left inert. */
  UartPort &operator=(UartPort &&other) noexcept;

  // One non-blocking write attempt: returns bytes accepted and whether
  // the whole message went (complete), the kernel is full (backpressure:
  // retry or abandon per caller policy), or the peer is dead (sticky).
/**
 * @brief One non-blocking write attempt: kComplete means the whole message went, kBackpressure
 *        means the kernel is full (retry or abandon per caller policy), kDead means the peer is
 *        dead (sticky).
 */
  enum class WriteOutcome : std::uint8_t { kComplete, kBackpressure, kDead };
/**
 * @brief One non-blocking write attempt.
 * @param[in] data bytes to write.
 * @param[in] len byte count.
 * @param[out] accepted bytes accepted by the kernel.
 * @return kComplete/kBackpressure/kDead. Never blocks, never throws.
 */
  WriteOutcome write_nowait(const char *data, std::size_t len, std::size_t &accepted) noexcept;

  // One non-blocking read (0 = nothing available; <0 with dead=true means
  // the peer hung up or errored). Never blocks, never throws.
/**
 * @brief One non-blocking read.
 * @param[out] dst destination buffer.
 * @param[in] cap dst capacity.
 * @param[out] dead true when the peer hung up or errored.
 * @return Bytes read (0 = nothing available; <0 with dead=true on hangup/error). Never blocks, never throws.
 */
  int read_nowait(char *dst, std::size_t cap, bool &dead) noexcept;

/** @brief Success predicate. @return True while no sticky failure latched. */
  [[nodiscard]] bool ok() const noexcept { return status_.ok(); }
/** @brief Latched sticky status. @return kOk or the first sticky failure. */
  [[nodiscard]] core::Status status() const noexcept { return status_; }
/** @brief Errno captured at the failing syscall (0 when ok). @return Captured errno value. */
  [[nodiscard]] int last_errno() const noexcept { return last_errno_; }
/** @brief Open device path. @return Device string. */
  [[nodiscard]] const std::string &device() const noexcept { return device_; }
/** @brief Configured baud rate in bits/s. @return Baud value. */
  [[nodiscard]] unsigned baud() const noexcept { return baud_; }

private:
  void cleanup() noexcept;

  std::string device_;
  unsigned baud_ = kDefaultUartBaud;
  int fd_ = -1;
  core::Status status_{};
  int last_errno_ = 0;
};

// Signal-row UART stream publisher (ISink, caller-owned like every sink).
// Signal-domain only: snapshots without signal rows emit zero lines (the
// CLI rejects --emit-uart for non-signal solvers instead of faking it).
/**
 * @brief Signal-row UART stream publisher (ISink, caller-owned like every sink; signal-domain only).
 * @note Snapshots without signal rows emit zero lines (the CLI rejects --emit-uart for non-signal
 *       solvers instead of faking it). ascii: raw lines (one write() per line; short writes abandon
 *       the rest of that line UDP-style — freshness beats staleness, next line resyncs; counted,
 *       never silent). cobs: COBS('S' + line) + 0x00 frames. No pending tail (no handshake like UDP,
 *       no length framing to corrupt like TCP), so no finish. UART has no handshake and no length
 *       framing to corrupt, so backpressure is a loud drop (dropped_send), never a poisoned publisher.
 */
class UartPublisher : public emit::ISink {
public:
/**
 * @brief Attach a stream publisher to an open port.
 * @param[in] port caller-owned open port (must outlive this publisher).
 * @param[in] mode wire mode (ascii raw lines or cobs frames).
 */
  explicit UartPublisher(UartPort &port, UartMode mode = UartMode::kAscii);

/**
 * @brief Render one snapshot's signal rows as uart_codec lines and write them without ever blocking the sim loop.
 * @param[in] snapshot Runner-owned emission buffer (observed synchronously; never retained).
 */
  void on_snapshot(const world::Snapshot &snapshot) override;

/** @brief Success predicate. @return True while no sticky failure latched. */
  [[nodiscard]] bool ok() const noexcept { return status_.ok(); }
/** @brief Latched sticky status. @return kOk or the first sticky failure. */
  [[nodiscard]] core::Status status() const noexcept { return status_; }
/** @brief Errno captured at the failing syscall (0 when ok). @return Captured errno value. */
  [[nodiscard]] int last_errno() const noexcept { return last_errno_; }
/** @brief Monotonic publisher counters (attempted/published/dropped/disabled/bytes_accepted; local acceptance only). @return Counter snapshot. */
  [[nodiscard]] const PublisherStats &stats() const noexcept { return stats_; }
/** @brief Stream lines fully written. @return Line count. */
  [[nodiscard]] std::uint64_t lines_written() const noexcept { return lines_written_; }
/** @brief Stream lines abandoned on short writes (freshness beats staleness; counted, never silent). @return Line count. */
  [[nodiscard]] std::uint64_t lines_dropped() const noexcept { return lines_dropped_; }
/** @brief Underlying device path. @return Device string. */
  [[nodiscard]] const std::string &device() const noexcept { return port_.device(); }
/** @brief Underlying baud rate in bits/s. @return Baud value. */
  [[nodiscard]] unsigned baud() const noexcept { return port_.baud(); }
/** @brief Configured wire mode. @return Wire mode. */
  [[nodiscard]] UartMode mode() const noexcept { return mode_; }

private:
  UartPort &port_; // caller-owned (outlives this publisher)
  UartMode mode_ = UartMode::kAscii;
  std::string wire_; // scratch: one framed line or frame (reused, no growth)
  PublisherStats stats_{};
  std::uint64_t lines_written_ = 0;
  std::uint64_t lines_dropped_ = 0;
  core::Status status_{};
  int last_errno_ = 0;
};

// Domain-free UART control pump over the same port (see header doc).
// poll_lines clears `out` then appends received command lines (without
// any prefix); push_response queues one response line; flush writes
// pending response bytes (true when fully drained).
/**
 * @brief Domain-free UART control pump over the same port (see header doc).
 * @note poll_lines clears out then appends received command lines (without any prefix);
 *       push_response queues one response line; flush writes pending response bytes (true when
 *       fully drained). Device-to-host commands arrive as `!line` (ascii) or `COBS('Q' + line)`
 *       (cobs); host-to-device responses leave as `!line` or `COBS('R' + line)`. Stream lines are
 *       transmit-only here (a device sending stream bytes is malformed and counted). One pending
 *       response buffer completes before newer ones (correctness-first: a dropped response breaks
 *       request-response pairing invisibly). Channel order documented at attach: local CLI, TCP slots, then UART.
 */
class UartControl {
public:
/**
 * @brief Attach a control pump to an open port (shared with the stream publisher).
 * @param[in] port caller-owned open port (must outlive this pump).
 * @param[in] mode wire mode.
 */
  explicit UartControl(UartPort &port, UartMode mode = UartMode::kAscii);

/**
 * @brief Non-blocking progress: read available bytes, split complete command units, count malformed runs.
 * @param[out] out cleared first, then received command lines (prefix-free).
 */
  void poll_lines(std::vector<std::string> &out);
/**
 * @brief Queue one response line (framing added here).
 * @param[in] line response text (newest waits while one pending response completes).
 */
  void push_response(std::string_view line);
/** @brief Write pending response bytes. @return True when fully drained. Never blocks, never throws. */
  bool flush() noexcept;

/** @brief Success predicate. @return True while no sticky failure latched. */
  [[nodiscard]] bool ok() const noexcept { return status_.ok(); }
/** @brief Latched sticky status. @return kOk or the first sticky failure. */
  [[nodiscard]] core::Status status() const noexcept { return status_; }
/** @brief Control-run identity for HELLO provenance. @return Run id value. */
  [[nodiscard]] std::uint64_t run_id() const noexcept { return run_id_; }
/** @brief Malformed units counted and dropped (garbage runs, COBS failures, device stream bytes). @return Count. */
  [[nodiscard]] std::uint64_t malformed() const noexcept { return malformed_; }
/** @brief Command lines received. @return Count. */
  [[nodiscard]] std::uint64_t commands_rx() const noexcept { return commands_rx_; }
/** @brief Response lines fully written. @return Count. */
  [[nodiscard]] std::uint64_t responses_tx() const noexcept { return responses_tx_; }
/** @brief Responses dropped with a dead peer. @return Count. */
  [[nodiscard]] std::uint64_t responses_dropped() const noexcept { return responses_dropped_; }

private:
  UartPort &port_; // caller-owned (shared with the stream publisher)
  UartMode mode_ = UartMode::kAscii;
  std::string inbuf_;
  std::string outbuf_; // one pending response (correctness-first: completes)
  std::uint64_t run_id_ = 0; // control-run identity for HELLO provenance
  std::uint64_t malformed_ = 0;
  std::uint64_t commands_rx_ = 0;
  std::uint64_t responses_tx_ = 0;
  std::uint64_t responses_dropped_ = 0;
  core::Status status_{};
};

} // namespace wengine::link::uart
