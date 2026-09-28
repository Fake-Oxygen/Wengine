#include "wengine/link/protocol.hpp"

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace wengine;

namespace {

// Solver-agnostic payload: spatial items plus named scalar channels, the
// shape every future solver (circuits, EM, FEA, ...) publishes. Exact-
// binary values throughout, so round-trips assert ==.
link::Frame make_frame(std::uint64_t step, double time) {
  link::Frame f;
  f.step = step;
  f.time = time;
  link::FrameItem a;
  a.x = 0.1 + 0.2; // inexact binary: proves bitwise fidelity
  a.y = 1.5;
  a.z = -0.25;
  a.vx = 0.0;
  a.vy = -0.5;
  a.vz = 2.5;
  f.items.push_back(a);
  link::FrameItem b;
  b.x = -4.0;
  b.has_velocity = false; // e.g. a static probe: allowed per item
  b.size = 2.0;
  b.color = 7;
  f.items.push_back(b);
  f.channels.push_back({"node_A", 3.375}); // circuit-style named scalar
  f.channels.push_back({"energy", 0.1 + 0.2});
  return f;
}

void test_header_init_and_check() {
  link::SegmentHeader h;
  assert(link::init_header(h, "projectile").ok());
  assert(h.version.load() == link::kVersion);
  assert(std::string(h.domain) == "projectile");
  assert(h.max_items == static_cast<std::uint64_t>(link::kMaxItems));
  assert(h.max_channels == static_cast<std::uint64_t>(link::kMaxChannels));
  assert(h.slot_count == static_cast<std::uint64_t>(link::kSlotCount));
  assert(link::header_published(h) == 0);
  assert(link::check_header(h).ok());

  // Loud misuse, never silent truncation (S7).
  link::SegmentHeader bad;
  assert(!link::init_header(bad, "").ok()); // empty domain
  assert(!link::init_header(bad, std::string(link::kDomainSize, 'x')).ok());
  assert(!link::init_header(bad, std::string(link::kDomainSize + 1, 'x')).ok());
  // Longest legal tag: kDomainSize - 1 chars + NUL.
  assert(link::init_header(bad, std::string(link::kDomainSize - 1, 'y')).ok());
  assert(link::check_header(bad).ok());

  // Corrupt/foreign headers fail loudly at open, not mid-stream. Headers
  // hold atomics, so they are not copyable: rebuild via init and mutate
  // one field at a time.
  {
    link::SegmentHeader corrupt;
    assert(link::init_header(corrupt, "projectile").ok());
    corrupt.version.store(link::kVersion + 1);
    assert(!link::check_header(corrupt).ok());
  }
  {
    link::SegmentHeader corrupt;
    assert(link::init_header(corrupt, "projectile").ok());
    corrupt.max_items = 7;
    assert(!link::check_header(corrupt).ok());
  }
  {
    link::SegmentHeader corrupt;
    assert(link::init_header(corrupt, "projectile").ok());
    corrupt.max_channels = 0;
    assert(!link::check_header(corrupt).ok());
  }
  {
    link::SegmentHeader corrupt;
    assert(link::init_header(corrupt, "projectile").ok());
    corrupt.slot_count = 0;
    assert(!link::check_header(corrupt).ok());
  }
  {
    link::SegmentHeader corrupt;
    assert(link::init_header(corrupt, "projectile").ok());
    corrupt.domain[0] = '\0';
    assert(!link::check_header(corrupt).ok());
  }
  {
    link::SegmentHeader corrupt;
    assert(link::init_header(corrupt, "projectile").ok());
    for (std::size_t i = 0; i < link::kDomainSize; ++i) {
      corrupt.domain[i] = 'z'; // unterminated: corrupt
    }
    assert(!link::check_header(corrupt).ok());
  }
}

void test_compat_matrix() {
  // v1 matrix: same version + capacities + valid domain interops; every
  // other combination refuses loudly with the actionable cause reported.
  // When v2 lands, its row is added here — these rows keep passing.
  link::SegmentHeader good;
  assert(link::init_header(good, "projectile").ok());
  const link::HeaderReport ok_rep = link::inspect_header(good);
  assert(ok_rep.status.ok() && ok_rep.issue == link::HeaderIssue::kNone);
  assert(ok_rep.found_version == link::kVersion);
  assert(link::check_header(good).ok()); // consistent with the report

  // Foreign versions: older, newer, zero (mid-construction), max.
  for (std::uint32_t v : {std::uint32_t{0}, link::kVersion + 1,
                          std::uint32_t{0xFFFFFFFFu}}) {
    if (v == link::kVersion) {
      continue;
    }
    link::SegmentHeader foreign;
    assert(link::init_header(foreign, "projectile").ok());
    foreign.version.store(v);
    const link::HeaderReport rep = link::inspect_header(foreign);
    assert(!rep.status.ok());
    assert(rep.status.code == core::StatusCode::kInvalidArgument);
    assert(rep.issue == link::HeaderIssue::kVersionMismatch);
    assert(rep.found_version == v); // the clear error carries the version
    assert(!link::check_header(foreign).ok());
  }

  // Capacity drift under a matching version: capacity cause, not version.
  {
    link::SegmentHeader drift;
    assert(link::init_header(drift, "projectile").ok());
    drift.slot_count = 16;
    const link::HeaderReport rep = link::inspect_header(drift);
    assert(!rep.status.ok() && rep.issue == link::HeaderIssue::kCapacityMismatch);
    assert(rep.found_version == link::kVersion);
  }
  // Domain faults under a matching version: domain cause.
  {
    link::SegmentHeader nodomain;
    assert(link::init_header(nodomain, "projectile").ok());
    nodomain.domain[0] = '\0';
    const link::HeaderReport rep = link::inspect_header(nodomain);
    assert(!rep.status.ok() && rep.issue == link::HeaderIssue::kDomainMismatch);
  }

  // Shared name rule, both endpoints (tools validate upfront, exit 2).
  assert(link::valid_segment_name("/wengine-demo"));
  assert(link::valid_segment_name("/a"));
  assert(!link::valid_segment_name(""));
  assert(!link::valid_segment_name("/"));
  assert(!link::valid_segment_name("no-leading-slash"));
  assert(!link::valid_segment_name("/has/nested/slash"));
}

void test_encode_decode_roundtrip() {
  const link::Frame src = make_frame(7, 0.07);
  link::LinkFrame wire{};
  assert(link::encode_frame(src, wire, 42).ok());
  assert(wire.frame_seq == 42);
  assert(wire.step_index == 7);
  assert(wire.item_count == 2);
  assert(wire.channel_count == 2);

  link::Frame dst;
  assert(link::decode_frame(wire, dst).ok());
  assert(dst == src); // order + bits preserved: exact, not approximate (S3)

  // Empty frames are valid (S5): the tick stays observable.
  const link::Frame empty;
  link::LinkFrame empty_wire{};
  assert(link::encode_frame(empty, empty_wire, 1).ok());
  assert(empty_wire.item_count == 0 && empty_wire.channel_count == 0);
  link::Frame empty_back;
  assert(link::decode_frame(empty_wire, empty_back).ok());
  assert(empty_back == empty);

  // Corrupt wire counts/names never read out of bounds.
  link::LinkFrame corrupt{};
  corrupt.item_count = static_cast<std::uint64_t>(link::kMaxItems) + 1;
  link::Frame sink;
  assert(!link::decode_frame(corrupt, sink).ok());
  corrupt.item_count = 0;
  corrupt.channel_count = static_cast<std::uint64_t>(link::kMaxChannels) + 1;
  assert(!link::decode_frame(corrupt, sink).ok());
}

void test_particle_adapter() {
  // The particle model enters the link only here: one adapter, dense slot
  // order preserved, channels mirror the GUI particle adapter.
  world::Snapshot s;
  s.step_index = 7;
  s.sim_time = 0.07;
  s.pos_x = {0.1 + 0.2, -4.0};
  s.pos_y = {1.5, 0.25};
  s.vel_x = {0.0, 2.5};
  s.vel_y = {-0.5, 0.0};

  const link::Frame f = link::frame_from_snapshot(s);
  assert(f.step == 7 && f.time == 0.07);
  assert(f.items.size() == 2);
  assert(f.items[0].x == 0.1 + 0.2);
  assert(f.items[0].y == 1.5);
  assert(f.items[0].z == 0.0);
  assert(f.items[0].vx == 0.0 && f.items[0].vy == -0.5);
  assert(f.items[1].x == -4.0);
  assert(f.channels.size() == 6);
  assert(f.channels[0].name == "count" && f.channels[0].value == 2.0);
  assert(f.channels[1].name == "p0.x" && f.channels[1].value == 0.1 + 0.2);

  // Adapter output round-trips through the wire exactly.
  link::LinkFrame wire{};
  assert(link::encode_frame(f, wire, 3).ok());
  link::Frame back;
  assert(link::decode_frame(wire, back).ok());
  assert(back == f);

  // Empty snapshot -> empty (but valid) frame.
  const link::Frame empty = link::frame_from_snapshot(world::Snapshot{});
  assert(empty.items.empty());
}

void test_encode_over_bound() {
  // Too many items: kOutOfRange with `out` untouched (drop loudly, S7).
  link::Frame big;
  big.items.assign(link::kMaxItems + 1, link::FrameItem{});
  link::LinkFrame wire{};
  wire.item_count = 999; // sentinel
  const core::Status st = link::encode_frame(big, wire, 9);
  assert(!st.ok() && st.code == core::StatusCode::kOutOfRange);
  assert(wire.item_count == 999);

  // Too many channels: same loud refusal.
  link::Frame many;
  for (std::size_t k = 0; k <= link::kMaxChannels; ++k) {
    many.channels.push_back({"c", 1.0});
  }
  link::LinkFrame wire2{};
  assert(!link::encode_frame(many, wire2, 9).ok());

  // Over-long channel name: same loud refusal (never truncated).
  link::Frame named;
  named.channels.push_back({std::string(link::kChannelNameSize, 'n'), 1.0});
  link::LinkFrame wire3{};
  assert(!link::encode_frame(named, wire3, 9).ok());

  // Longest legal name (kChannelNameSize - 1 + NUL) fits exactly.
  link::Frame edge;
  edge.channels.push_back({std::string(link::kChannelNameSize - 1, 'e'), 2.0});
  link::LinkFrame wire4{};
  assert(link::encode_frame(edge, wire4, 9).ok());
  link::Frame edge_back;
  assert(link::decode_frame(wire4, edge_back).ok());
  assert(edge_back == edge);

  // Exactly at both bounds still fits.
  link::Frame full;
  full.items.assign(link::kMaxItems, link::FrameItem{});
  for (std::size_t k = 0; k < link::kMaxChannels; ++k) {
    full.channels.push_back({"c", 0.5});
  }
  link::LinkFrame full_wire{};
  assert(link::encode_frame(full, full_wire, 10).ok());
  link::Frame full_back;
  assert(link::decode_frame(full_wire, full_back).ok());
  assert(full_back == full);

  // Unterminated channel name on the wire: corrupt, refuse loudly.
  link::LinkFrame corrupt{};
  corrupt.channel_count = 1;
  for (std::size_t i = 0; i < link::kChannelNameSize; ++i) {
    corrupt.channels[0].name[i] = 'q';
  }
  link::Frame sink;
  assert(!link::decode_frame(corrupt, sink).ok());
}

void test_seqlock_states() {
  link::Slot slot; // seq == 0: never written
  link::LinkFrame out{};
  assert(link::slot_try_read(slot, out) == link::SlotRead::kEmpty);

  link::slot_begin_write(slot); // writer active: readers back off
  assert(link::slot_try_read(slot, out) == link::SlotRead::kBusy);

  const link::Frame src = make_frame(5, 0.05);
  assert(link::encode_frame(src, slot.frame, 5).ok());
  link::slot_end_write(slot);
  assert(link::slot_try_read(slot, out) == link::SlotRead::kOk);
  assert(out.frame_seq == 5);
  link::Frame dst;
  assert(link::decode_frame(out, dst).ok());
  assert(dst == src);
}

void test_ring_and_lag() {
  const std::uint64_t slots = static_cast<std::uint64_t>(link::kSlotCount);
  assert(link::ring_slot(0, slots) == 0);
  assert(link::ring_slot(slots - 1, slots) == slots - 1);
  assert(link::ring_slot(slots, slots) == 0); // overwrite oldest: drop-oldest
  assert(link::ring_slot(slots + 1, slots) == 1);

  assert(link::reader_missed(0, 1) == 0); // first frame: nothing missed
  assert(link::reader_missed(5, 6) == 0); // consecutive: nothing missed
  assert(link::reader_missed(5, 5) == 0); // re-read: nothing missed
  assert(link::reader_missed(5, 8) == 2); // two frames overwritten unseen
}

void test_lag_stats() {
  link::LagStats cur;
  assert(link::lag_observe(cur, 1) == 0);
  assert(cur.received == 1 && cur.missed == 0 && cur.max_gap == 0);
  assert(link::lag_observe(cur, 2) == 0);
  assert(cur.received == 2 && cur.missed == 0);
  assert(link::lag_observe(cur, 5) == 2); // frames 3,4 overwritten unseen
  assert(cur.received == 3 && cur.missed == 2 && cur.max_gap == 2);
  assert(cur.last_seq == 5);
  assert(link::lag_observe(cur, 6) == 0);
  assert(cur.received == 4 && cur.missed == 2 && cur.max_gap == 2);

  // First observation counts pre-attach frames as missed (sample display).
  link::LagStats late;
  assert(link::lag_observe(late, 1416) == 1415);
  assert(late.received == 1 && late.missed == 1415 && late.max_gap == 1415);
}

void test_header_counters() {
  link::SegmentHeader h;
  assert(link::init_header(h, "projectile").ok());
  link::header_note_publish(h);
  link::header_note_publish(h);
  link::header_note_publish(h);
  assert(link::header_published(h) == 3);
}

// PHYSI-30: the header version is the commit word, published last with
// release semantics. A concurrent attacher must observe only unready
// (version 0) or a complete valid header, never a ready version with
// incomplete fields (false incompatibility).
void test_header_init_staged_unready() {
  link::SegmentHeader h;
  // Fresh header is unready: version 0 is never a verdict.
  assert(h.version.load(std::memory_order_acquire) == 0);
  link::HeaderReport fresh = link::inspect_header(h);
  assert(!fresh.status.ok());
  assert(fresh.issue == link::HeaderIssue::kVersionMismatch);
  assert(fresh.found_version == 0);

  // Stage the init_header field writes in order, leaving version untouched.
  // After every stage the header must still report unready, never a
  // capacity/domain refusal under a ready version.
  const char *domain = "projectile";
  std::size_t len = std::string(domain).size();
  for (std::size_t i = 0; i < len; ++i) {
    h.domain[i] = domain[i];
    assert(h.version.load(std::memory_order_acquire) == 0);
    const link::HeaderReport rep = link::inspect_header(h);
    assert(!rep.status.ok());
    assert(rep.issue == link::HeaderIssue::kVersionMismatch);
    assert(rep.found_version == 0);
  }
  h.domain[len] = '\0';
  h.max_items = static_cast<std::uint64_t>(link::kMaxItems);
  {
    const link::HeaderReport rep = link::inspect_header(h);
    assert(!rep.status.ok() && rep.issue == link::HeaderIssue::kVersionMismatch);
    assert(rep.found_version == 0);
  }
  h.max_channels = static_cast<std::uint64_t>(link::kMaxChannels);
  h.slot_count = static_cast<std::uint64_t>(link::kSlotCount);
  h.publish_count.store(0, std::memory_order_relaxed);
  {
    const link::HeaderReport rep = link::inspect_header(h);
    assert(!rep.status.ok() && rep.issue == link::HeaderIssue::kVersionMismatch);
    assert(rep.found_version == 0);
  }

  // Exactly one final release-store publishes kVersion; the acquire gate
  // then observes the complete valid header.
  h.version.store(link::kVersion, std::memory_order_release);
  const link::HeaderReport done = link::inspect_header(h);
  assert(done.status.ok() && done.issue == link::HeaderIssue::kNone);
  assert(done.found_version == link::kVersion);
  assert(link::check_header(h).ok());
}

void test_header_init_concurrent_attach() {
  // Staged writer with delays so the reader provably overlaps
  // initialization; the reader classifies every observation. Race-free by
  // construction: inspect_header returns on version != kVersion before
  // touching plain fields, and plain writes are sequenced before the final
  // release-store the reader acquires.
  link::SegmentHeader h;
  std::atomic<bool> writer_done{false};
  std::atomic<bool> saw_unready{false};

  std::thread writer([&]() {
    h.version.store(0, std::memory_order_relaxed);
    const char *domain = "projectile";
    const std::size_t len = std::string(domain).size();
    for (std::size_t i = 0; i < len; ++i) {
      h.domain[i] = domain[i];
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    h.domain[len] = '\0';
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    h.max_items = static_cast<std::uint64_t>(link::kMaxItems);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    h.max_channels = static_cast<std::uint64_t>(link::kMaxChannels);
    h.slot_count = static_cast<std::uint64_t>(link::kSlotCount);
    h.publish_count.store(0, std::memory_order_relaxed);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    h.version.store(link::kVersion, std::memory_order_release);
    writer_done.store(true, std::memory_order_release);
  });

  while (!writer_done.load(std::memory_order_acquire)) {
    const link::HeaderReport rep = link::inspect_header(h);
    if (rep.found_version == 0) {
      // Mid-construction: unready, never a verdict.
      assert(!rep.status.ok());
      assert(rep.issue == link::HeaderIssue::kVersionMismatch);
      saw_unready.store(true, std::memory_order_relaxed);
    } else {
      // A ready version must mean a complete valid header, never a false
      // capacity/domain refusal from torn init fields.
      assert(rep.found_version == link::kVersion);
      assert(rep.status.ok());
      assert(rep.issue == link::HeaderIssue::kNone);
    }
  }
  writer.join();
  assert(saw_unready.load(std::memory_order_relaxed));
  const link::HeaderReport done = link::inspect_header(h);
  assert(done.status.ok() && done.issue == link::HeaderIssue::kNone);
  assert(link::check_header(h).ok());
}

// Hammers one ring from a writer thread while the main thread reads every
// slot: only kOk frames are inspected, and each must be self-consistent
// (step == seq, payload matches the writer's function of seq). Any torn
// byte escaping slot_try_read would break self-consistency.
void test_seqlock_concurrent() {
  constexpr std::uint64_t kFrames = 200;
  std::array<link::Slot, link::kSlotCount> ring{};
  for (auto &s : ring) {
    s.seq.store(0, std::memory_order_relaxed);
  }
  link::SegmentHeader header;
  assert(link::init_header(header, "circuit").ok()); // any domain: generic

  std::thread writer([&]() {
    for (std::uint64_t i = 1; i <= kFrames; ++i) {
      link::Frame staged;
      staged.step = i; // step == seq by construction
      staged.time = static_cast<double>(i) * 0.01;
      link::FrameItem it;
      it.x = static_cast<double>(i) * 0.25;
      it.y = static_cast<double>(i) * 0.5;
      staged.items.push_back(it);
      staged.channels.push_back({"probe", static_cast<double>(i) * 1.5});
      link::LinkFrame wire{};
      assert(link::encode_frame(staged, wire, i).ok());
      const std::size_t slot = static_cast<std::size_t>(
          link::ring_slot(i, static_cast<std::uint64_t>(link::kSlotCount)));
      link::slot_begin_write(ring[slot]);
      ring[slot].frame = wire;
      link::slot_end_write(ring[slot]);
      link::header_note_publish(header);
    }
  });

  std::uint64_t max_seen = 0;
  for (std::uint64_t spins = 0; spins < 50000000ULL && max_seen != kFrames; ++spins) {
    for (auto &s : ring) {
      link::LinkFrame got{};
      if (link::slot_try_read(s, got) != link::SlotRead::kOk) {
        continue;
      }
      if (got.frame_seq <= max_seen || got.frame_seq > kFrames) {
        continue; // stale re-read or impossible: ignore
      }
      // Self-consistency of a clean read.
      assert(got.step_index == got.frame_seq);
      assert(got.item_count == 1 && got.channel_count == 1);
      assert(got.items[0].x == static_cast<double>(got.frame_seq) * 0.25);
      assert(got.items[0].y == static_cast<double>(got.frame_seq) * 0.5);
      link::Frame back;
      assert(link::decode_frame(got, back).ok());
      assert(back.step == got.frame_seq);
      assert(back.channels.size() == 1 && back.channels[0].name == "probe");
      assert(back.channels[0].value == static_cast<double>(got.frame_seq) * 1.5);
      max_seen = got.frame_seq;
    }
  }
  writer.join();
  assert(max_seen == kFrames);
  assert(link::header_published(header) == kFrames);
}

} // namespace

int main() {
  test_header_init_and_check();
  test_compat_matrix();
  test_encode_decode_roundtrip();
  test_particle_adapter();
  test_encode_over_bound();
  test_seqlock_states();
  test_ring_and_lag();
  test_lag_stats();
  test_header_counters();
  test_header_init_staged_unready();
  test_header_init_concurrent_attach();
  test_seqlock_concurrent();
  std::cout << "test_link_protocol: OK\n";
  return 0;
}
