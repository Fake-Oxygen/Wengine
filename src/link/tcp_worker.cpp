// POSIX worker thread + socket servicing for the PHYSI-46 block worker:
// connect/hello around the platform-free framing in tcp_worker.hpp, the
// single worker loop (encode + readiness + pending writes), and bounded
// shutdown. Same justified exception to the header-only engine rule as the
// other link endpoint TUs (platform syscalls cannot live in headers): the
// public header stays platform-free, the Windows port replaces this file.
//
// The simulation callback (TcpBlockWorker::on_snapshot) never runs here —
// it only fills/seals/enqueues in the header-inline path. All blocking
// waits below (select slices, finish polling, join) belong to the worker
// or application threads, never the sim loop.

#if defined(_WIN32)
#error "wengine-link TCP worker is POSIX-only. The Windows port replaces this TU — see tcp_worker.hpp."
#endif

#include "wengine/link/tcp_worker.hpp"

#include "socket_common.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <thread>
#include <utility>
#include <vector>

namespace wengine::link {

namespace {

// Worker poll slices: idle queue polling (delivery latency) and socket
// readiness waits (shutdown latency). Both bound worker join time without
// ever touching the sim thread.
inline constexpr int kIdlePollMs = 1;
inline constexpr int kSelectSliceMs = 50;

// Hello send budget at construction (edge time): the peer is loopback or
// freshly accepted; a backpressured handshake is a failed construction,
// never a pending frame.
inline constexpr std::size_t kHelloBytes = sizeof(TcpHello);

// Short sleep via select (no fd sets): worker/app threads only.
void sleep_ms(int ms) noexcept {
  timeval tv{};
  tv.tv_sec = ms / 1000;
  tv.tv_usec = (ms % 1000) * 1000;
  (void)::select(0, nullptr, nullptr, nullptr, &tv);
}

// True when fd is writable within timeout_ms (false on timeout, EINTR, or
// select error — the following send reports the verdict either way).
bool wait_writable(int fd, int timeout_ms) noexcept {
  fd_set wfds{};
  FD_ZERO(&wfds);
  FD_SET(fd, &wfds);
  timeval tv{};
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  return ::select(fd + 1, nullptr, &wfds, nullptr, &tv) > 0;
}

} // namespace

TcpBlockWorker::TcpBlockWorker(TcpWorkerConfig cfg) : cfg_(std::move(cfg)) {
  if (cfg_.domain.empty() || cfg_.domain.size() >= kDomainSize) {
    sticky_.store(core::StatusCode::kInvalidArgument, std::memory_order_release);
    return;
  }
  const auto ep = parse_endpoint(cfg_.endpoint);
  if (!ep.ok()) {
    sticky_.store(core::StatusCode::kInvalidArgument, std::memory_order_release);
    return;
  }
  run_id_ = socket_internal::make_run_id();
  int err = 0;
  fd_ = socket_internal::open_connected(ep.value, SOCK_STREAM,
                                        socket_internal::kConnectTimeoutMs, err);
  if (fd_ < 0) {
    last_errno_.store(err, std::memory_order_release);
    sticky_.store(core::StatusCode::kInternal, std::memory_order_release);
    return;
  }
  socket_internal::tune_send_buffer(fd_);
  TcpHello hello_msg{};
  hello_msg.magic = kTcpMagic;
  hello_msg.version = kTcpVersion;
  for (std::size_t i = 0; i < cfg_.domain.size(); ++i) {
    hello_msg.domain[i] = cfg_.domain[i];
  }
  unsigned char hello[kHelloBytes]{};
  std::memcpy(hello, &hello_msg, sizeof(hello_msg));
  std::size_t sent = 0;
  const socket_internal::SendResult r =
      socket_internal::send_all(fd_, hello, sizeof(hello), sent, err);
  if (r != socket_internal::SendResult::kComplete) {
    last_errno_.store(err, std::memory_order_release);
    sticky_.store(core::StatusCode::kInternal, std::memory_order_release);
    ::close(fd_);
    fd_ = -1;
    return;
  }
  try {
    worker_started_.store(true, std::memory_order_release);
    worker_ = std::thread([this] { worker_loop(); });
  } catch (...) {
    worker_started_.store(false, std::memory_order_release);
    sticky_.store(core::StatusCode::kInternal, std::memory_order_release);
    ::close(fd_);
    fd_ = -1;
  }
}

TcpBlockWorker::~TcpBlockWorker() { shutdown(); }

void TcpBlockWorker::on_snapshot(const world::Snapshot &snapshot) {
  const auto t0 = std::chrono::steady_clock::now();
  attempted_.fetch_add(1, std::memory_order_relaxed);
  if (sticky_.load(std::memory_order_acquire) != core::StatusCode::kOk) {
    disabled_.fetch_add(1, std::memory_order_relaxed);
  } else {
    bool has_sealed = false;
    const core::Status bst = builder_.push(snapshot, sealed_, has_sealed);
    if (!bst.ok()) {
      // Malformed tick (channel bound / unknown quantity): loud like an
      // encode refusal — sticky, counted, never half-queued.
      over_bound_.fetch_add(1, std::memory_order_relaxed);
      note_sticky(bst.code, 0);
    } else if (has_sealed) {
      // Queue-full/closed rejections are counted inside the queue itself
      // (dropped_*/closed_*) and surface through health().queue.
      (void)queue_.try_push(std::move(sealed_));
    }
  }
  const auto t1 = std::chrono::steady_clock::now();
  const std::uint64_t ns = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
  callback_count_.fetch_add(1, std::memory_order_relaxed);
  callback_total_ns_.fetch_add(ns, std::memory_order_relaxed);
  const std::uint64_t prev_max = callback_max_ns_.load(std::memory_order_relaxed);
  if (ns > prev_max) {
    callback_max_ns_.store(ns, std::memory_order_relaxed);
  }
}

core::Status TcpBlockWorker::finish(int timeout_ms) noexcept {
  if (timeout_ms < 0) {
    return {core::StatusCode::kInvalidArgument};
  }
  if (sticky_.load(std::memory_order_acquire) != core::StatusCode::kOk) {
    return status();
  }
  if (stop_.load(std::memory_order_acquire)) {
    return {core::StatusCode::kInternal}; // stopped: drain via shutdown accounting
  }
  // Seal the builder tail so the final partial block is delivered, not
  // stranded. A full queue here is loud (queue.dropped_*) like any push.
  sample::SampleBlock tail;
  bool has_tail = false;
  if (!builder_.flush(tail, has_tail).ok()) {
    over_bound_.fetch_add(1, std::memory_order_relaxed);
    note_sticky(core::StatusCode::kInternal, 0);
    return status();
  }
  if (has_tail) {
    attempted_.fetch_add(1, std::memory_order_relaxed);
    (void)queue_.try_push(std::move(tail));
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  for (;;) {
    if (sticky_.load(std::memory_order_acquire) != core::StatusCode::kOk) {
      return status();
    }
    if (queue_.empty() && held_blocks_.load(std::memory_order_acquire) == 0 &&
        pending_bytes_.load(std::memory_order_acquire) == 0) {
      return core::Status::success();
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return {core::StatusCode::kInternal}; // deadline: NOT sticky, retry or shut down
    }
    sleep_ms(kIdlePollMs);
  }
}

void TcpBlockWorker::shutdown() noexcept {
  if (stop_.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  queue_.shutdown(); // further on_snapshot calls refuse kClosed, counted
  if (worker_.joinable()) {
    worker_.join(); // bounded: the loop only ever waits in short slices
  }
  // Remainder accounting: everything still owned becomes abandoned with
  // exact block/sample counts; gauges return to zero.
  sample::SampleBlock rest;
  std::uint64_t ab = 0;
  std::uint64_t abs = 0;
  while (queue_.try_pop(rest)) {
    ++ab;
    abs += static_cast<std::uint64_t>(rest.steps.size());
  }
  ab += held_blocks_.exchange(0, std::memory_order_acq_rel);
  abs += held_samples_.exchange(0, std::memory_order_acq_rel);
  pending_bytes_.store(0, std::memory_order_release);
  abandoned_blocks_.fetch_add(ab, std::memory_order_relaxed);
  abandoned_samples_.fetch_add(abs, std::memory_order_relaxed);
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

TcpWorkerHealth TcpBlockWorker::health() const noexcept {
  TcpWorkerHealth h;
  h.attempted = attempted_.load(std::memory_order_relaxed);
  h.disabled_after_error = disabled_.load(std::memory_order_relaxed);
  h.published_blocks = published_blocks_.load(std::memory_order_relaxed);
  h.published_samples = published_samples_.load(std::memory_order_relaxed);
  h.dropped_over_bound = over_bound_.load(std::memory_order_relaxed);
  h.dropped_send = send_drops_.load(std::memory_order_relaxed);
  h.dropped_send_samples = send_drop_samples_.load(std::memory_order_relaxed);
  h.coalesced_blocks = coalesced_blocks_.load(std::memory_order_relaxed);
  h.coalesced_samples = coalesced_samples_.load(std::memory_order_relaxed);
  h.abandoned_blocks = abandoned_blocks_.load(std::memory_order_relaxed);
  h.abandoned_samples = abandoned_samples_.load(std::memory_order_relaxed);
  h.bytes_accepted = bytes_accepted_.load(std::memory_order_relaxed);
  h.queued_blocks = static_cast<std::uint64_t>(queue_.size());
  h.queued_bytes = static_cast<std::uint64_t>(queue_.bytes_queued());
  h.held_blocks = held_blocks_.load(std::memory_order_relaxed);
  h.held_samples = held_samples_.load(std::memory_order_relaxed);
  h.callback_count = callback_count_.load(std::memory_order_relaxed);
  h.callback_ns_total = callback_total_ns_.load(std::memory_order_relaxed);
  h.callback_ns_max = callback_max_ns_.load(std::memory_order_relaxed);
  h.worker_alive =
      worker_started_.load(std::memory_order_acquire) && !stop_.load(std::memory_order_acquire);
  const std::uint64_t pending = pending_bytes_.load(std::memory_order_acquire);
  h.has_pending = pending != 0;
  h.pending_bytes = static_cast<std::size_t>(pending);
  h.queue = queue_.stats();
  return h;
}

void TcpBlockWorker::note_sticky(core::StatusCode code, int err) noexcept {
  core::StatusCode expect = core::StatusCode::kOk;
  if (sticky_.compare_exchange_strong(expect, code, std::memory_order_acq_rel)) {
    last_errno_.store(err, std::memory_order_release);
  }
}

void TcpBlockWorker::worker_loop() noexcept {
  // Worker-owned scratch: one in-flight block + one wire buffer (length
  // prefix + one maximum-size record). Reused for the worker lifetime.
  sample::SampleBlock block;
  std::vector<unsigned char> wire(4 + sample::kBlockMaxBytes, 0);
  std::uint64_t send_seq = 0;
  std::size_t pending_off = 0;
  std::size_t pending_total = 0;
  std::uint64_t pending_samples = 0;

  while (!stop_.load(std::memory_order_acquire)) {
    if (sticky_.load(std::memory_order_acquire) != core::StatusCode::kOk) {
      return; // poisoned: remainder is abandoned at shutdown, counted there
    }
    // Depth-1 pending discipline first: a partial wire frame must complete
    // before any later bytes enter the stream, or framing corrupts.
    if (pending_total != 0) {
      if (fd_ < 0) {
        note_sticky(core::StatusCode::kInternal, EBADF);
        return;
      }
      if (!wait_writable(fd_, kSelectSliceMs)) {
        continue; // slice elapsed: recheck stop/sticky, never hang on a peer
      }
      int err = 0;
      std::size_t sent = 0;
      const socket_internal::SendResult r = socket_internal::send_all(
          fd_, wire.data() + pending_off, pending_total - pending_off, sent, err);
      pending_off += sent;
      pending_bytes_.store(pending_total - pending_off, std::memory_order_release);
      if (r == socket_internal::SendResult::kComplete) {
        published_blocks_.fetch_add(1, std::memory_order_relaxed);
        published_samples_.fetch_add(pending_samples, std::memory_order_relaxed);
        bytes_accepted_.fetch_add(static_cast<std::uint64_t>(pending_total),
                                  std::memory_order_relaxed);
        pending_off = 0;
        pending_total = 0;
        pending_samples = 0;
        held_blocks_.store(0, std::memory_order_release);
        held_samples_.store(0, std::memory_order_release);
        continue;
      }
      if (r == socket_internal::SendResult::kDead) {
        note_sticky(core::StatusCode::kInternal, err);
        return;
      }
      continue; // transient backpressure: pending preserved, no drop counted
    }
    // Next block: latest mode drains a backlog to the newest (explicit
    // coalescing, counted); ordered mode takes the head and preserves it.
    bool have = false;
    if (cfg_.mode == TcpWorkerMode::kLatest) {
      sample::SampleBlock newest;
      bool any = false;
      while (queue_.try_pop(newest)) {
        if (any) {
          coalesced_blocks_.fetch_add(1, std::memory_order_relaxed);
          coalesced_samples_.fetch_add(static_cast<std::uint64_t>(block.steps.size()),
                                       std::memory_order_relaxed);
        }
        block = std::move(newest);
        any = true;
      }
      have = any;
    } else {
      have = queue_.try_pop(block);
    }
    if (!have) {
      sleep_ms(kIdlePollMs);
      continue;
    }
    const std::uint64_t samples = static_cast<std::uint64_t>(block.steps.size());
    held_blocks_.store(1, std::memory_order_release);
    held_samples_.store(samples, std::memory_order_release);
    std::size_t rec_len = 0;
    const core::Status est =
        sample::encode_block(block, cfg_.domain, run_id_, send_seq + 1, wire.data() + 4,
                             wire.size() - 4, rec_len);
    if (!est.ok()) {
      // Builder-sealed blocks always fit; a foreign over-bound push is
      // loud and sticky like every transport's encode refusal.
      over_bound_.fetch_add(1, std::memory_order_relaxed);
      note_sticky(core::StatusCode::kOutOfRange, 0);
      return; // held_* gauges preserved for abandon accounting
    }
    socket_detail::put_u32_le(wire.data(), static_cast<std::uint32_t>(rec_len));
    const std::size_t total = 4 + rec_len;
    int err = 0;
    std::size_t sent = 0;
    const socket_internal::SendResult r =
        socket_internal::send_all(fd_, wire.data(), total, sent, err);
    if (r == socket_internal::SendResult::kComplete) {
      ++send_seq;
      published_blocks_.fetch_add(1, std::memory_order_relaxed);
      published_samples_.fetch_add(samples, std::memory_order_relaxed);
      bytes_accepted_.fetch_add(static_cast<std::uint64_t>(total), std::memory_order_relaxed);
      held_blocks_.store(0, std::memory_order_release);
      held_samples_.store(0, std::memory_order_release);
      continue;
    }
    if (r == socket_internal::SendResult::kDead) {
      note_sticky(core::StatusCode::kInternal, err);
      return; // held_* gauges preserved for abandon accounting
    }
    if (sent == 0 && cfg_.mode == TcpWorkerMode::kLatest) {
      // Nothing entered the stream: the fresh block may go loudly while a
      // newer one is already queuing (transient, counted, never sticky).
      send_drops_.fetch_add(1, std::memory_order_relaxed);
      send_drop_samples_.fetch_add(samples, std::memory_order_relaxed);
      held_blocks_.store(0, std::memory_order_release);
      held_samples_.store(0, std::memory_order_release);
      continue;
    }
    // Partial (or ordered zero-byte) send: hold the tail — discarding it
    // would corrupt stream framing.
    ++send_seq;
    pending_off = sent;
    pending_total = total;
    pending_samples = samples;
    pending_bytes_.store(total - sent, std::memory_order_release);
  }
}

} // namespace wengine::link
