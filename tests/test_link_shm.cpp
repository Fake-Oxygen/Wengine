// SHM publisher: segment create/publish/read-back through a real POSIX
// segment, headless-run fan-out proving every emitted snapshot lands on
// the bus, drop-oldest under a slow reader, and loud failure paths.

#include "wengine/core/run_config.hpp"
#include "wengine/link/shm_publisher.hpp"
#include "wengine/link/shm_reader.hpp"
#include "wengine/solvers/run.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <new>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

using namespace wengine;

namespace {

std::string seg_name(const char *tag) {
  return "/wengine-test-shm-" + std::string(tag) + "-" + std::to_string(::getpid());
}

// Read-only second mapping of a live segment: proves cross-mapping
// visibility (what an external reader would see) using only protocol
// primitives — no reader class involved.
struct ReaderMapping {
  int fd = -1;
  void *base = nullptr;
  std::size_t size = 0;
  const link::SegmentHeader *header = nullptr;
  const link::Slot *slots = nullptr;

  explicit ReaderMapping(const std::string &name) {
    size = sizeof(link::SegmentHeader) +
           link::kSlotCount * sizeof(link::Slot);
    fd = ::shm_open(name.c_str(), O_RDONLY, 0600);
    if (fd < 0) {
      return;
    }
    base = ::mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
      base = nullptr;
      ::close(fd);
      fd = -1;
      return;
    }
    header =
        static_cast<const link::SegmentHeader *>(base);
    slots = reinterpret_cast<const link::Slot *>(
        static_cast<const char *>(base) + sizeof(link::SegmentHeader));
  }

  ~ReaderMapping() {
    if (base != nullptr) {
      ::munmap(base, size);
    }
    if (fd >= 0) {
      ::close(fd);
    }
  }

  ReaderMapping(const ReaderMapping &) = delete;
  ReaderMapping &operator=(const ReaderMapping &) = delete;

  [[nodiscard]] bool ok() const noexcept { return base != nullptr; }
};

// Highest frame_seq among clean reads; false when the ring is still empty.
bool read_latest(const link::Slot *slots, link::LinkFrame &out) {
  bool found = false;
  std::uint64_t best = 0;
  for (std::size_t i = 0; i < link::kSlotCount; ++i) {
    link::LinkFrame got{};
    if (link::slot_try_read(slots[i], got) != link::SlotRead::kOk) {
      continue;
    }
    if (!found || got.frame_seq > best) {
      best = got.frame_seq;
      out = got;
      found = true;
    }
  }
  return found;
}

core::RunConfig batch_cfg(std::uint64_t steps, std::uint64_t emit_every) {
  core::RunConfig cfg;
  cfg.loop.dt = 0.01;
  cfg.loop.emit_every = emit_every;
  cfg.steps = steps;
  cfg.solver = "projectile";
  cfg.gravity = 9.81;
  cfg.particles = 1;
  return cfg;
}

void test_headless_publishes_every_emit() {
  // Acceptance path: run_headless + publisher; every emitted snapshot lands
  // on the bus and the latest decodes bit-exact to the final state.
  const std::string name = seg_name("batch");
  link::ShmPublisher pub(name, "projectile");
  assert(pub.ok());
  assert(pub.header() != nullptr && link::check_header(*pub.header()).ok());

  world::State final_state;
  std::array<emit::ISink *, 1> sinks{&pub};
  assert(solvers::run_headless(batch_cfg(5, 1), sinks, final_state).ok());
  assert(pub.ok());
  assert(pub.stats().published == 5);
  assert(pub.stats().dropped_over_bound == 0);
  assert(link::header_published(*pub.header()) == 5);

  ReaderMapping reader(name);
  assert(reader.ok());
  assert(link::check_header(*reader.header).ok());
  link::LinkFrame wire{};
  assert(read_latest(reader.slots, wire));
  assert(wire.frame_seq == 5);
  link::Frame got;
  assert(link::decode_frame(wire, got).ok());
  const link::Frame want =
      link::frame_from_snapshot(world::take_snapshot(final_state));
  assert(got == want); // bus content == engine truth, exactly
}

void test_drop_oldest_under_slow_reader() {
  // More frames than slots with no reader attached mid-run: the ring
  // overwrites (drop-oldest), the latest frame is intact, and the gap in
  // frame_seq accounts exactly for the overwritten ones.
  const std::string name = seg_name("lag");
  link::ShmPublisher pub(name, "projectile");
  assert(pub.ok());
  constexpr std::uint64_t kSteps = link::kSlotCount + 4;
  world::State unused;
  std::array<emit::ISink *, 1> sinks{&pub};
  assert(solvers::run_headless(batch_cfg(kSteps, 1), sinks, unused).ok());
  assert(pub.stats().published == kSteps);

  ReaderMapping reader(name);
  assert(reader.ok());
  link::LinkFrame wire{};
  assert(read_latest(reader.slots, wire));
  assert(wire.frame_seq == kSteps);
  assert(wire.step_index == kSteps);
  // A reader that saw only frame 1 before this read missed exactly the gap.
  assert(link::reader_missed(1, wire.frame_seq) == kSteps - 2);
}

void test_bad_open_is_loud() {
  link::ShmPublisher no_slash("no-leading-slash", "projectile");
  assert(!no_slash.ok());
  assert(no_slash.status().code == core::StatusCode::kInvalidArgument);
  assert(no_slash.header() == nullptr);

  link::ShmPublisher nested("/has/nested/slash", "projectile");
  assert(!nested.ok());

  link::ShmPublisher no_domain(seg_name("nodomain"), "");
  assert(!no_domain.ok());

  link::ShmPublisher null_adapter(seg_name("nulladapt"), "projectile", nullptr);
  assert(!null_adapter.ok());

  // Failed publishers hold nothing and never publish.
  world::Snapshot s;
  no_slash.on_snapshot(s); // must not crash, stays inert
  assert(!no_slash.ok());
}

void test_over_bound_frame_dropped_loudly() {
  const std::string name = seg_name("bound");
  link::ShmPublisher pub(name, "projectile");
  assert(pub.ok());

  // One good frame first.
  world::Snapshot small;
  small.step_index = 1;
  small.sim_time = 0.01;
  small.pos_x = {1.0};
  small.pos_y = {2.0};
  small.vel_x = {3.0};
  small.vel_y = {4.0};
  pub.on_snapshot(small);
  assert(pub.ok() && pub.stats().published == 1);

  // Over-bound frame: dropped + counted + sticky loud status.
  world::Snapshot big;
  big.step_index = 2;
  big.pos_x.assign(link::kMaxItems + 1, 1.0);
  big.pos_y.assign(link::kMaxItems + 1, 1.0);
  big.vel_x.assign(link::kMaxItems + 1, 1.0);
  big.vel_y.assign(link::kMaxItems + 1, 1.0);
  pub.on_snapshot(big);
  assert(!pub.ok());
  assert(pub.status().code == core::StatusCode::kOutOfRange);
  assert(pub.stats().dropped_over_bound == 1);
  assert(pub.stats().published == 1); // refused frame never hit the bus

  // The earlier good frame is still intact on the bus.
  ReaderMapping reader(name);
  assert(reader.ok());
  link::LinkFrame wire{};
  assert(read_latest(reader.slots, wire));
  assert(wire.frame_seq == 1 && wire.step_index == 1);
  link::Frame back;
  assert(link::decode_frame(wire, back).ok());
  assert(back == link::frame_from_snapshot(small));

  // Sticky: further snapshots are ignored, never half-published.
  pub.on_snapshot(small);
  assert(pub.stats().published == 1);
}

void test_move_transfers_ownership() {  const std::string name = seg_name("move");
  link::ShmPublisher moved(name, "projectile");
  assert(moved.ok());
  world::Snapshot s;
  s.step_index = 1;
  moved.on_snapshot(s);
  assert(moved.stats().published == 1);

  link::ShmPublisher target = std::move(moved);
  assert(target.ok() && target.stats().published == 1);
  assert(target.name() == name);
  target.on_snapshot(s);
  assert(target.stats().published == 2);
  // Moved-from is inert and destroys safely (no double unmap/unlink).
  assert(!moved.ok());
  moved.on_snapshot(s);
}

void test_reader_attach_and_latest() {
  const std::string name = seg_name("reader");
  link::ShmPublisher pub(name, "projectile");
  assert(pub.ok());
  world::State final_state;
  std::array<emit::ISink *, 1> sinks{&pub};
  assert(solvers::run_headless(batch_cfg(3, 1), sinks, final_state).ok());

  link::ShmReader reader(name);
  assert(reader.ok());
  assert(reader.state() == link::AttachState::kAttached);
  assert(reader.header() != nullptr);
  assert(link::check_header(*reader.header()).ok());
  assert(std::string(reader.header()->domain) == "projectile");
  assert(reader.published() == 3);

  // Latest bus frame decodes bit-exact to the engine's final state.
  link::Frame got;
  assert(reader.read_latest_frame(got));
  assert(got == link::frame_from_snapshot(world::take_snapshot(final_state)));

  // Cursor semantics: first poll yields the latest, second yields nothing.
  std::uint64_t cursor = 0;
  link::LinkFrame wire{};
  assert(reader.poll_newer(cursor, wire));
  assert(cursor == 3 && wire.step_index == 3);
  assert(!reader.poll_newer(cursor, wire));

  // Reader never unlinks: a second attach works while it is alive, and the
  // publisher is unaffected by reader lifetimes.
  {
    link::ShmReader second(name);
    assert(second.ok() && second.published() == 3);
  }
  assert(pub.ok() && pub.stats().published == 3);
}

void test_reader_rejects_foreign() {
  // Missing segment: loud failure, never a crash; retryable state.
  link::ShmReader missing("/wengine-test-shm-no-such-segment");
  assert(!missing.ok());
  assert(missing.state() == link::AttachState::kMissing);
  assert(missing.header() == nullptr && missing.published() == 0);

  // Wrong-sized segment (publisher mid-construction looks like this):
  // unready, retryable after the publisher is ready.
  const std::string name = seg_name("foreign");
  ::shm_unlink(name.c_str());
  const int fd = ::shm_open(name.c_str(), O_CREAT | O_RDWR, 0600);
  assert(fd >= 0);
  assert(::ftruncate(fd, 16) == 0);
  ::close(fd);
  link::ShmReader foreign(name);
  assert(!foreign.ok());
  assert(foreign.state() == link::AttachState::kUnready);
  assert(foreign.status().code == core::StatusCode::kInvalidArgument);
  ::shm_unlink(name.c_str());

  link::ShmReader bad_name("no-leading-slash");
  assert(!bad_name.ok());
  assert(bad_name.state() == link::AttachState::kBadName);
}

// Raw segment crafter: whatever bytes a foreign or mid-construction writer
// would leave behind. RAII: unmaps, closes, unlinks.
struct CraftedSegment {
  std::string name;
  int fd = -1;
  void *base = nullptr;
  std::size_t size = sizeof(link::SegmentHeader) +
                     link::kSlotCount * sizeof(link::Slot);
  link::SegmentHeader *header = nullptr;

  explicit CraftedSegment(const std::string &n) : name(n) {
    ::shm_unlink(name.c_str());
    fd = ::shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    assert(fd >= 0);
    assert(::ftruncate(fd, static_cast<off_t>(size)) == 0);
    base = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    assert(base != MAP_FAILED);
    header = new (base) link::SegmentHeader(); // all zeros: unready look
  }

  ~CraftedSegment() {
    ::munmap(base, size);
    ::close(fd);
    ::shm_unlink(name.c_str());
  }

  CraftedSegment(const CraftedSegment &) = delete;
  CraftedSegment &operator=(const CraftedSegment &) = delete;
};

void test_version_negotiation() {
  // New-reader / old-writer row: a v1 header (capacities intact, plain
  // payload) refuses fast with the found version reported — the clear
  // error. Same verdict on every attempt (deterministic negotiation,
  // nothing to retry). v1 and v2 share the byte size but never interop:
  // different synchronization.
  {
    CraftedSegment seg(seg_name("v1"));
    const char *domain = "projectile";
    for (std::size_t i = 0; domain[i] != '\0'; ++i) {
      seg.header->domain[i] = domain[i];
    }
    seg.header->max_items = static_cast<std::uint64_t>(link::kMaxItems);
    seg.header->max_channels = static_cast<std::uint64_t>(link::kMaxChannels);
    seg.header->slot_count = static_cast<std::uint64_t>(link::kSlotCount);
    seg.header->version.store(link::kVersionV1, std::memory_order_release);
    link::ShmReader reader(seg.name);
    assert(!reader.ok());
    assert(reader.state() == link::AttachState::kRefused);
    assert(reader.report().issue == link::HeaderIssue::kVersionMismatch);
    assert(reader.report().found_version == link::kVersionV1);
    assert(reader.header() == nullptr);
    // Repeatable: negotiation has no transient component.
    link::ShmReader retry(seg.name);
    assert(retry.state() == link::AttachState::kRefused);
  }
  // Old-reader / new-writer row: a future v3 header refuses the same way.
  {
    CraftedSegment seg(seg_name("v3"));
    const char *domain = "projectile";
    for (std::size_t i = 0; domain[i] != '\0'; ++i) {
      seg.header->domain[i] = domain[i];
    }
    seg.header->max_items = static_cast<std::uint64_t>(link::kMaxItems);
    seg.header->max_channels = static_cast<std::uint64_t>(link::kMaxChannels);
    seg.header->slot_count = static_cast<std::uint64_t>(link::kSlotCount);
    seg.header->version.store(link::kVersion + 1, std::memory_order_release);
    link::ShmReader reader(seg.name);
    assert(!reader.ok());
    assert(reader.state() == link::AttachState::kRefused);
    assert(reader.report().issue == link::HeaderIssue::kVersionMismatch);
    assert(reader.report().found_version == link::kVersion + 1);
    assert(reader.header() == nullptr);
    // Repeatable: negotiation has no transient component.
    link::ShmReader retry(seg.name);
    assert(retry.state() == link::AttachState::kRefused);
  }
  // Mid-construction look (right size, all zeros): unready, retryable —
  // never mistaken for a verdict. (Version 0 is reserved as the unready
  // marker and will never be assigned to a real writer.)
  {
    CraftedSegment seg(seg_name("mid"));
    link::ShmReader reader(seg.name);
    assert(!reader.ok());
    assert(reader.state() == link::AttachState::kUnready);
  }
  // Capacity drift under a matching version refuses as capacity, and a
  // good attach reports kAttached with a clean report.
  {
    CraftedSegment seg(seg_name("cap"));
    seg.header->domain[0] = 'p';
    seg.header->domain[1] = '\0';
    seg.header->max_items = 7;
    seg.header->max_channels = static_cast<std::uint64_t>(link::kMaxChannels);
    seg.header->slot_count = static_cast<std::uint64_t>(link::kSlotCount);
    seg.header->version.store(link::kVersion, std::memory_order_release);
    link::ShmReader reader(seg.name);
    assert(reader.state() == link::AttachState::kRefused);
    assert(reader.report().issue == link::HeaderIssue::kCapacityMismatch);
    assert(reader.report().found_version == link::kVersion);
  }
}

void test_reader_move() {  const std::string name = seg_name("rmove");
  link::ShmPublisher pub(name, "projectile");
  assert(pub.ok());
  world::State unused;
  std::array<emit::ISink *, 1> sinks{&pub};
  assert(solvers::run_headless(batch_cfg(2, 1), sinks, unused).ok());

  link::ShmReader moved(name);
  assert(moved.ok());
  link::ShmReader target = std::move(moved);
  assert(target.ok() && target.published() == 2);
  link::Frame got;
  assert(target.read_latest_frame(got));
  assert(got.step == 2);
  assert(!moved.ok()); // moved-from: inert, destroys safely
}

void test_attached_reader_survives_publisher_exit() {
  // PHYSI-43: the segment lives only while the publisher lives, but an
  // attached reader keeps its mapping across exit/unlink and still drains
  // the retained final frames. Attaching after the exit is kMissing by
  // design (a short flood can finish before a reader ever attaches).
  const std::string name = seg_name("exit");
  auto pub = std::make_unique<link::ShmPublisher>(name, "projectile");
  assert(pub->ok());
  link::ShmReader reader(name); // reader-first: attached before any publish
  assert(reader.ok());

  world::State unused;
  std::array<emit::ISink *, 1> sinks{pub.get()};
  assert(solvers::run_headless(batch_cfg(5, 1), sinks, unused).ok());
  assert(pub->stats().published == 5);

  // Live drain: latest-only jump to the final frame with exact accounting.
  std::uint64_t cursor = 0;
  link::LinkFrame wire{};
  assert(reader.poll_newer(cursor, wire));
  assert(cursor == 5 && wire.step_index == 5);
  assert(link::reader_missed(0, 5) == 4);
  assert(!reader.poll_newer(cursor, wire));

  pub.reset(); // publisher exit: unmaps, closes, unlinks the name.

  // The attached mapping still holds the retained final frame.
  link::LinkFrame retained{};
  assert(reader.read_latest(retained));
  assert(retained.frame_seq == 5 && retained.step_index == 5);
  assert(!reader.poll_newer(cursor, retained)); // cursor parked, no phantom
  assert(cursor == 5);

  // Late attach after the exit can never succeed: the bus is gone.
  link::ShmReader late(name);
  assert(!late.ok());
  assert(late.state() == link::AttachState::kMissing);
}

void test_stale_reader_parks_on_same_name_republish() {
  // PHYSI-43 endpoint-level mirror of the GUI reconnect test: after exit +
  // same-name republish, the stale mapping parks on the old run's final
  // frame (never advances, never corrupts); only a fresh attach — explicit
  // reconnect — sees the new run.
  const std::string name = seg_name("repub");
  world::State unused;
  auto pub = std::make_unique<link::ShmPublisher>(name, "projectile");
  assert(pub->ok());
  link::ShmReader stale(name);
  assert(stale.ok());
  std::array<emit::ISink *, 1> sinks{pub.get()};
  assert(solvers::run_headless(batch_cfg(2, 1), sinks, unused).ok());

  std::uint64_t cursor = 0;
  link::LinkFrame wire{};
  assert(stale.poll_newer(cursor, wire));
  assert(cursor == 2 && wire.step_index == 2);

  pub.reset(); // first run exits.
  pub = std::make_unique<link::ShmPublisher>(name, "projectile");
  assert(pub->ok());
  sinks[0] = pub.get();
  assert(solvers::run_headless(batch_cfg(5, 1), sinks, unused).ok());

  // Stale mapping: nothing newer than the old final; content frozen.
  assert(!stale.poll_newer(cursor, wire));
  assert(cursor == 2);
  link::LinkFrame frozen{};
  assert(stale.read_latest(frozen));
  assert(frozen.frame_seq == 2 && frozen.step_index == 2);

  // Fresh attach (reconnect) sees the new run's final frame.
  link::ShmReader fresh(name);
  assert(fresh.ok());
  link::LinkFrame latest{};
  assert(fresh.read_latest(latest));
  assert(latest.frame_seq == 5 && latest.step_index == 5);
}

} // namespace

int main() {
  test_headless_publishes_every_emit();
  test_drop_oldest_under_slow_reader();
  test_bad_open_is_loud();
  test_over_bound_frame_dropped_loudly();
  test_move_transfers_ownership();
  test_reader_attach_and_latest();
  test_reader_rejects_foreign();
  test_version_negotiation();
  test_reader_move();
  test_attached_reader_survives_publisher_exit();
  test_stale_reader_parks_on_same_name_republish();
  std::cout << "test_link_shm: OK\n";
  return 0;
}
