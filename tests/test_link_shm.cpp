// SHM publisher: segment create/publish/read-back through a real POSIX
// segment, headless-run fan-out proving every emitted snapshot lands on
// the bus, drop-oldest under a slow reader, and loud failure paths.

#include "wengine/core/run_config.hpp"
#include "wengine/link/shm_publisher.hpp"
#include "wengine/solvers/run.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <iostream>
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

void test_move_transfers_ownership() {
  const std::string name = seg_name("move");
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

} // namespace

int main() {
  test_headless_publishes_every_emit();
  test_drop_oldest_under_slow_reader();
  test_bad_open_is_loud();
  test_over_bound_frame_dropped_loudly();
  test_move_transfers_ownership();
  std::cout << "test_link_shm: OK\n";
  return 0;
}
