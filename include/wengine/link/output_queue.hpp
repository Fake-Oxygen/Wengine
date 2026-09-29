#pragma once

// Bounded owning SPSC output queue (PHYSI-45): fixed-capacity reusable
// storage for owned SampleBlock values (PHYSI-44) between a simulation
// producer and one async consumer. Storage/ownership only — worker threads
// and endpoint wiring are separate tasks.
//
// WHY: Runner calls every ISink synchronously with a stack-local Snapshot
// reference that no async observer may retain. This queue is the ownership
// bridge: the producer copies a sealed SampleBlock in through a bounded
// try_push, the consumer reads it while it is queue-owned (front) and
// returns the slot by popping. An unbounded queue would only turn frame
// loss into growing latency and memory use, so capacity is fixed in slots
// AND bytes, and overflow is loud, never silent.
//
// LIFETIME (from docs/delivery.md §4 ownership):
// - try_push copies the block into queue-owned slot storage; the caller
//   keeps its block and may mutate/reuse it immediately — queued data is
//   immutable and unaffected (enqueue-then-mutate the source freely).
// - front() yields a const view into queue-owned storage, valid until the
//   matching pop()/try_pop(). The consumer must not retain it past that;
//   a retaining consumer copies out (try_pop) before returning.
// - pop()/try_pop() returns the slot to the producer for reuse. Slot
//   vector capacities are retained, so steady same-shape traffic performs
//   no per-block allocation after warmup (first fill of each slot).
// - shutdown() stops admission only: queued blocks stay readable and
//   drainable to empty; done() == closed && empty.
//
// OVERFLOW (from docs/delivery.md §5 counter vocabulary):
// - try_push never blocks and never overwrites a block the consumer may
//   be reading: when all slots are outstanding OR the byte budget would be
//   exceeded it returns kFull and counts the rejected block + samples
//   (maps to PublisherStats::dropped_queue_full).
// - After shutdown() try_push returns kClosed and counts separately
//   (caller-side disabled, akin to disabled_after_error).
// - Counters are monotonic; exactness holds at quiescent points (no
//   concurrent push/pop in flight), like PublisherStats accounting.
//
// THREADING: exactly one producer thread (try_push) and one consumer
// thread (front/pop/try_pop). Slot hand-off uses release/acquire pairs on
// head_/tail_; statistics are relaxed atomics (exact at rest). Anything
// else — multiple producers/consumers, or stats() racing live updates —
// needs external sync. No syscalls, no I/O. The consumer path is
// non-allocating and noexcept; try_push allocates only while slot
// capacities warm up.

#include "wengine/link/sample_block.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace wengine::link::sample {

// Default slot count mirrors the SHM ring depth: bounded jitter cover, not
// a completeness promise (see docs/delivery.md classes A/B).
inline constexpr std::size_t kOutputQueueDefaultSlots = 8;
// Default byte budget holds one maximum-size block per default slot.
inline constexpr std::size_t kOutputQueueDefaultBytes =
    kBlockMaxBytes * kOutputQueueDefaultSlots;

struct OutputQueueConfig {
  std::size_t max_slots = kOutputQueueDefaultSlots;
  std::size_t max_bytes = kOutputQueueDefaultBytes;
};

enum class PushResult : std::uint8_t { kOk, kFull, kClosed };

// Plain snapshot of monotonic queue counters (see OutputQueue::stats()).
struct OutputQueueStats {
  std::uint64_t pushed_blocks = 0;
  std::uint64_t pushed_samples = 0;
  std::uint64_t popped_blocks = 0;
  std::uint64_t popped_samples = 0;
  std::uint64_t dropped_blocks = 0;  // kFull refusals (slot or byte budget)
  std::uint64_t dropped_samples = 0;
  std::uint64_t closed_blocks = 0;  // kClosed refusals (post-shutdown)
  std::uint64_t closed_samples = 0;
  std::uint64_t high_water_slots = 0;  // max ready slots observed
  std::uint64_t high_water_bytes = 0;  // max queued bytes observed

  bool operator==(const OutputQueueStats &) const noexcept = default;
};

class OutputQueue {
 public:
  explicit OutputQueue(OutputQueueConfig cfg = {})
      : cfg_(sanitize(cfg)), slots_(cfg_.max_slots), slot_bytes_(cfg_.max_slots, 0) {}

  OutputQueue(const OutputQueue &) = delete;
  OutputQueue &operator=(const OutputQueue &) = delete;
  OutputQueue(OutputQueue &&) = delete;
  OutputQueue &operator=(OutputQueue &&) = delete;

  [[nodiscard]] const OutputQueueConfig &config() const noexcept { return cfg_; }
  [[nodiscard]] std::size_t capacity() const noexcept { return cfg_.max_slots; }
  [[nodiscard]] std::size_t byte_capacity() const noexcept { return cfg_.max_bytes; }

  // Producer: copy one owned block into the next free slot. kOk owns the
  // copy from here; kFull (slots exhausted or byte budget exceeded) and
  // kClosed (post-shutdown) leave the queue untouched and count the
  // rejected block + its samples. Never blocks, never overwrites.
  PushResult try_push(const SampleBlock &in) {
    if (closed_.load(std::memory_order_acquire)) {
      closed_blocks_.fetch_add(1, std::memory_order_relaxed);
      closed_samples_.fetch_add(sample_count_of(in), std::memory_order_relaxed);
      return PushResult::kClosed;
    }
    const std::size_t head = head_.load(std::memory_order_relaxed);
    const std::size_t tail = tail_.load(std::memory_order_acquire);
    if (head - tail >= cfg_.max_slots) {
      dropped_blocks_.fetch_add(1, std::memory_order_relaxed);
      dropped_samples_.fetch_add(sample_count_of(in), std::memory_order_relaxed);
      return PushResult::kFull;
    }
    const std::size_t need = block_bytes(in);
    const std::size_t queued = bytes_.load(std::memory_order_acquire);
    if (queued + need > cfg_.max_bytes) {
      dropped_blocks_.fetch_add(1, std::memory_order_relaxed);
      dropped_samples_.fetch_add(sample_count_of(in), std::memory_order_relaxed);
      return PushResult::kFull;
    }
    const std::size_t idx = head % cfg_.max_slots;
    slots_[idx] = in;  // copy-assign reuses warmed slot capacity
    slot_bytes_[idx] = need;
    bytes_.fetch_add(need, std::memory_order_release);
    // Release: publishes the slot stores above to the consumer.
    head_.store(head + 1, std::memory_order_release);
    pushed_blocks_.fetch_add(1, std::memory_order_relaxed);
    pushed_samples_.fetch_add(sample_count_of(in), std::memory_order_relaxed);
    note_high_water(head - tail + 1, queued + need);
    return PushResult::kOk;
  }

  // Producer: hand a block over by move. Same bounds and counting as the
  // copy form; steady same-shape traffic stays allocation-free either way
  // (capacities swap rather than grow).
  PushResult try_push(SampleBlock &&in) {
    if (closed_.load(std::memory_order_acquire)) {
      closed_blocks_.fetch_add(1, std::memory_order_relaxed);
      closed_samples_.fetch_add(sample_count_of(in), std::memory_order_relaxed);
      return PushResult::kClosed;
    }
    const std::size_t head = head_.load(std::memory_order_relaxed);
    const std::size_t tail = tail_.load(std::memory_order_acquire);
    if (head - tail >= cfg_.max_slots) {
      dropped_blocks_.fetch_add(1, std::memory_order_relaxed);
      dropped_samples_.fetch_add(sample_count_of(in), std::memory_order_relaxed);
      return PushResult::kFull;
    }
    const std::size_t need = block_bytes(in);
    const std::size_t queued = bytes_.load(std::memory_order_acquire);
    if (queued + need > cfg_.max_bytes) {
      dropped_blocks_.fetch_add(1, std::memory_order_relaxed);
      dropped_samples_.fetch_add(sample_count_of(in), std::memory_order_relaxed);
      return PushResult::kFull;
    }
    const std::uint64_t samples = sample_count_of(in);
    const std::size_t idx = head % cfg_.max_slots;
    slots_[idx] = std::move(in);
    slot_bytes_[idx] = need;
    bytes_.fetch_add(need, std::memory_order_release);
    head_.store(head + 1, std::memory_order_release);
    pushed_blocks_.fetch_add(1, std::memory_order_relaxed);
    pushed_samples_.fetch_add(samples, std::memory_order_relaxed);
    note_high_water(head - tail + 1, queued + need);
    return PushResult::kOk;
  }

  // Consumer: oldest ready block, or nullptr when empty. The view is valid
  // until the matching pop()/try_pop() and must not be retained past it.
  [[nodiscard]] const SampleBlock *front() const noexcept {
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    // Acquire pairs with the producer's head_ release: a visible slot is a
    // fully stored slot.
    const std::size_t head = head_.load(std::memory_order_acquire);
    if (tail == head) {
      return nullptr;
    }
    return &slots_[tail % cfg_.max_slots];
  }

  // Consumer: release the front slot for producer reuse. No-op on empty
  // (never UB); the normal contract is front() != nullptr.
  void pop() noexcept {
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    const std::size_t head = head_.load(std::memory_order_acquire);
    if (tail == head) {
      return;
    }
    const std::size_t idx = tail % cfg_.max_slots;
    popped_samples_.fetch_add(sample_count_of(slots_[idx]), std::memory_order_relaxed);
    popped_blocks_.fetch_add(1, std::memory_order_relaxed);
    bytes_.fetch_sub(slot_bytes_[idx], std::memory_order_release);
    slot_bytes_[idx] = 0;
    // Release: returns the slot to the producer; the producer's acquire
    // load of tail_ observes a fully released slot.
    tail_.store(tail + 1, std::memory_order_release);
  }

  // Consumer: copy the front block into caller-owned storage (reusing its
  // capacity) and release the slot. False when empty with `out` untouched.
  bool try_pop(SampleBlock &out) {
    const SampleBlock *head_block = front();
    if (head_block == nullptr) {
      return false;
    }
    out = *head_block;
    pop();
    return true;
  }

  // Admission stop: later try_push reports kClosed. Queued blocks stay
  // readable and drainable; idempotent.
  void shutdown() noexcept { closed_.store(true, std::memory_order_release); }

  [[nodiscard]] bool closed() const noexcept {
    return closed_.load(std::memory_order_acquire);
  }

  // Ready-block count. Exact at rest; may shift under live traffic.
  [[nodiscard]] std::size_t size() const noexcept {
    const std::size_t head = head_.load(std::memory_order_acquire);
    const std::size_t tail = tail_.load(std::memory_order_acquire);
    return head - tail;
  }

  [[nodiscard]] bool empty() const noexcept { return size() == 0; }

  [[nodiscard]] std::size_t bytes_queued() const noexcept {
    return bytes_.load(std::memory_order_acquire);
  }

  // Monotonic counters. Exact at rest (no push/pop in flight); a snapshot
  // taken during traffic may mix epochs.
  [[nodiscard]] OutputQueueStats stats() const noexcept {
    OutputQueueStats s;
    s.pushed_blocks = pushed_blocks_.load(std::memory_order_relaxed);
    s.pushed_samples = pushed_samples_.load(std::memory_order_relaxed);
    s.popped_blocks = popped_blocks_.load(std::memory_order_relaxed);
    s.popped_samples = popped_samples_.load(std::memory_order_relaxed);
    s.dropped_blocks = dropped_blocks_.load(std::memory_order_relaxed);
    s.dropped_samples = dropped_samples_.load(std::memory_order_relaxed);
    s.closed_blocks = closed_blocks_.load(std::memory_order_relaxed);
    s.closed_samples = closed_samples_.load(std::memory_order_relaxed);
    s.high_water_slots = high_water_slots_.load(std::memory_order_relaxed);
    s.high_water_bytes = high_water_bytes_.load(std::memory_order_relaxed);
    return s;
  }

  // Closed and drained: the worker shutdown quiescence point.
  [[nodiscard]] bool done() const noexcept { return closed() && empty(); }

 private:
  static OutputQueueConfig sanitize(OutputQueueConfig cfg) noexcept {
    if (cfg.max_slots == 0) {
      cfg.max_slots = kOutputQueueDefaultSlots;
    }
    if (cfg.max_bytes == 0) {
      cfg.max_bytes = kOutputQueueDefaultBytes;
    }
    return cfg;
  }

  // Queued-byte footprint of one block: same shape as encoded_size() but
  // without the transport bound, so over-bound blocks still account.
  [[nodiscard]] static std::size_t block_bytes(const SampleBlock &b) noexcept {
    const std::size_t channels = b.serials.size();
    return kBlockHeaderSize + channels * kChannelEntrySize +
           b.steps.size() * (kSampleOverhead + channels * sizeof(double));
  }

  [[nodiscard]] static std::uint64_t sample_count_of(const SampleBlock &b) noexcept {
    return static_cast<std::uint64_t>(b.steps.size());
  }

  // Producer-only (single writer): plain max update, no lost writes.
  void note_high_water(std::size_t ready, std::size_t queued_bytes) noexcept {
    const std::uint64_t ready_count = static_cast<std::uint64_t>(ready);
    if (ready_count > high_water_slots_.load(std::memory_order_relaxed)) {
      high_water_slots_.store(ready_count, std::memory_order_relaxed);
    }
    const std::uint64_t queued_count = static_cast<std::uint64_t>(queued_bytes);
    if (queued_count > high_water_bytes_.load(std::memory_order_relaxed)) {
      high_water_bytes_.store(queued_count, std::memory_order_relaxed);
    }
  }

  OutputQueueConfig cfg_;
  std::vector<SampleBlock> slots_;  // ring: logical slot k lives at k % max_slots
  std::vector<std::size_t> slot_bytes_;  // per-slot footprint, same indexing
  // Monotonic SPSC cursors: head_ published blocks (producer), tail_
  // released blocks (consumer). Ring occupancy is head_ - tail_.
  mutable std::atomic<std::size_t> head_{0};
  mutable std::atomic<std::size_t> tail_{0};
  mutable std::atomic<std::size_t> bytes_{0};
  mutable std::atomic<bool> closed_{false};
  mutable std::atomic<std::uint64_t> pushed_blocks_{0};
  mutable std::atomic<std::uint64_t> pushed_samples_{0};
  mutable std::atomic<std::uint64_t> popped_blocks_{0};
  mutable std::atomic<std::uint64_t> popped_samples_{0};
  mutable std::atomic<std::uint64_t> dropped_blocks_{0};
  mutable std::atomic<std::uint64_t> dropped_samples_{0};
  mutable std::atomic<std::uint64_t> closed_blocks_{0};
  mutable std::atomic<std::uint64_t> closed_samples_{0};
  mutable std::atomic<std::uint64_t> high_water_slots_{0};
  mutable std::atomic<std::uint64_t> high_water_bytes_{0};
};

}  // namespace wengine::link::sample
