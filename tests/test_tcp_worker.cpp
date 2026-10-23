// Worker-backed TCP output (PHYSI-46): the sim callback only fills/seals/
// enqueues owned SampleBlocks while one worker thread encodes and serves
// the socket. Loopback tests verify ordered exactness (values/step IDs via
// the record_stream batch decoder), latest-mode coalescing, stalled-sibling
// isolation with exact final State, and bounded shutdown accounting.
// No timing thresholds are asserted: pressure comes from byte volumes past
// the kernel buffer with fully stalled readers, never from sleeps.

#include "wengine/core/run_config.hpp"
#include "wengine/emit/sink.hpp"
#include "wengine/link/compact_codec.hpp"
#include "wengine/link/frame.hpp"
#include "wengine/link/record_stream.hpp"
#include "wengine/link/sample_block.hpp"
#include "wengine/link/socket_publisher.hpp"
#include "wengine/link/tcp_worker.hpp"
#include "wengine/solvers/run.hpp"
#include "wengine/world/state.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace wengine;

namespace {

constexpr int kIoTimeoutSec = 15;

void set_recv_timeout(int fd) {
  timeval tv{};
  tv.tv_sec = kIoTimeoutSec;
  tv.tv_usec = 0;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

int make_listener(std::uint16_t &port_out) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 ||
      ::listen(fd, 8) != 0) {
    ::close(fd);
    return -1;
  }
  socklen_t len = sizeof(addr);
  if (::getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &len) != 0) {
    ::close(fd);
    return -1;
  }
  port_out = ntohs(addr.sin_port);
  return fd;
}

std::string endpoint_for(std::uint16_t port) {
  return "127.0.0.1:" + std::to_string(port);
}

bool recv_all(int fd, unsigned char *buf, std::size_t len) {
  std::size_t got = 0;
  while (got < len) {
    const ssize_t n = ::recv(fd, buf + got, len - got, 0);
    if (n <= 0) {
      return false;
    }
    got += static_cast<std::size_t>(n);
  }
  return true;
}

struct Recorder : public emit::ISink {
  std::vector<world::Snapshot> snaps;
  void on_snapshot(const world::Snapshot &s) override { snaps.push_back(s); }
};

core::RunConfig signal_cfg(double dt, std::uint64_t steps, std::uint64_t count = 1) {
  core::RunConfig cfg;
  cfg.loop.dt = dt;
  cfg.loop.emit_every = 1;
  cfg.steps = steps;
  cfg.solver = "signal";
  cfg.particles = 0;
  cfg.signal.count = count;
  return cfg;
}

// Conservation from tcp_worker.hpp (block units; exact at rest). Drained
// rest additionally balances samples exactly.
void check_conservation(const link::TcpWorkerHealth &h, bool drained) {
  assert(h.queue.pushed_blocks == h.published_blocks + h.dropped_send +
                                        h.coalesced_blocks + h.abandoned_blocks +
                                        h.queued_blocks + h.held_blocks);
  if (drained) {
    assert(h.queued_blocks == 0 && h.held_blocks == 0);
    assert(h.queue.pushed_samples == h.published_samples + h.dropped_send_samples +
                                          h.coalesced_samples + h.abandoned_samples);
  }
}

// Rebuild the exact sealed-block stream the worker must have produced from
// the recorded snapshots (same builder config the worker used).
std::vector<link::sample::SampleBlock> sealed_stream(
    const std::vector<world::Snapshot> &snaps) {
  link::sample::SampleBlockBuilder builder;
  std::vector<link::sample::SampleBlock> out;
  for (const world::Snapshot &s : snaps) {
    link::sample::SampleBlock sealed;
    bool has_sealed = false;
    assert(builder.push(s, sealed, has_sealed).ok());
    if (has_sealed) {
      out.push_back(sealed);
    }
  }
  link::sample::SampleBlock tail;
  bool has_tail = false;
  assert(builder.flush(tail, has_tail).ok());
  if (has_tail) {
    out.push_back(tail);
  }
  return out;
}

void test_stream_decoder() {
  // Mixed framed/block stream with a trailing partial record, then loud
  // refusals for absurd lengths, unknown kinds and domain mismatch.
  link::Frame frame;
  frame.step = 9;
  frame.time = 0.5;
  frame.channels.push_back({"sig.0.U", 2.5});
  link::sample::SampleBlock block;
  block.serials = {4};
  block.quantities = {0};
  block.steps = {9};
  block.times = {0.5};
  block.values = {2.5};

  std::array<unsigned char, 65507> enc{};
  std::size_t frame_len = 0;
  assert(link::compact::encode_record(frame, "signal", 11, 3, enc.data(), enc.size(),
                                      frame_len)
             .ok());
  std::size_t block_len = 0;
  // NOTE: enc is reused; copy each record out before the next encode.
  const std::vector<unsigned char> frame_rec(enc.data(), enc.data() + frame_len);
  assert(link::sample::encode_block(block, "signal", 11, 7, enc.data(), enc.size(),
                                    block_len)
             .ok());
  const std::vector<unsigned char> block_rec(enc.data(), enc.data() + block_len);

  std::vector<unsigned char> stream;
  auto append_prefixed = [&](const std::vector<unsigned char> &rec) {
    const std::uint32_t n = static_cast<std::uint32_t>(rec.size());
    stream.push_back(static_cast<unsigned char>(n & 0xFFu));
    stream.push_back(static_cast<unsigned char>((n >> 8) & 0xFFu));
    stream.push_back(static_cast<unsigned char>((n >> 16) & 0xFFu));
    stream.push_back(static_cast<unsigned char>((n >> 24) & 0xFFu));
    stream.insert(stream.end(), rec.begin(), rec.end());
  };
  append_prefixed(frame_rec);
  append_prefixed(block_rec);
  stream.push_back(0x05); // trailing partial prefix: not an error
  stream.push_back(0x00);

  link::StreamDecode dec;
  assert(link::decode_stream(stream.data(), stream.size(), "signal", dec).ok());
  assert(dec.records.size() == 2 && dec.has_partial);
  assert(dec.consumed == stream.size() - 2);
  assert(dec.records[0].kind == link::compact::RecordKind::kFrame);
  assert(dec.records[0].frame == frame);
  assert(dec.records[0].run_id == 11 && dec.records[0].seq == 3);
  assert(dec.records[1].kind == link::compact::RecordKind::kSampleBlock);
  assert(dec.records[1].block == block);
  assert(dec.records[1].run_id == 11 && dec.records[1].seq == 7);

  // Absurd prefix length: loud, never sized from.
  unsigned char bad_prefix[4]{0xFF, 0xFF, 0xFF, 0xFF};
  assert(!link::decode_stream(bad_prefix, sizeof(bad_prefix), "signal", dec).ok());
  // Unknown kind byte: loud.
  std::vector<unsigned char> bad_kind = frame_rec;
  bad_kind[8] = 9;
  std::vector<unsigned char> bad_stream;
  const std::uint32_t bn = static_cast<std::uint32_t>(bad_kind.size());
  bad_stream.push_back(static_cast<unsigned char>(bn & 0xFFu));
  bad_stream.push_back(static_cast<unsigned char>((bn >> 8) & 0xFFu));
  bad_stream.push_back(static_cast<unsigned char>((bn >> 16) & 0xFFu));
  bad_stream.push_back(static_cast<unsigned char>((bn >> 24) & 0xFFu));
  bad_stream.insert(bad_stream.end(), bad_kind.begin(), bad_kind.end());
  assert(!link::decode_stream(bad_stream.data(), bad_stream.size(), "signal", dec).ok());
  // Domain mismatch: loud.
  assert(!link::decode_stream(stream.data(), stream.size() - 2, "other", dec).ok());
  // Empty stream decodes clean.
  assert(link::decode_stream(stream.data(), 0, "signal", dec).ok());
  assert(dec.records.empty() && !dec.has_partial && dec.consumed == 0);
}

void test_ordered_loopback_exact() {
  // 200 snapshots -> blocks of 64/64/64/8; every value and step ID arrives
  // bit-exact with consecutive send seqs; the sim State equals a run with
  // no networking attached.
  std::uint16_t port = 0;
  const int listen_fd = make_listener(port);
  assert(listen_fd >= 0);
  link::TcpWorkerConfig cfg;
  cfg.endpoint = endpoint_for(port);
  cfg.domain = "signal";
  cfg.mode = link::TcpWorkerMode::kOrdered;
  link::TcpBlockWorker worker(cfg);
  assert(worker.ok());
  const std::uint64_t run_id = worker.run_id();
  const int peer = ::accept(listen_fd, nullptr, nullptr);
  assert(peer >= 0);
  set_recv_timeout(peer);

  world::State final_state;
  Recorder rec;
  std::array<emit::ISink *, 2> sinks{&worker, &rec};
  assert(solvers::run_headless(signal_cfg(0.01, 200), sinks, final_state).ok());
  assert(worker.finish(10000).ok());

  link::TcpHello hello{};
  assert(recv_all(peer, reinterpret_cast<unsigned char *>(&hello), sizeof(hello)));
  assert(link::check_tcp_hello(hello, "signal").ok());

  const std::vector<link::sample::SampleBlock> want = sealed_stream(rec.snaps);
  assert(want.size() == 4);
  std::vector<unsigned char> bytes;
  std::vector<link::StreamRecord> got;
  std::array<unsigned char, 65536> chunk{};
  while (got.size() < want.size()) {
    const ssize_t n = ::recv(peer, chunk.data(), chunk.size(), 0);
    assert(n > 0);
    bytes.insert(bytes.end(), chunk.begin(), chunk.begin() + n);
    link::StreamDecode dec;
    assert(link::decode_stream(bytes.data(), bytes.size(), "signal", dec).ok());
    got = dec.records;
    assert(!dec.has_partial || got.size() < want.size());
  }
  assert(got.size() == want.size());
  for (std::size_t i = 0; i < want.size(); ++i) {
    assert(got[i].kind == link::compact::RecordKind::kSampleBlock);
    assert(got[i].block == want[i]); // values + step IDs exact
    assert(got[i].run_id == run_id && got[i].seq == i + 1);
  }
  const link::TcpWorkerHealth h = worker.health();
  assert(h.attempted == 201); // 200 snapshots + 1 finish() flush
  assert(h.published_blocks == 4);
  assert(h.published_samples == 200 && h.dropped_send == 0);
  assert(h.coalesced_blocks == 0 && h.queue.dropped_blocks == 0);
  assert(h.queued_blocks == 0 && h.held_blocks == 0 && !h.has_pending);
  assert(h.callback_count == 200 && h.callback_ns_max > 0);
  check_conservation(h, true);

  world::State ref_state;
  assert(solvers::run_headless(signal_cfg(0.01, 200),
                               std::span<emit::ISink *>{}, ref_state)
             .ok());
  assert(final_state == ref_state);
  ::close(peer);
  ::close(listen_fd);
}

void test_latest_coalesces_under_pressure() {
  // Unread reader during a ~2 MiB burst into a 64-slot queue: the volume
  // volumes past loopback socket buffers with margin (a stalled peer keeps
  // buffers ungrown), so the worker stall and the resulting backlog are
  // certain, never timing-dependent. The first pop after the reader starts
  // draining coalesces the backlog to the newest; the reader drains
  // concurrently with finish(). Received seqs stay strictly increasing
  // with a fresh tail; every received block matches one sealed block.
  constexpr std::uint64_t kSteps = 2000;
  constexpr std::uint64_t kSources = 128;
  std::uint16_t port = 0;
  const int listen_fd = make_listener(port);
  assert(listen_fd >= 0);
  link::TcpWorkerConfig cfg;
  cfg.endpoint = endpoint_for(port);
  cfg.domain = "signal";
  cfg.mode = link::TcpWorkerMode::kLatest;
  cfg.queue.max_slots = 64;
  // Wide (~65 KiB) blocks: provision bytes explicitly — the default byte
  // budget would bind long before the 64 slots and turn absorption into
  // loud drops. The stall must come from the unread socket, not the queue.
  cfg.queue.max_bytes = 4 * 1024 * 1024;
  link::TcpBlockWorker worker(cfg);
  assert(worker.ok());
  const int peer = ::accept(listen_fd, nullptr, nullptr);
  assert(peer >= 0); // held: never read during the burst
  set_recv_timeout(peer);

  world::State final_state;
  Recorder rec;
  std::array<emit::ISink *, 2> sinks{&worker, &rec};
  assert(solvers::run_headless(signal_cfg(0.001, kSteps, kSources), sinks, final_state).ok());
  const std::vector<link::sample::SampleBlock> want = sealed_stream(rec.snaps);
  // Wide blocks seal on the byte budget before 64 samples, so only the
  // totals are pinned: every snapshot lands in exactly one block, in order.
  std::uint64_t want_samples = 0;
  for (const auto &b : want) {
    want_samples += static_cast<std::uint64_t>(b.sample_count());
  }
  assert(!want.empty() && want_samples == kSteps);
  assert(want.size() <= 64); // queue absorbs every seal: no driver drops

  // Drain concurrently with finish(): the reader stops at the newest
  // sealed block (content match — coalesced seals never arrive, so no
  // count target can terminate this loop).
  std::vector<unsigned char> bytes;
  std::atomic<bool> go{false};
  const link::sample::SampleBlock *newest = nullptr;
  std::thread reader([&] {
    link::TcpHello hello{};
    if (!recv_all(peer, reinterpret_cast<unsigned char *>(&hello), sizeof(hello)) ||
        !link::check_tcp_hello(hello, "signal").ok()) {
      return;
    }
    std::array<unsigned char, 65536> chunk{};
    for (;;) {
      const ssize_t n = ::recv(peer, chunk.data(), chunk.size(), 0);
      if (n <= 0) {
        return;
      }
      bytes.insert(bytes.end(), chunk.begin(), chunk.begin() + n);
      if (!go.load(std::memory_order_acquire)) {
        continue;
      }
      link::StreamDecode dec;
      if (!link::decode_stream(bytes.data(), bytes.size(), "signal", dec).ok()) {
        return;
      }
      if (!dec.records.empty() && newest != nullptr &&
          dec.records.back().kind == link::compact::RecordKind::kSampleBlock &&
          dec.records.back().block == *newest) {
        return; // newest sealed block arrived; nothing was sealed after it
      }
    }
  });
  newest = &want.back();
  go.store(true, std::memory_order_release);
  assert(worker.finish(120000).ok());
  reader.join();
  link::StreamDecode dec;
  assert(link::decode_stream(bytes.data(), bytes.size(), "signal", dec).ok());
  assert(!dec.has_partial);
  const std::vector<link::StreamRecord> got = dec.records;
  assert(!got.empty()); // pre-stall head + coalesced tail (at least)
  for (std::size_t i = 1; i < got.size(); ++i) {
    assert(got[i].seq > got[i - 1].seq);
  }
  for (const link::StreamRecord &r : got) {
    assert(r.kind == link::compact::RecordKind::kSampleBlock);
  }
  // Every received block is one of the sealed blocks, in seal order (send
  // seqs cannot index seals under coalescing — values/steps match by scan).
  std::size_t pos = 0;
  for (const link::StreamRecord &r : got) {
    bool found = false;
    for (; pos < want.size(); ++pos) {
      if (r.block == want[pos]) {
        found = true;
        ++pos;
        break;
      }
    }
    assert(found);
  }
  assert(pos > 0 && want[pos - 1] == want.back()); // newest sealed delivered
  const link::TcpWorkerHealth h = worker.health();
  assert(h.coalesced_blocks > 0); // backlog visibly coalesced, never silent
  assert(h.coalesced_samples > 0);
  assert(h.queue.dropped_blocks == 0); // 64 slots absorbed every seal
  check_conservation(h, true);
  ::close(peer);
  ::close(listen_fd);
}

void test_stalled_sibling_isolation() {
  // A never-draining reader on worker A must not slow the sim thread or
  // worker B: B delivers every block, the final State equals a run with no
  // networking, and A reports queue-full loudly with a bounded finish.
  // The burst (~1.1 MiB) volumes past loopback socket buffers so the
  // stall on A is certain; B's queue is sized past the whole 63-block
  // burst so its lossless delivery is certain too, at any optimization
  // level (an 8-slot default queue drops under an -O3 producer burst).
  constexpr std::uint64_t kSteps = 4000;
  constexpr std::uint64_t kSources = 32;
  std::uint16_t port_a = 0;
  const int listen_a = make_listener(port_a);
  assert(listen_a >= 0);
  std::uint16_t port_b = 0;
  const int listen_b = make_listener(port_b);
  assert(listen_b >= 0);

  link::TcpWorkerConfig cfg_a;
  cfg_a.endpoint = endpoint_for(port_a);
  cfg_a.domain = "signal";
  cfg_a.mode = link::TcpWorkerMode::kOrdered;
  // Small relative to the 63-block burst, but sized so the stall still
  // overflows by volume, never by timing: the kernel doubles the worker's
  // 256 KiB SO_SNDBUF (512 KiB effective) while the pinned 4 KiB receive
  // window stays 8 KiB effective, so the stalled peer absorbs ~30 blocks
  // before EAGAIN; the worker holds 1 more in pending. An 8-slot queue
  // (30 + 1 + 8 = 39 < 63) therefore overflows with 20+ drops of margin
  // against kernel accounting slack — a 32-slot queue would land exactly
  // on the absorb boundary (31 + 1 + 31 = 63) and drop nothing.
  cfg_a.queue.max_slots = 8;
  cfg_a.queue.max_bytes = link::sample::kBlockMaxBytes * 8;
  link::TcpBlockWorker worker_a(cfg_a);
  assert(worker_a.ok());
  link::TcpWorkerConfig cfg_b;
  cfg_b.endpoint = endpoint_for(port_b);
  cfg_b.domain = "signal";
  cfg_b.mode = link::TcpWorkerMode::kOrdered;
  // Absorb the whole burst deterministically: 4000 steps seal 63 blocks
  // and an -O3 sim thread emits them faster than the worker's poll +
  // encode + send loop drains an 8-slot default queue, so the default
  // bound drops loudly by design. Sizing the queue past the known burst
  // (slots and byte budget alike — the default byte budget also only
  // covers 8 maximum-size blocks) removes the producer-speed assumption
  // without touching the stall under test on worker A.
  cfg_b.queue.max_slots = 128;
  cfg_b.queue.max_bytes = link::sample::kBlockMaxBytes * 128;
  link::TcpBlockWorker worker_b(cfg_b);
  assert(worker_b.ok());

  const int peer_a = ::accept(listen_a, nullptr, nullptr);
  assert(peer_a >= 0); // stalled: never read
  set_recv_timeout(peer_a);
  // Pin the stalled peer's receive window tiny (kernel doubles to 8 KiB)
  // so worker A blocks on full kernel buffers after ~15 blocks at any
  // producer speed; with default autotuned buffers the stall needs the
  // worker to have moved megabytes first, which an -O3 burst never does.
  const int tiny_rcv = 4096;
  assert(::setsockopt(peer_a, SOL_SOCKET, SO_RCVBUF, &tiny_rcv, sizeof(tiny_rcv)) == 0);
  const int peer_b = ::accept(listen_b, nullptr, nullptr);
  assert(peer_b >= 0);
  set_recv_timeout(peer_b);

  // B drains concurrently so only A feels backpressure. The reader stops
  // after exactly the sealed-block count below (deterministic from steps).
  constexpr std::size_t kExpectBlocks =
      static_cast<std::size_t>((kSteps + 63) / 64);
  std::vector<unsigned char> bytes_b;
  std::atomic<std::size_t> decoded_b{0};
  std::atomic<bool> hello_b_ok{false};
  std::thread reader_b([&] {
    link::TcpHello hello_b{};
    if (!recv_all(peer_b, reinterpret_cast<unsigned char *>(&hello_b), sizeof(hello_b)) ||
        !link::check_tcp_hello(hello_b, "signal").ok()) {
      return;
    }
    hello_b_ok.store(true, std::memory_order_release);
    std::array<unsigned char, 65536> chunk{};
    while (decoded_b.load(std::memory_order_acquire) < kExpectBlocks) {
      const ssize_t n = ::recv(peer_b, chunk.data(), chunk.size(), 0);
      if (n <= 0) {
        return; // timeout/close: main asserts the count below
      }
      bytes_b.insert(bytes_b.end(), chunk.begin(), chunk.begin() + n);
      link::StreamDecode dec;
      if (!link::decode_stream(bytes_b.data(), bytes_b.size(), "signal", dec).ok()) {
        return;
      }
      decoded_b.store(dec.records.size(), std::memory_order_release);
    }
  });

  world::State final_state;
  Recorder rec;
  std::array<emit::ISink *, 3> sinks{&worker_a, &worker_b, &rec};
  assert(solvers::run_headless(signal_cfg(0.001, kSteps, kSources), sinks, final_state).ok());
  assert(worker_b.finish(30000).ok());
  reader_b.join();
  assert(hello_b_ok.load(std::memory_order_acquire));
  assert(decoded_b.load(std::memory_order_acquire) == kExpectBlocks);
  std::vector<link::StreamRecord> got_b;
  {
    link::StreamDecode dec;
    assert(link::decode_stream(bytes_b.data(), bytes_b.size(), "signal", dec).ok());
    assert(!dec.has_partial && dec.records.size() == kExpectBlocks);
    got_b = dec.records;
  }
  const std::vector<link::sample::SampleBlock> want = sealed_stream(rec.snaps);
  assert(got_b.size() == want.size()); // sibling waited on nothing
  for (std::size_t i = 0; i < want.size(); ++i) {
    assert(got_b[i].block == want[i]);
    assert(got_b[i].seq == i + 1);
  }
  const link::TcpWorkerHealth hb = worker_b.health();
  assert(hb.queue.dropped_blocks == 0 && hb.coalesced_blocks == 0);
  check_conservation(hb, true);

  // A hit its tiny queue under the stall: loud drops, bounded finish.
  const link::TcpWorkerHealth ha_mid = worker_a.health();
  assert(ha_mid.queue.dropped_blocks > 0);
  assert(!worker_a.finish(100).ok()); // still stalled: deadline, not sticky
  assert(worker_a.ok()); // deadline never poisons
  worker_a.shutdown();
  const link::TcpWorkerHealth ha = worker_a.health();
  assert(ha.abandoned_blocks > 0); // remainder reported, never lost silently
  check_conservation(ha, true);

  world::State ref_state;
  assert(solvers::run_headless(signal_cfg(0.001, kSteps, kSources),
                               std::span<emit::ISink *>{}, ref_state)
             .ok());
  assert(final_state == ref_state); // networking never touches numerics
  ::close(peer_a);
  ::close(peer_b);
  ::close(listen_a);
  ::close(listen_b);
}

void test_shutdown_and_construction_paths() {
  // Failed construction (nobody listening): sticky, admissions disabled,
  // accounting still balances.
  {
    link::TcpWorkerConfig cfg;
    cfg.endpoint = "127.0.0.1:1"; // discard port: refused
    cfg.domain = "signal";
    link::TcpBlockWorker dead(cfg);
    assert(!dead.ok());
    world::Snapshot snap;
    snap.step_index = 1;
    dead.on_snapshot(snap);
    dead.on_snapshot(snap);
    const link::TcpWorkerHealth h = dead.health();
    assert(h.attempted == 2 && h.disabled_after_error == 2);
    assert(h.published_blocks == 0);
    check_conservation(h, true);
    dead.shutdown(); // safe after failed construction, idempotent
    dead.shutdown();
    assert(!dead.finish(0).ok());
  }
  // Bad config shape: loud usage refusal, never a thread.
  {
    link::TcpWorkerConfig cfg;
    cfg.endpoint = "127.0.0.1:1";
    cfg.domain = "";
    link::TcpBlockWorker bad(cfg);
    assert(!bad.ok());
  }
  {
    link::TcpWorkerConfig cfg;
    cfg.endpoint = "not-an-endpoint";
    cfg.domain = "signal";
    link::TcpBlockWorker bad(cfg);
    assert(!bad.ok());
  }
  // finish() usage gate before any shutdown.
  {
    std::uint16_t port = 0;
    const int listen_fd = make_listener(port);
    assert(listen_fd >= 0);
    link::TcpWorkerConfig cfg;
    cfg.endpoint = endpoint_for(port);
    cfg.domain = "signal";
    link::TcpBlockWorker worker(cfg);
    assert(worker.ok());
    assert(!worker.finish(-1).ok());
    const int peer = ::accept(listen_fd, nullptr, nullptr);
    assert(peer >= 0);
    set_recv_timeout(peer);
    world::State state;
    Recorder rec;
    std::array<emit::ISink *, 2> sinks{&worker, &rec};
    assert(solvers::run_headless(signal_cfg(0.01, 10), sinks, state).ok());
    assert(worker.finish(10000).ok());
    worker.shutdown();
    worker.shutdown();
    const link::TcpWorkerHealth h = worker.health();
    assert(!h.worker_alive && h.abandoned_blocks == 0);
    assert(h.attempted == 11 && h.published_blocks == 1); // 10 snaps + flush
    check_conservation(h, true);
    ::close(peer);
    ::close(listen_fd);
  }
}

} // namespace

int main() {
  test_stream_decoder();
  test_ordered_loopback_exact();
  test_latest_coalesces_under_pressure();
  test_stalled_sibling_isolation();
  test_shutdown_and_construction_paths();
  std::cout << "test_tcp_worker: OK\n";
  return 0;
}
