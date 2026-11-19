/**
 * @file
 * Worker-backed TCP output for batched signal samples (PHYSI-46).
 * @note Opt-in emit::ISink moving TCP encoding and socket servicing off the simulation
 *       thread: the sim callback only fills/seals/enqueues owned SampleBlocks (PHYSI-44
 *       builder + PHYSI-45 queue); exactly one explicit worker thread (no pool, no knob)
 *       handles wire encoding, socket readiness and pending writes. Two modes: kOrdered
 *       (every sealed block sent; full queue reported to the driver, never hidden) and
 *       kLatest (backlog coalesced to newest, counted never silent). Shutdown is
 *       deadline-bounded with exact abandoned-block accounting. Declaration only — the worker
 *       thread runs in the POSIX TU src/link/tcp_worker.cpp; a Windows port replaces that TU,
 *       never this header. Consumers: CLI --emit-tcp worker-backed outputs, telemetry/record paths.
 */
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

/**
 * @brief Worker delivery mode, fixed at construction: kOrdered (sample mode: every sealed block sent in seal order) vs kLatest (live-view mode: backlog coalesced to newest, counted).
 * @note Ordered default. Coalesced and queue-full-dropped blocks never take a send_seq, so sent seqs stay consecutive per run.
 */
enum class TcpWorkerMode : std::uint8_t { kOrdered, kLatest };

/** @brief Worker configuration (explicit per worker, fixed at construction). */
struct TcpWorkerConfig {
/** @brief host:port text (active connect at construction). */
  std::string endpoint; // host:port, active connect at construction
/** @brief Record domain tag. */
  std::string domain = "signal";
/** @brief Delivery mode (kOrdered default). */
  TcpWorkerMode mode = TcpWorkerMode::kOrdered;
/** @brief Batching: samples-per-block / byte budget. */
  sample::SampleBlockConfig block; // batching: samples-per-block/byte budget
/** @brief Bounded memory: slots + bytes. */
  sample::OutputQueueConfig queue; // bounded memory: slots + bytes
};

// Point-in-time worker snapshot (see the accounting equation above).
/** @brief Point-in-time worker snapshot (see the header conservation equation; block units, exact at rest). */
struct TcpWorkerHealth {
/** @brief on_snapshot calls + finish() flushes (diagnostic). */
  std::uint64_t attempted = 0; // on_snapshot calls + finish() flushes
/** @brief Snapshots ignored once poisoned. */
  std::uint64_t disabled_after_error = 0;
/** @brief Blocks accepted for sending (send_seq counts seals accepted, strictly increasing per run). */
  std::uint64_t published_blocks = 0;
/** @brief Samples accepted for sending. */
  std::uint64_t published_samples = 0;
/** @brief Builder/encode refusals (sticky; poison the worker loudly, never become blocks). */
  std::uint64_t dropped_over_bound = 0; // builder/encode refusals (sticky)
/** @brief Latest-mode fresh-frame drops (transient). */
  std::uint64_t dropped_send = 0; // latest-mode fresh-frame drops (transient)
/** @brief Latest-mode dropped samples. */
  std::uint64_t dropped_send_samples = 0;
/** @brief Latest-mode intentional skips (never silent; reconcile like StrideSink skips). */
  std::uint64_t coalesced_blocks = 0; // latest-mode intentional skips
/** @brief Latest-mode intentionally skipped samples. */
  std::uint64_t coalesced_samples = 0;
/** @brief Blocks owned at stop but never sent. */
  std::uint64_t abandoned_blocks = 0; // owned at stop, never sent
/** @brief Samples owned at stop but never sent. */
  std::uint64_t abandoned_samples = 0;
/** @brief Wire bytes accepted locally (prefix + records; local acceptance only). */
  std::uint64_t bytes_accepted = 0; // wire bytes (prefix + records)
/** @brief Live queue depth in blocks. */
  std::uint64_t queued_blocks = 0; // live queue depth
/** @brief Live queued bytes. */
  std::uint64_t queued_bytes = 0;
/** @brief 0/1: worker holds a popped/pending block. */
  std::uint64_t held_blocks = 0; // 0/1: worker holds a popped/pending block
/** @brief Held-block sample count. */
  std::uint64_t held_samples = 0;
/** @brief Sim-callback samples (steady_clock). */
  std::uint64_t callback_count = 0; // sim-callback samples (steady_clock)
/** @brief Total sim-callback nanoseconds (sub-microsecond target: the callback never encodes or touches the socket). */
  std::uint64_t callback_ns_total = 0;
/** @brief Max sim-callback nanoseconds. */
  std::uint64_t callback_ns_max = 0;
/** @brief Worker thread liveness. */
  bool worker_alive = false;
/** @brief Partial wire frame held for framing safety. */
  bool has_pending = false; // partial wire frame held for framing safety
/** @brief Held partial-frame byte count. */
  std::size_t pending_bytes = 0;
/** @brief Queue detail (slot/byte/high-water + dropped/closed). */
  sample::OutputQueueStats queue; // slot/byte/high-water + dropped/closed detail
};

/**
 * @brief Worker-backed TCP block publisher (one producer on_snapshot + one consumer worker thread).
 * @note The sim callback performs no syscalls, no blocking, no encode: builder fill + bounded
 *       enqueue + a steady_clock sample. finish()/shutdown()/health() are application-edge and
 *       must not race on_snapshot (same contract as TcpPublisher::finish). health() merges relaxed
 *       atomics and is safe to call any time; exactness holds at rest. Not copyable, not movable
 *       (owns its thread); the caller owns the worker like every other publisher.
 */
class TcpBlockWorker : public emit::ISink {
 public:
/**
 * @brief Build the worker (endpoint active-connect happens on the worker thread per the TU).
 * @param[in] cfg endpoint, domain, mode, block batching, and queue budgets (moved in).
 */
  explicit TcpBlockWorker(TcpWorkerConfig cfg);
/** @brief Shut down the same way as shutdown() (never blocks on the network). */
  ~TcpBlockWorker() override;

/** @brief Non-copyable (owns its thread). */
  TcpBlockWorker(const TcpBlockWorker &) = delete;
/** @brief Non-copyable (owns its thread). */
  TcpBlockWorker &operator=(const TcpBlockWorker &) = delete;
/** @brief Non-movable (owns its thread). */
  TcpBlockWorker(TcpBlockWorker &&) = delete;
/** @brief Non-movable (owns its thread). */
  TcpBlockWorker &operator=(TcpBlockWorker &&) = delete;

  // Simulation thread only: fill the builder, seal full blocks straight
  // into the queue. Never encodes, never touches the socket, never blocks;
  // a full queue or a poisoned worker counts loudly and returns.
/**
 * @brief Simulation thread only: fill the builder, seal full blocks straight into the queue.
 * @param[in] snapshot Runner-owned emission buffer (observed synchronously; the queue copies sealed blocks into owned slots).
 * @note Never encodes, never touches the socket, never blocks; a full queue or a poisoned worker counts loudly and returns.
 */
  void on_snapshot(const world::Snapshot &snapshot) override;

  // Application edge (never the sim thread, never the destructor): seal
  // the builder tail into the queue, then wait up to timeout_ms for the
  // queue + pending bytes to drain. Success iff fully drained with the
  // worker still healthy; kInvalidArgument for timeout_ms < 0; kInternal
  // on deadline expiry (queue/pending preserved for reporting, worker NOT
  // poisoned — retry or shut down explicitly) or when the worker is
  // already stopped/poisoned. timeout_ms == 0 probes once.
/**
 * @brief Application edge (never the sim thread, never the destructor): seal the builder tail into the queue, then wait up to the deadline for the queue + pending bytes to drain.
 * @param[in] timeout_ms wait budget in ms (0 probes once; negative is kInvalidArgument).
 * @return kOk iff fully drained with the worker still healthy; kInvalidArgument for timeout_ms < 0; kInternal on deadline expiry (queue/pending preserved for reporting, worker NOT poisoned — retry or shut down explicitly) or when the worker is already stopped/poisoned.
 */
  [[nodiscard]] core::Status finish(int timeout_ms) noexcept;

  // Application edge + destructor: stop admission, abandon the remainder
  // with exact counts, join the worker (bounded by the poll slice) and
  // close the socket. Idempotent; safe after a failed construction.
/** @brief Application edge + destructor: stop admission, abandon the remainder with exact counts, join the worker (bounded by the poll slice) and close the socket. Idempotent; safe after a failed construction. */
  void shutdown() noexcept;

/** @brief Success predicate. @return True while no sticky failure latched. */
  [[nodiscard]] bool ok() const noexcept {
    return sticky_.load(std::memory_order_acquire) == core::StatusCode::kOk;
  }
/** @brief Latched sticky status. @return kOk, or the first sticky failure code. */
  [[nodiscard]] core::Status status() const noexcept {
    return core::Status{sticky_.load(std::memory_order_acquire)};
  }
/** @brief Errno captured at the failing syscall (0 when ok). @return Captured errno value. */
  [[nodiscard]] int last_errno() const noexcept {
    return last_errno_.load(std::memory_order_acquire);
  }
/** @brief Point-in-time worker snapshot (merges relaxed atomics; exact at rest). @return Health counters. */
  [[nodiscard]] TcpWorkerHealth health() const noexcept;
/** @brief Configured host:port endpoint text. @return Endpoint string. */
  [[nodiscard]] const std::string &endpoint() const noexcept { return cfg_.endpoint; }
/** @brief Configured record domain tag. @return Domain string. */
  [[nodiscard]] const std::string &domain() const noexcept { return cfg_.domain; }
/** @brief Configured delivery mode. @return Worker mode. */
  [[nodiscard]] TcpWorkerMode mode() const noexcept { return cfg_.mode; }
/** @brief Publisher-run identity (opaque; PHYSI-40 session identity). @return Run id value. */
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
