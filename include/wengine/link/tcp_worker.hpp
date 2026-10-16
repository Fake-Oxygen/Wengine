#pragma once

// Worker-backed TCP output for batched signal samples (PHYSI-46): an
// opt-in emit::ISink that moves TCP encoding and socket servicing off the
// simulation thread.
//
// WHY: TcpPublisher performs non-blocking send() syscalls and compact
// encode copies on Runner's synchronous fan-out path, so one slow endpoint
// consumes simulation-thread time and pending writes stall until the next
// emission. Here the simulation callback only fills/seals/enqueues owned
// SampleBlocks (PHYSI-44 builder + PHYSI-45 queue); exactly one explicit
// worker thread (worker count stays one — no pool, no knob) handles wire
// encoding, socket readiness and pending writes.
//
// MODES (explicit per worker, fixed at construction):
// - kOrdered (sample mode): every sealed block is sent in seal order;
//   a full queue is reported to the driver via dropped_queue_full counts,
//   never hidden. Use for telemetry/recording.
// - kLatest (live-view mode): the worker explicitly coalesces a queued
//   backlog to the newest block before sending; coalesced blocks are
//   counted (coalesced_blocks/samples), never silent. Use for live
//   off-host display. (Coarse sibling of the SHM latest-only reader and
//   the deferred opt-in socket-coalescing policy; ordered stays default.)
//
// BOUNDS (memory + shutdown, both explicit):
// - Memory: queue slot/byte caps (TcpWorkerConfig::queue) + one wire
//   buffer (length prefix + one maximum-size block) + one in-flight block.
//   The sim callback never allocates past builder/queue warmup.
// - Shutdown: finish(timeout_ms) flushes the builder tail and waits (at
//   most) the deadline for the queue + pending bytes to drain WITHOUT
//   stopping the worker; shutdown() stops admission, abandons the
//   remainder with exact abandoned_* counts, joins the worker (bounded by
//   the worker poll slice, never by the peer) and closes the socket. The
//   destructor shuts down the same way and never blocks on the network.
//
// CONSERVATION (block units; exact at rest):
//   queue.pushed_blocks == published_blocks + dropped_send + coalesced_blocks
//                         + abandoned_blocks + queued_blocks + held_blocks
// and, once fully drained (queued == held == 0):
//   queue.pushed_samples == published_samples + dropped_send_samples
//                           + coalesced_samples + abandoned_samples
// Queue-full/closed refusals never enter the queue: they stay visible in
// queue.dropped_*/closed_* for the driver (ordered mode reports full this
// way) alongside attempted (snapshot calls + finish flushes, diagnostic)
// and disabled_after_error. Builder/encode over_bound failures poison the
// worker loudly and never become blocks; an encode-failed held block moves
// to abandoned at stop, so the post-stop equation still balances.
// Coalesced and queue-full-dropped blocks never take a send_seq, so sent
// seqs stay consecutive per run; receivers reconcile the sealed-vs-sent
// difference against coalesced_blocks and the queue counters exactly like
// StrideSink skips — intentional, not loss.
// send_seq counts blocks accepted FOR SENDING (not seals), strictly
// increasing per run; run_id is the PHYSI-40 session identity.
//
// THREADING: one producer (on_snapshot, sim thread) + one consumer (the
// worker). finish()/shutdown()/health() are application-edge and must not
// race on_snapshot (same contract as TcpPublisher::finish). health() merges
// relaxed atomics and is safe to call any time; exactness holds at rest.
// The sim callback performs no syscalls, no blocking, no encode: builder
// fill + bounded enqueue + a steady_clock sample for callback latency.
//
// PORTABILITY: declaration only — the worker thread runs in the POSIX TU
// (src/link/tcp_worker.cpp) beside the socket publishers; the Windows port
// replaces that TU, never this header. Not copyable, not movable (owns its
// thread); the caller owns the worker like every other publisher.

#include "wengine/core/status.hpp"
#include "wengine/emit/sink.hpp"
#include "wengine/link/output_queue.hpp"
#include "wengine/link/sample_block.hpp"
#include "wengine/world/state.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>

namespace wengine::link {

enum class TcpWorkerMode : std::uint8_t { kOrdered, kLatest };

struct TcpWorkerConfig {
  std::string endpoint; // host:port, active connect at construction
  std::string domain = "signal";
  TcpWorkerMode mode = TcpWorkerMode::kOrdered;
  sample::SampleBlockConfig block; // batching: samples-per-block/byte budget
  sample::OutputQueueConfig queue; // bounded memory: slots + bytes
};

// Point-in-time worker snapshot (see the accounting equation above).
struct TcpWorkerHealth {
  std::uint64_t attempted = 0; // on_snapshot calls + finish() flushes
  std::uint64_t disabled_after_error = 0;
  std::uint64_t published_blocks = 0;
  std::uint64_t published_samples = 0;
  std::uint64_t dropped_over_bound = 0; // builder/encode refusals (sticky)
  std::uint64_t dropped_send = 0; // latest-mode fresh-frame drops (transient)
  std::uint64_t dropped_send_samples = 0;
  std::uint64_t coalesced_blocks = 0; // latest-mode intentional skips
  std::uint64_t coalesced_samples = 0;
  std::uint64_t abandoned_blocks = 0; // owned at stop, never sent
  std::uint64_t abandoned_samples = 0;
  std::uint64_t bytes_accepted = 0; // wire bytes (prefix + records)
  std::uint64_t queued_blocks = 0; // live queue depth
  std::uint64_t queued_bytes = 0;
  std::uint64_t held_blocks = 0; // 0/1: worker holds a popped/pending block
  std::uint64_t held_samples = 0;
  std::uint64_t callback_count = 0; // sim-callback samples (steady_clock)
  std::uint64_t callback_ns_total = 0;
  std::uint64_t callback_ns_max = 0;
  bool worker_alive = false;
  bool has_pending = false; // partial wire frame held for framing safety
  std::size_t pending_bytes = 0;
  sample::OutputQueueStats queue; // slot/byte/high-water + dropped/closed detail
};

class TcpBlockWorker : public emit::ISink {
 public:
  explicit TcpBlockWorker(TcpWorkerConfig cfg);
  ~TcpBlockWorker() override;

  TcpBlockWorker(const TcpBlockWorker &) = delete;
  TcpBlockWorker &operator=(const TcpBlockWorker &) = delete;
  TcpBlockWorker(TcpBlockWorker &&) = delete;
  TcpBlockWorker &operator=(TcpBlockWorker &&) = delete;

  // Simulation thread only: fill the builder, seal full blocks straight
  // into the queue. Never encodes, never touches the socket, never blocks;
  // a full queue or a poisoned worker counts loudly and returns.
  void on_snapshot(const world::Snapshot &snapshot) override;

  // Application edge (never the sim thread, never the destructor): seal
  // the builder tail into the queue, then wait up to timeout_ms for the
  // queue + pending bytes to drain. Success iff fully drained with the
  // worker still healthy; kInvalidArgument for timeout_ms < 0; kInternal
  // on deadline expiry (queue/pending preserved for reporting, worker NOT
  // poisoned — retry or shut down explicitly) or when the worker is
  // already stopped/poisoned. timeout_ms == 0 probes once.
  [[nodiscard]] core::Status finish(int timeout_ms) noexcept;

  // Application edge + destructor: stop admission, abandon the remainder
  // with exact counts, join the worker (bounded by the poll slice) and
  // close the socket. Idempotent; safe after a failed construction.
  void shutdown() noexcept;

  [[nodiscard]] bool ok() const noexcept {
    return sticky_.load(std::memory_order_acquire) == core::StatusCode::kOk;
  }
  [[nodiscard]] core::Status status() const noexcept {
    return core::Status{sticky_.load(std::memory_order_acquire)};
  }
  [[nodiscard]] int last_errno() const noexcept {
    return last_errno_.load(std::memory_order_acquire);
  }
  [[nodiscard]] TcpWorkerHealth health() const noexcept;
  [[nodiscard]] const std::string &endpoint() const noexcept { return cfg_.endpoint; }
  [[nodiscard]] const std::string &domain() const noexcept { return cfg_.domain; }
  [[nodiscard]] TcpWorkerMode mode() const noexcept { return cfg_.mode; }
  [[nodiscard]] std::uint64_t run_id() const noexcept { return run_id_; }

 private:
  void worker_loop() noexcept;
  void note_sticky(core::StatusCode code, int err) noexcept;

  TcpWorkerConfig cfg_;
  std::uint64_t run_id_ = 0;
  // Sim thread only (same single-writer contract as every Runner sink):
  sample::SampleBlockBuilder builder_{cfg_.block};
  sample::SampleBlock sealed_;
  // SPSC bridge: sim produces, worker consumes (see output_queue.hpp).
  sample::OutputQueue queue_{cfg_.queue};
  std::thread worker_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> worker_started_{false};
  std::atomic<core::StatusCode> sticky_{core::StatusCode::kOk};
  std::atomic<int> last_errno_{0};
  // Sim-side counters (relaxed; exact at rest):
  std::atomic<std::uint64_t> attempted_{0};
  std::atomic<std::uint64_t> disabled_{0};
  std::atomic<std::uint64_t> callback_count_{0};
  std::atomic<std::uint64_t> callback_total_ns_{0};
  std::atomic<std::uint64_t> callback_max_ns_{0};
  // Worker-side counters (relaxed; exact at rest):
  std::atomic<std::uint64_t> published_blocks_{0};
  std::atomic<std::uint64_t> published_samples_{0};
  std::atomic<std::uint64_t> over_bound_{0};
  std::atomic<std::uint64_t> send_drops_{0};
  std::atomic<std::uint64_t> send_drop_samples_{0};
  std::atomic<std::uint64_t> coalesced_blocks_{0};
  std::atomic<std::uint64_t> coalesced_samples_{0};
  std::atomic<std::uint64_t> abandoned_blocks_{0};
  std::atomic<std::uint64_t> abandoned_samples_{0};
  std::atomic<std::uint64_t> bytes_accepted_{0};
  std::atomic<std::uint64_t> held_blocks_{0};
  std::atomic<std::uint64_t> held_samples_{0};
  std::atomic<std::uint64_t> pending_bytes_{0};
  int fd_ = -1; // edge + worker thread only, never the sim callback
};

} // namespace wengine::link
