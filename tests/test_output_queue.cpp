// Bounded owning SPSC output queue (PHYSI-45): FIFO/wraparound, explicit
// full/byte-budget/closed refusal with rejected-block/sample counts,
// shutdown drain, post-enqueue immutability, reusable slot storage, and
// threaded producer/consumer cover for race detection (run under clang
// TSAN; GCC libtsan cannot start on this host).
//
// Values are assigned literally and only copied, so round-trips assert
// == throughout. Drop counts under live threads are schedule-dependent:
// threaded tests assert exact accounting invariants and FIFO order, never
// an exact drop count.

#include "wengine/link/output_queue.hpp"
#include "wengine/link/sample_block.hpp"
#include "wengine/world/state.hpp"

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

using namespace wengine;

namespace {

// Deterministic block: `n_samples` consecutive steps from `first_step`,
// `channels` values per sample, value = base + step * 10 + channel.
link::sample::SampleBlock make_block(std::uint64_t first_step, std::size_t n_samples,
                                     std::size_t channels, std::uint64_t serial_base,
                                     double base) {
  link::sample::SampleBlock b;
  for (std::size_t c = 0; c < channels; ++c) {
    b.serials.push_back(serial_base + static_cast<std::uint64_t>(c));
    b.quantities.push_back(static_cast<std::uint8_t>(c % 2));
  }
  for (std::size_t s = 0; s < n_samples; ++s) {
    const std::uint64_t step = first_step + static_cast<std::uint64_t>(s);
    b.steps.push_back(step);
    b.times.push_back(0.001 * static_cast<double>(step));
    for (std::size_t c = 0; c < channels; ++c) {
      b.values.push_back(base + 10.0 * static_cast<double>(step) +
                         static_cast<double>(c));
    }
  }
  return b;
}

world::Snapshot make_snapshot(std::uint64_t step, double time,
                              const std::vector<std::uint64_t> &serials,
                              const std::vector<std::uint8_t> &quantities,
                              const std::vector<double> &values) {
  world::Snapshot s;
  s.step_index = step;
  s.sim_time = time;
  s.sig_serial = serials;
  s.sig_quantity = quantities;
  s.sig_value = values;
  return s;
}

void test_empty_transitions() {
  link::sample::OutputQueue q;
  assert(q.empty());
  assert(q.size() == 0);
  assert(q.bytes_queued() == 0);
  assert(q.front() == nullptr);
  link::sample::SampleBlock sentinel = make_block(999, 1, 1, 50, 1.0);
  link::sample::SampleBlock out = sentinel;
  assert(!q.try_pop(out));
  assert(out == sentinel);  // untouched on empty
  assert(q.stats() == link::sample::OutputQueueStats{});

  const link::sample::SampleBlock b = make_block(7, 2, 1, 3, 100.0);
  assert(q.try_push(b) == link::sample::PushResult::kOk);
  assert(!q.empty() && q.size() == 1);
  // Footprint: 68 header + 1*12 schema + 2*(16 + 1*8).
  assert(q.bytes_queued() == 68 + 12 + 2 * 24);
  assert(q.front() != nullptr && *q.front() == b);
  link::sample::SampleBlock got;
  assert(q.try_pop(got));
  assert(got == b);
  assert(q.empty() && q.bytes_queued() == 0);
  const link::sample::OutputQueueStats s = q.stats();
  assert(s.pushed_blocks == 1 && s.popped_blocks == 1);
  assert(s.pushed_samples == 2 && s.popped_samples == 2);
  assert(s.dropped_blocks == 0 && s.closed_blocks == 0);
}

void test_fifo_wraparound() {
  link::sample::OutputQueueConfig cfg;
  cfg.max_slots = 3;
  link::sample::OutputQueue q(cfg);
  std::vector<link::sample::SampleBlock> blocks;
  for (std::uint64_t i = 0; i < 5; ++i) {
    blocks.push_back(make_block(10 + i * 2, 2, 2, 0, 0.0));
  }
  for (std::size_t i = 0; i < 3; ++i) {
    assert(q.try_push(blocks[i]) == link::sample::PushResult::kOk);
  }
  // Full: explicit refusal, never an overwrite of the block under read.
  assert(q.try_push(blocks[3]) == link::sample::PushResult::kFull);
  assert(q.front() != nullptr && *q.front() == blocks[0]);
  assert(q.size() == 3);

  link::sample::SampleBlock got;
  assert(q.try_pop(got) && got == blocks[0]);
  assert(q.try_push(blocks[3]) == link::sample::PushResult::kOk);
  assert(q.try_push(blocks[4]) == link::sample::PushResult::kFull);
  assert(q.try_pop(got) && got == blocks[1]);
  assert(q.try_push(blocks[4]) == link::sample::PushResult::kOk);
  assert(q.try_push(blocks[0]) == link::sample::PushResult::kFull);

  for (std::size_t i = 2; i < 5; ++i) {
    assert(q.try_pop(got) && got == blocks[i]);
  }
  assert(q.empty());
  const link::sample::OutputQueueStats s = q.stats();
  assert(s.pushed_blocks == 5 && s.popped_blocks == 5);
  assert(s.pushed_samples == 10 && s.popped_samples == 10);
  assert(s.dropped_blocks == 3 && s.dropped_samples == 6);
  assert(s.high_water_slots == 3);
}

void test_byte_budget_binds_before_slots() {
  // One 1-channel/2-sample block: 68 + 12 + 2*(16+8) = 128 bytes.
  link::sample::OutputQueueConfig cfg;
  cfg.max_slots = 8;
  cfg.max_bytes = 256;
  link::sample::OutputQueue q(cfg);
  const link::sample::SampleBlock b = make_block(0, 2, 1, 0, 5.0);
  assert(q.try_push(b) == link::sample::PushResult::kOk);
  assert(q.try_push(b) == link::sample::PushResult::kOk);
  assert(q.bytes_queued() == 256);
  // Slots free (2/8) but the byte budget is exhausted: loud kFull.
  assert(q.try_push(b) == link::sample::PushResult::kFull);
  assert(q.size() == 2);
  link::sample::SampleBlock got;
  assert(q.try_pop(got) && got == b);
  assert(q.bytes_queued() == 128);
  assert(q.try_push(b) == link::sample::PushResult::kOk);
  assert(q.size() == 2 && q.bytes_queued() == 256);
  const link::sample::OutputQueueStats s = q.stats();
  assert(s.pushed_blocks == 3 && s.dropped_blocks == 1);
  assert(s.dropped_samples == 2);
  assert(s.high_water_bytes == 256);
}

void test_producer_consumer_stalls() {
  link::sample::OutputQueueConfig cfg;
  cfg.max_slots = 4;
  link::sample::OutputQueue q(cfg);
  // Producer stall: polling an empty queue reports empty, counts nothing.
  for (int i = 0; i < 100; ++i) {
    assert(q.front() == nullptr);
    link::sample::SampleBlock out;
    assert(!q.try_pop(out));
  }
  assert(q.stats() == link::sample::OutputQueueStats{});

  // Consumer stall: the producer fills up, refuses loudly, then the
  // consumer drains everything in order once it runs again.
  std::vector<link::sample::SampleBlock> blocks;
  for (std::uint64_t i = 0; i < 4; ++i) {
    blocks.push_back(make_block(i, 1, 1, 0, 0.0));
  }
  for (const auto &b : blocks) {
    assert(q.try_push(b) == link::sample::PushResult::kOk);
  }
  const link::sample::SampleBlock extra = make_block(99, 3, 1, 0, 0.0);
  for (int i = 0; i < 3; ++i) {
    assert(q.try_push(extra) == link::sample::PushResult::kFull);
  }
  link::sample::SampleBlock got;
  for (const auto &b : blocks) {
    assert(q.try_pop(got) && got == b);
  }
  assert(q.empty());
  const link::sample::OutputQueueStats s = q.stats();
  assert(s.pushed_blocks == 4 && s.popped_blocks == 4);
  assert(s.dropped_blocks == 3 && s.dropped_samples == 9);
}

void test_shutdown_drains_and_refuses() {
  link::sample::OutputQueue q;
  const link::sample::SampleBlock b0 = make_block(1, 2, 1, 0, 1.0);
  const link::sample::SampleBlock b1 = make_block(3, 1, 2, 7, 2.0);
  assert(q.try_push(b0) == link::sample::PushResult::kOk);
  assert(q.try_push(b1) == link::sample::PushResult::kOk);
  q.shutdown();
  assert(q.closed() && !q.done());
  // Admission stops; queued blocks stay readable in order.
  const link::sample::SampleBlock b2 = make_block(9, 4, 1, 0, 3.0);
  assert(q.try_push(b2) == link::sample::PushResult::kClosed);
  assert(q.try_push(b2) == link::sample::PushResult::kClosed);
  assert(q.size() == 2);
  link::sample::SampleBlock got;
  assert(q.try_pop(got) && got == b0);
  assert(q.try_pop(got) && got == b1);
  assert(!q.try_pop(got));
  assert(q.done());
  q.shutdown();  // idempotent
  assert(q.done());
  const link::sample::OutputQueueStats s = q.stats();
  assert(s.pushed_blocks == 2 && s.popped_blocks == 2);
  assert(s.closed_blocks == 2 && s.closed_samples == 8);
  assert(s.dropped_blocks == 0);

  // Shutdown on an empty queue is immediately done.
  link::sample::OutputQueue idle;
  idle.shutdown();
  assert(idle.done());
  assert(idle.try_push(b0) == link::sample::PushResult::kClosed);
}

void test_post_enqueue_mutation_is_isolated() {
  // Builder -> queue -> mutate the snapshot and the source block: the
  // queued copy stays immutable until ownership returns via pop.
  link::sample::SampleBlockBuilder builder;
  const world::Snapshot snap =
      make_snapshot(5, 0.05, {11, 12}, {0, 1}, {2.5, -0.5});
  link::sample::SampleBlock sealed;
  bool has_sealed = false;
  assert(builder.push(snap, sealed, has_sealed).ok() && !has_sealed);
  link::sample::SampleBlock tail;
  bool has_tail = false;
  assert(builder.flush(tail, has_tail).ok() && has_tail);

  link::sample::OutputQueue q;
  assert(q.try_push(tail) == link::sample::PushResult::kOk);
  const link::sample::SampleBlock expect = tail;
  // Mutate everything the queued copy was derived from.
  tail.values.assign(tail.values.size(), 12345.0);
  tail.steps.assign(tail.steps.size(), 0);
  assert(q.front() != nullptr && *q.front() == expect);
  tail.serials.clear();
  tail.quantities.clear();
  tail.steps.clear();
  tail.times.clear();
  tail.values.clear();
  link::sample::SampleBlock got;
  assert(q.try_pop(got) && got == expect);
}

void test_reusable_storage_no_per_block_alloc() {
  // Same-shape traffic: after the first fill warms every slot, vector
  // capacities observed through front() stay exactly stable across
  // further full cycles (copy-assign reuses warmed storage).
  link::sample::OutputQueueConfig cfg;
  cfg.max_slots = 4;
  link::sample::OutputQueue q(cfg);
  constexpr std::size_t kCycles = 4;
  std::size_t cap_serials = 0;
  std::size_t cap_values = 0;
  std::size_t cap_steps = 0;
  bool warmed = false;
  for (std::size_t cycle = 0; cycle < kCycles; ++cycle) {
    for (std::size_t i = 0; i < cfg.max_slots; ++i) {
      const link::sample::SampleBlock b =
          make_block(cycle * 100 + i * 4, 4, 2, 0, 1.0);
      assert(q.try_push(b) == link::sample::PushResult::kOk);
    }
    for (std::size_t i = 0; i < cfg.max_slots; ++i) {
      const link::sample::SampleBlock *f = q.front();
      assert(f != nullptr);
      const link::sample::SampleBlock expect =
          make_block(cycle * 100 + i * 4, 4, 2, 0, 1.0);
      assert(*f == expect);
      if (!warmed) {
        cap_serials = f->serials.capacity();
        cap_steps = f->steps.capacity();
        cap_values = f->values.capacity();
      } else {
        assert(f->serials.capacity() == cap_serials);
        assert(f->steps.capacity() == cap_steps);
        assert(f->values.capacity() == cap_values);
      }
      q.pop();
    }
    warmed = true;
  }
  assert(cap_values >= 4 * 2);  // warmed storage actually retained
  const link::sample::OutputQueueStats s = q.stats();
  assert(s.pushed_blocks == kCycles * cfg.max_slots);
  assert(s.popped_blocks == s.pushed_blocks);

  // Move-push path carries the same value, counted once.
  link::sample::SampleBlock moved = make_block(777, 3, 1, 4, 9.0);
  const link::sample::SampleBlock expect = moved;
  assert(q.try_push(std::move(moved)) == link::sample::PushResult::kOk);
  link::sample::SampleBlock got;
  assert(q.try_pop(got) && got == expect);
}

void test_threaded_fifo_exact() {
  // Capacity above the stream: every push succeeds, the consumer observes
  // the exact FIFO stream. Deterministic by construction.
  constexpr std::uint64_t kBlocks = 2000;
  link::sample::OutputQueueConfig cfg;
  cfg.max_slots = 2048;
  link::sample::OutputQueue q(cfg);
  std::vector<std::uint64_t> received;
  received.reserve(kBlocks);
  std::thread consumer([&]() {
    link::sample::SampleBlock out;
    while (received.size() < kBlocks) {
      if (q.try_pop(out)) {
        assert(out.sample_count() == 1);
        received.push_back(out.steps[0]);
        const double want = 0.25 * static_cast<double>(out.steps[0]);
        assert(out.values[0] == want);
      } else {
        std::this_thread::yield();
      }
    }
  });
  for (std::uint64_t i = 0; i < kBlocks; ++i) {
    link::sample::SampleBlock b = make_block(i, 1, 1, 0, 0.0);
    b.values[0] = 0.25 * static_cast<double>(i);
    assert(q.try_push(b) == link::sample::PushResult::kOk);
  }
  consumer.join();
  assert(received.size() == kBlocks);
  for (std::uint64_t i = 0; i < kBlocks; ++i) {
    assert(received[i] == i);
  }
  const link::sample::OutputQueueStats s = q.stats();
  assert(s.pushed_blocks == kBlocks && s.popped_blocks == kBlocks);
  assert(s.dropped_blocks == 0 && s.closed_blocks == 0);
}

void test_threaded_bounded_accounting() {
  // Tight capacity under live threads: drops happen, but every admitted
  // block arrives exactly once in order and the counters balance exactly.
  constexpr std::uint64_t kBlocks = 4000;
  link::sample::OutputQueueConfig cfg;
  cfg.max_slots = 8;
  link::sample::OutputQueue q(cfg);
  std::atomic<bool> producer_done{false};
  std::vector<std::uint64_t> pushed_first;
  pushed_first.reserve(kBlocks);
  std::vector<std::uint64_t> received_first;
  received_first.reserve(kBlocks);
  std::thread producer([&]() {
    for (std::uint64_t i = 0; i < kBlocks; ++i) {
      link::sample::SampleBlock b = make_block(i * 3, 3, 2, 0, 1.0);
      if (q.try_push(b) == link::sample::PushResult::kOk) {
        pushed_first.push_back(i * 3);
      }
    }
    producer_done.store(true, std::memory_order_release);
  });
  std::thread consumer([&]() {
    link::sample::SampleBlock out;
    while (!producer_done.load(std::memory_order_acquire) || !q.empty()) {
      if (q.try_pop(out)) {
        assert(out.sample_count() == 3);
        received_first.push_back(out.steps[0]);
      } else {
        std::this_thread::yield();
      }
    }
  });
  producer.join();
  consumer.join();
  // Every admitted block arrives once, in FIFO order.
  assert(received_first == pushed_first);
  for (std::size_t i = 1; i < received_first.size(); ++i) {
    assert(received_first[i] > received_first[i - 1]);
  }
  const link::sample::OutputQueueStats s = q.stats();
  assert(s.pushed_blocks + s.dropped_blocks == kBlocks);
  assert(s.popped_blocks == s.pushed_blocks);
  assert(s.pushed_samples + s.dropped_samples == kBlocks * 3);
  assert(s.popped_samples == s.pushed_samples);
  assert(q.empty() && q.bytes_queued() == 0);
}

}  // namespace

int main() {
  test_empty_transitions();
  test_fifo_wraparound();
  test_byte_budget_binds_before_slots();
  test_producer_consumer_stalls();
  test_shutdown_drains_and_refuses();
  test_post_enqueue_mutation_is_isolated();
  test_reusable_storage_no_per_block_alloc();
  test_threaded_fifo_exact();
  test_threaded_bounded_accounting();
  std::cout << "test_output_queue: OK\n";
  return 0;
}
