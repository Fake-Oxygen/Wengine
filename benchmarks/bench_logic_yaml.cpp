// Workspace save/load scaling (LOGIC-V2-SCALE4 follow-up, no format change):
// canonical YAML save_text/load_text wall times over a size ladder to
// 1M gates. Links wengine_core + wengine_logic_yaml, never the
// assertion policy.
//
// Repro:
//   bench_logic_yaml [size...]
//   defaults: 1024 10000 100000 1048576 (the 1M point takes ~1 min:
//   ~150 MB of canonical text through the yaml-cpp DOM).
// Each arg clamps to 1..kMaxGates (zero/non-numeric falls back to the
// default ladder). Construction is the deterministic layered motif
// (first 6 gates dangling-input NOTs; every later gate fan-in 2 from
// xorshift-earlier slots, so every prefix is a closed sub-net), built
// bulk slot-direct like test_logic_scale's parity net — construction is
// setup, never measured. One-shot stages: a single save + load per
// size, no averaging, no extrapolation.
//
// Output grammar (one labeled line per stage and size):
//   bench_logic_yaml save: <size> gates, <bytes> B, <s> s
//   bench_logic_yaml load: <size> gates, <bytes> B, <s> s, roundtrip=identical
//   bench_logic_yaml: setup failed (<reason>)   [exit 1]
//
// The load line fails loudly (exit 1) on any refusal, live-count
// mismatch, or save/load/save byte mismatch. Timings vary run to run;
// shapes, bytes, and the identical verdict never do.

#include "wengine/logic/adjacency.hpp"
#include "wengine/logic/netlist.hpp"
#include "wengine/logic/yaml_store.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

using wengine::logic::Gate;
using wengine::logic::GateKind;
using wengine::logic::kMaxGates;
using wengine::logic::LogicValue;
using wengine::logic::Netlist;
using wengine::logic::Slot;

template <typename T> inline void do_not_optimize(const T &v) {
#if defined(__GNUC__) || defined(__clang__)
  asm volatile("" : : "r,m"(v) : "memory");
#else
  volatile const T *p = &v;
  (void)p;
#endif
}

using Clock = std::chrono::high_resolution_clock;

template <typename F> double time_s(F &&f) {
  const auto t0 = Clock::now();
  f();
  const auto t1 = Clock::now();
  return static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()) / 1e9;
}

// FNV-1a over bytes: pins content run to run (timings vary, bytes never do).
std::uint64_t fnv_bytes(const std::string &s) {
  std::uint64_t h = 1469598103934665603ULL;
  for (unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ULL;
  }
  return h;
}

inline GateKind layered_kind(std::size_t i) {
  return (i % 3 == 0) ? GateKind::kAnd : (i % 3 == 1) ? GateKind::kOr : GateKind::kXor;
}

// Bulk layered build, slot-direct (mirrors test_logic_scale's parity
// net: same RNG, same slots, same counters). Setup only — never timed.
bool build_layered(Netlist &nl, std::size_t n, std::size_t kInputs) {
  using namespace wengine::logic;
  if (n <= kInputs || n > kMaxGates) {
    return false;
  }
  if (!ensure_slots(nl, n)) {
    return false;
  }
  std::uint32_t rng = 0xC0FFEEu;
  auto rnd = [&]() -> std::uint32_t {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
  };
  std::uint32_t counters[21];
  for (std::size_t c = 0; c < 21; ++c) {
    counters[c] = 1;
  }
  for (std::size_t i = 0; i < n; ++i) {
    const GateKind kind = (i < kInputs) ? GateKind::kNot : layered_kind(i);
    const std::uint8_t fan = (i < kInputs) ? 1 : 2;
    Gate &g = nl.slots[i];
    g.live = true;
    g.kind = kind;
    g.counter = counters[kind_ordinal(kind)]++;
    g.input_count = fan;
    g.ignored_mask = 0;
    g.ignore_output = false;
    g.ignore_nq_output = false;
    g.output_fanout = 0;
    g.nq_fanout = 0;
    g.propagation_delay_ps = 0;
    for (std::size_t k = 0; k < kMaxInputs; ++k) {
      g.drivers[k] = kNoDriver;
    }
    if (i >= kInputs) {
      for (std::uint8_t k = 0; k < 2; ++k) {
        g.drivers[k] = static_cast<Slot>(rnd() % i);
      }
    }
  }
  for (std::size_t c = 0; c < 21; ++c) {
    nl.next_counter[c] = counters[c];
  }
  nl.live_count = static_cast<std::uint32_t>(n);
  nl.dirty = true;
  // Fanout recount (motif has no NQ/shared edges, so a plain driver
  // fanout pass suffices; save/load never reads it, but the net stays
  // well-formed for future solve stages).
  for (std::size_t i = 0; i < n; ++i) {
    const Gate &g = nl.slots[i];
    for (std::uint8_t k = 0; k < g.input_count; ++k) {
      if (g.drivers[k] != kNoDriver) {
        ++nl.slots[g.drivers[k]].output_fanout;
      }
    }
  }
  pool_store::rebuild_all(nl); // bulk direct build: one seam rebuilds every derived store
  return true;
}

} // namespace

int main(int argc, char **argv) {
  using namespace wengine::logic;
  std::vector<std::size_t> sizes;
  for (int a = 1; a < argc; ++a) {
    const long v = std::atol(argv[a]);
    if (v >= 1 && static_cast<std::uint64_t>(v) <= kMaxGates) {
      sizes.push_back(static_cast<std::size_t>(v));
    }
  }
  if (sizes.empty()) {
    sizes = {1024, 10000, 100000, kMaxGates};
  }
  constexpr std::size_t kInputs = 6;
  for (std::size_t n : sizes) {
    Netlist nl;
    if (!build_layered(nl, n, kInputs)) {
      std::cout << "bench_logic_yaml: setup failed (build " << n << " gates)\n";
      return 1;
    }
    std::vector<LogicValue> inits(slot_count(nl), LogicValue::kLow);
    std::string text;
    const double save_s = time_s([&] { yaml::save_text(nl, inits, text); });
    do_not_optimize(text.size());
    const std::uint64_t save_sum = fnv_bytes(text);
    do_not_optimize(save_sum);
    std::cout << "bench_logic_yaml save: " << n << " gates, " << text.size() << " B, " << save_s << " s"
              << " (checksum=" << save_sum << ")\n";
    yaml::Workspace ws;
    std::string err;
    double load_s = 0.0;
    bool ok = false;
    load_s = time_s([&] { ok = yaml::load_text(text, ws, err); });
    do_not_optimize(ws.netlist.live_count);
    if (!ok) {
      std::cout << "bench_logic_yaml: load failed at " << n << " gates: " << err << "\n";
      return 1;
    }
    if (ws.netlist.live_count != static_cast<std::uint32_t>(n)) {
      std::cout << "bench_logic_yaml: live-count mismatch at " << n << " gates\n";
      return 1;
    }
    std::string again;
    yaml::save_text(ws.netlist, ws.init, again);
    const bool identical = (again == text);
    do_not_optimize(again.size());
    if (!identical) {
      std::cout << "bench_logic_yaml: round-trip mismatch at " << n << " gates\n";
      return 1;
    }
    std::cout << "bench_logic_yaml load: " << n << " gates, " << text.size() << " B, " << load_s << " s, roundtrip=identical\n";
  }
  return 0;
}
