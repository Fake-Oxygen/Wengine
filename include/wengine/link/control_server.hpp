#pragma once

// TCP line server for the SIG-10 generic control plane (Zephyr/PC clients).
//
// Domain-free transport: this server moves newline-terminated text lines
// and knows nothing about signal verbs. The driver (CLI `sig run
// --control-tcp`, future GUI/REPL hooks) glues it to solvers::ControlBus:
// poll() collects client lines -> bus.enqueue(1 + slot, line) -> bus
// drains at the step boundary -> take_responses -> push_response -> flush().
//
// Model (same policy family as the stream publishers):
// - Listen on a bind endpoint (`host:port`, same shape as --emit-tcp).
// - Up to max_clients concurrent clients (default 8); beyond that the
//   accepted socket is closed immediately and counted (refused_full) —
//   loud, never a silent stall.
// - Client slots are stable: slot N always drains as bus channel 1+N, so
//   the fixed drain order never depends on connect/disconnect timing. A
//   new client reusing a slot inherits its channel (order by slot, not by
//   arrival). Channel 0 stays reserved for the local CLI.
// - poll() and flush() never block (non-blocking sockets, EAGAIN loops)
//   and never throw; the sim loop calls them around each step. Failures
//   are sticky core::Status with last_errno(), like every publisher.
// - Lines longer than signal::control::kMaxLineBytes are refused with an
//   inline `ERR invalid_argument line too long` queued to that client
//   (never forwarded to the bus); a buffer with no newline past the cap
//   is dropped the same way. Empty lines are ignored (telnet friendliness).
// - Responses are best-effort: each client has a bounded send buffer
//   (kControlSendCap). A client past the cap is closed and counted — an
//   applied command stays applied, so clients reconcile with LIST after a
//   disconnect (same "local acceptance is not receipt" vocabulary as
//   docs/delivery.md).
// - run_id is generated at construction (same process/time mixing as the
//   stream publishers) and reported in HELLO answers via the bus.
//
// POSIX-only TU (src/link/control_server.cpp) like the socket publishers;
// the Windows port replaces that TU, never this header. Single driver
// thread only (poll + bus drain + step + flush).
//
// UART control (SIG-8) speaks the same lines over COBS-escaped frames;
// this server is the TCP endpoint of that shared line protocol.

#include "wengine/core/status.hpp"
#include "wengine/link/socket_publisher.hpp" // parse_endpoint (endpoint shape)
#include "wengine/signal/control.hpp" // kMaxLineBytes (single source of truth)

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace wengine::link {

// Default concurrent-client ceiling (engineering choice: interactive
// control fan-in is small; raise explicitly for robot fleets).
inline constexpr std::uint32_t kDefaultControlClients = 8;
// Per-client pending-response cap in bytes (see header: past it the client
// is closed and counted, never grown unbounded).
inline constexpr std::size_t kControlSendCap = 64 * 1024;

// One poll's new lines from a client slot (slots are dense 0..clients).
struct ControlSlotLines {
  std::uint32_t slot = 0;
  std::vector<std::string> lines;
};

struct ControlServerStats {
  std::uint64_t accepted = 0;
  std::uint64_t refused_full = 0;
  std::uint64_t closed = 0;
  std::uint64_t lines = 0;
  std::uint64_t overlong_dropped = 0;
  std::uint64_t responses = 0;
  std::uint64_t response_dropped = 0;
  std::uint64_t bytes_sent = 0;
};

class ControlServer {
public:
  explicit ControlServer(std::string_view bind_endpoint,
                         std::uint32_t max_clients = kDefaultControlClients);
  ~ControlServer();

  ControlServer(const ControlServer &) = delete;
  ControlServer &operator=(const ControlServer &) = delete;
  ControlServer(ControlServer &&other) noexcept;
  ControlServer &operator=(ControlServer &&other) noexcept;

  // Non-blocking progress: accept pending clients, recv available bytes,
  // split complete lines. Clears `out` first, then appends one entry per
  // slot that produced lines (slot order). No-op when !ok().
  void poll(std::vector<ControlSlotLines> &out);

  // Queue one response line for a slot (`\n` appended here). Dead slots
  // and over-cap buffers are counted, never grown (see header).
  void push_response(std::uint32_t slot, std::string_view line);

  // Non-blocking send of pending response bytes. True when every send
  // buffer is empty (idle clean); false with ok() still true is transient
  // backpressure (retry later). Never blocks, never throws.
  bool flush() noexcept;

  [[nodiscard]] bool ok() const noexcept { return status_.ok(); }
  [[nodiscard]] core::Status status() const noexcept { return status_; }
  [[nodiscard]] int last_errno() const noexcept { return last_errno_; }
  [[nodiscard]] const ControlServerStats &stats() const noexcept { return stats_; }
  [[nodiscard]] const std::string &endpoint() const noexcept { return endpoint_; }
  [[nodiscard]] std::uint64_t run_id() const noexcept { return run_id_; }
  [[nodiscard]] std::uint32_t client_count() const noexcept { return live_clients_; }
  [[nodiscard]] std::uint32_t max_clients() const noexcept { return max_clients_; }

private:
  void cleanup() noexcept;
  void close_slot(std::uint32_t slot) noexcept;

  std::string endpoint_;
  std::uint32_t max_clients_ = kDefaultControlClients;
  std::uint64_t run_id_ = 0;
  int listen_fd_ = -1;
  struct Slot {
    int fd = -1; // -1 = free
    std::string inbuf;
    std::string outbuf;
  };
  std::vector<Slot> slots_;
  std::uint32_t live_clients_ = 0;
  ControlServerStats stats_{};
  core::Status status_{};
  int last_errno_ = 0;
};

} // namespace wengine::link
