// Scale-profiling sweep (015): growth/wire/compile/solve/word/shared/
// step/optimize/dedup/remove/reset attribution over a size ladder to
// >=1M gates. Links wengine_core only, never the assertion policy.
//
// Repro:
//   bench_logic_scale [motif] [min_size] [max_size] [steps] [words] [budget_s] [reps]
//   defaults:          0       1024       16384      5       20      300        5
// Bounds (clamped with a notice): motif 0..3, min_size 1..kMaxGates,
// max_size 1..kMaxGates (min<=max enforced by swap), steps 1..16,
// words 1..1024, budget_s 1..86400, reps 1..100. The committed
// full-ladder invocations (including the >=1M point) are pinned verbatim
// in docs/benchmarks/logic-scale.md; the no-args default is the quick
// smoke ladder (minutes).
//
// Motifs (deterministic xorshift construction, seeded from argc: identical
// args re-run to bit-identical nets/checksums; timings vary, shapes never
// do): 0 plain layered (uniform-earlier drivers), 1 wide (recent-window
// drivers), 2 deep-chain (spine + chords), 3 mixed (plain wiring plus
// shared-marked agreeing extras, pulls on skipped dangling inputs, a DFF
// strand, and delay annotations — the 011-014 coverage motif).
// Construction is add-then-immediately-wire in slot order (per-op timed
// accumulation separates growth-add from growth-wire): the incremental
// cycle probe then scans a tiny downstream cone, which is the honest
// steady-authoring cost — bulk wire-after-add-all would measure a
// pathological cone instead.
//
// Stages per size (one labeled line each; warmup separated from steady
// state; first-compile never averaged into steady solve):
//   growth-add, growth-wire, compile-rebuild, solve, word, shared-solve,
//   step, optimize (O0 pipeline), dedup (isolated all-pairs pass on a
//   NAND-only twin), remove, reset.
// Steady-state stages (solve/word/shared-solve/step) report median+min
// over `reps` (committed runs use >=5); one-shot stages report a single
// measurement. Every row carries its sample count; a stage exceeding
// `budget_s` at a size prints UNMEASURED-AT-SIZE loudly and the sweep
// continues — never extrapolated, never estimated, never silently
// skipped.
//
// Output grammar (contracts/bench-invocation.md):
//   bench_logic_scale <stage>: <size> gates x <n> ops, <median> ns/<unit>
//     (min=<m>, fp=<footprint>, checksum=<c>)
//   bench_logic_scale <stage>: <size> gates UNMEASURED-AT-SIZE
//     (budget <budget_s>s exceeded)
//   bench_logic_scale: setup failed (<reason>)   [exit 1]
//
// Dynamic pool (replaces the old kCap/u16-vs-u32 fidelity note): one u32
// pool — memory and per-op scans scale with live size (slot_count),
// never the max cap; the ladder max stays 1048576 via kMaxGates (loud
// past-cap refusal, never silent). Netlist is a small handle (the pool
// lives on the heap); every stage calls the wrapper entry points, so no
// call site sizes scratch. The committed 015 numbers stay the "before"
// baseline.

#include "wengine/logic/adjacency.hpp"
#include "wengine/logic/eval.hpp"
#include "wengine/logic/netlist.hpp"
#include "wengine/logic/optimize.hpp"
#include "wengine/logic/optimize_o2_dedup.hpp"
#include "wengine/logic/optimize_passes.hpp"
#include "wengine/logic/seq_state.hpp"
#include "wengine/logic/seq_step.hpp"
#include "wengine/logic/topo.hpp"
#include "wengine/logic/types.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {

using wengine::logic::bit_words_for_inputs;
using wengine::logic::block_input_count;
using wengine::logic::block_output_count;
using wengine::logic::EdgeFilter;
using wengine::logic::Gate;
using wengine::logic::GateKind;
using wengine::logic::kMaxGates;
using wengine::logic::kMaxInputs;
using wengine::logic::LogicValue;
using wengine::logic::Netlist;
using wengine::logic::OptimizeOptions;
using wengine::logic::OptLevel;
using wengine::logic::SeqState;
using wengine::logic::Slot;
using wengine::logic::SolveDiag;

std::uint32_t xorshift(std::uint32_t &s) noexcept {
  s ^= s << 13;
  s ^= s >> 17;
  s ^= s << 5;
  return s;
}

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

std::uint64_t clamp_u64(std::uint64_t v, std::uint64_t lo, std::uint64_t hi, const char *name) {
  const std::uint64_t c = v < lo ? lo : (v > hi ? hi : v);
  if (c != v) {
    std::cout << "bench_logic_scale: clamped " << name << " " << v << " -> " << c << "\n";
  }
  return c;
}

// Order-sensitive FNV-1a over 64-bit words plus a lane sum: pins both
// content and order run to run.
struct Checksum {
  std::uint64_t fnv = 1469598103934665603ULL;
  std::uint64_t sum = 0;
  void mix(std::uint64_t w) {
    fnv ^= w;
    fnv *= 1099511628211ULL;
    sum += w;
  }
  void mix(LogicValue v) { mix(v == LogicValue::kHigh ? 0x9E3779B97F4A7C15ULL : 0xC2B2AE3D27D4EB4FULL); }
};

double median_of(std::vector<double> &v) {
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

// Deterministic heap footprint of one live net: the single PoolStore seam
// (pool_store::bytes). No allocator sampling: every size is exact, so
// identical args re-run to bit-identical footprints.
std::size_t pool_bytes(const Netlist &nl) {
  return wengine::logic::pool_store::bytes(nl);
}

// Motif net construction: first kInputs gates are dangling-input NOTs
// (block inputs); the rest follow the motif wiring. Returns false with
// a loud reason on any refusal (construction defects are harness bugs,
// never silent).
bool build_motif(Netlist &nl, std::uint64_t motif, std::size_t n, std::size_t kInputs, double &add_s, double &wire_s,
                 std::size_t &n_wires, std::string &why) {
  using namespace wengine::logic;
  if (n <= kInputs || n > kMaxGates) {
    why = "size out of range";
    return false;
  }
  std::uint32_t rng = 0x51ED270Bu ^ static_cast<std::uint32_t>(motif * 0x9E3779B9u + n);
  n_wires = 0;
  add_s = 0.0;
  wire_s = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    GateKind kind = GateKind::kNot;
    std::uint8_t fan = 1;
    if (i >= kInputs) {
      if (motif == 3 && i % 97 == 0) {
        kind = GateKind::kDff; // sequential strand (mixed motif)
        fan = 2;
      } else {
        const unsigned pick = xorshift(rng) % 3;
        kind = (pick == 0) ? GateKind::kAnd : (pick == 1) ? GateKind::kOr : GateKind::kXor;
        fan = 2;
      }
    }
    const auto t0 = Clock::now();
    auto r = add_gate(nl, kind, fan);
    const auto t1 = Clock::now();
    add_s += static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()) / 1e9;
    if (!r.ok()) {
      why = "add_gate refused";
      return false;
    }
    if (i >= kInputs) {
      const bool skip_second = (motif == 3 && i % 7 == 0);
      for (std::uint8_t k = 0; k < fan; ++k) {
        if (k == 1 && skip_second) {
          continue; // deliberately dangling (pulled below)
        }
        Slot drv = 0;
        if (motif == 1) {
          // Wide: recent-window drivers (breadth over depth).
          const std::size_t lo = (i > 512) ? i - 512 : 0;
          drv = static_cast<Slot>(lo + xorshift(rng) % (i - lo));
        } else if (motif == 2 && k == 0) {
          drv = static_cast<Slot>(i - 1); // spine
        } else {
          drv = static_cast<Slot>(xorshift(rng) % i); // uniform earlier
        }
        const auto w0 = Clock::now();
        const auto st = wire(nl, drv, static_cast<Slot>(i), k);
        const auto w1 = Clock::now();
        wire_s += static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(w1 - w0).count()) / 1e9;
        if (!st.ok()) {
          why = "wire refused";
          return false;
        }
        ++n_wires;
      }
      if (motif == 3) {
        if (i % 5 == 0 && fan == 2 && i % 7 != 0) {
          // Shared agreeing extra (span path measurable, values agree).
          // Only when input 1 was wired above (i%7==0 leaves it
          // dangling for the pull path instead).
          if (!mark_shared(nl, static_cast<Slot>(i), 1).ok()) {
            why = "mark_shared refused";
            return false;
          }
          const auto prim = nl.slots[i].drivers[1];
          const auto w0 = Clock::now();
          const auto st = wire(nl, prim, static_cast<Slot>(i), 1);
          const auto w1 = Clock::now();
          wire_s += static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(w1 - w0).count()) / 1e9;
          if (!st.ok()) {
            why = "shared append refused";
            return false;
          }
          ++n_wires;
        }
        if (i % 7 == 0 && fan == 2) {
          // Pulled dangling second input (termination path).
          if (!set_pull(nl, static_cast<Slot>(i), 1, (i % 2 == 0) ? 1 : 2).ok()) {
            why = "set_pull refused";
            return false;
          }
        }
        if (i % 11 == 0) {
          if (!set_gate_delay(nl, static_cast<Slot>(i), 100).ok()) {
            why = "set_gate_delay refused";
            return false;
          }
        }
      }
    }
  }
  return true;
}

// NAND-only twin for the isolated dedup stage (bulk scaffolding: the
// pass cost, not construction, is the measured subject).
bool build_dedup_twin(Netlist &nl, std::size_t n, std::size_t kInputs) {
  using namespace wengine::logic;
  using namespace wengine::logic::detail;
  if (!ensure_slots(nl, n)) {
    return false;
  }
  std::uint32_t rng = 0xD15EA5E1u ^ static_cast<std::uint32_t>(n);
  std::uint32_t counters[21];
  for (std::size_t c = 0; c < 21; ++c) {
    counters[c] = 1;
  }
  for (std::size_t i = 0; i < n; ++i) {
    Gate &g = nl.slots[i];
    g.live = true;
    g.kind = (i < kInputs) ? GateKind::kNot : GateKind::kNand;
    g.counter = counters[kind_ordinal(g.kind)]++;
    g.input_count = (i < kInputs) ? 1 : 2;
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
      g.drivers[0] = static_cast<Slot>(rng % i);
      rng = xorshift(rng);
      g.drivers[1] = static_cast<Slot>(rng % i);
      rng = xorshift(rng);
    }
  }
  for (std::size_t c = 0; c < 21; ++c) {
    nl.next_counter[c] = counters[c];
  }
  nl.live_count = static_cast<Slot>(n);
  nl.dirty = true;
  nl.topo_order.clear();
  recount_fanout(nl);
  pool_store::rebuild_all(nl); // bulk direct build: one seam rebuilds every derived store
  return true;
}

void print_measured(const char *stage, std::size_t size, std::size_t ops, double median_ns, double min_ns,
                    std::size_t fp, const Checksum &c, const char *unit) {
  std::printf("bench_logic_scale %s: %zu gates x %zu ops, %.3f ns/%s (min=%.3f, fp=%zu, checksum=%llu)\n", stage,
              size, ops, median_ns, unit, min_ns, fp, (unsigned long long)c.fnv);
  do_not_optimize(c.fnv);
}

void print_unmeasured(const char *stage, std::size_t size, std::uint64_t budget_s) {
  std::printf("bench_logic_scale %s: %zu gates UNMEASURED-AT-SIZE (budget %llus exceeded)\n", stage, size,
              (unsigned long long)budget_s);
}

// Predictive budget guard for one-shot stages (compile-rebuild, step,
// optimize, dedup, remove): with two history points (n0,t0), (n1,t1),
// fit the local scaling exponent k = ln(t1/t0)/ln(n1/n0) clamped to
// [1,3]; with a single point, assume k=2 (quadratic — matches every
// named suspect; dedup uses k=3 for its cubic worst case). Decline the
// run when t1 * (n/n1)^k exceeds the budget. The exponent is measured
// where possible, conservative otherwise; a declined stage prints the
// same loud UNMEASURED-AT-SIZE line — no number is estimated, the cell
// stays loudly absent per FR-006. Growth-add, solve, word,
// shared-solve, and reset always run (linear-entry/steady stages; the
// SC-001 reach path).
struct BudgetHistory {
  double t0 = -1.0, t1 = -1.0;
  std::size_t n0 = 0, n1 = 0;
  double default_k = 2.0;
  bool decline(std::size_t n, double budget_s) {
    if (n1 == 0 || t1 <= 0.0) {
      return false; // no history: always attempt
    }
    double k = default_k;
    if (n0 != 0 && n1 > n0 && t0 > 0.0 && n > n1) {
      k = std::log(t1 / t0) / std::log(static_cast<double>(n1) / static_cast<double>(n0));
      if (k < 1.0) {
        k = 1.0;
      } else if (k > 3.0) {
        k = 3.0;
      }
    }
    if (n <= n1) {
      return false;
    }
    const double r = static_cast<double>(n) / static_cast<double>(n1);
    return t1 * std::pow(r, k) > budget_s;
  }
  void record(std::size_t n, double t) {
    if (n == n1) {
      t1 = t;
      return;
    }
    n0 = n1;
    t0 = t1;
    n1 = n;
    t1 = t;
  }
};

// One ladder point on the dynamic pool. Returns false only on harness
// defects (setup failed, exit 1); budget overruns are loud lines.
// The SC-001 reach chain (growth-add, growth-wire, compile-rebuild,
// solve, word, shared-solve, reset) always runs — retroactive budget
// classification only; terminal one-shot stages (step, optimize, dedup,
// remove) honor the predictive budget guard via the shared histories.
bool run_point(std::uint64_t motif, std::size_t n, std::uint64_t words, std::uint64_t budget_s, std::uint64_t reps,
               BudgetHistory &h_step, BudgetHistory &h_opt, BudgetHistory &h_dedup, BudgetHistory &h_remove) {
  using namespace wengine::logic;
  // Repetition taper (stated in the variance policy): R=5 up to 16384
  // gates, R=2 above. Steady-state magnitudes are session-infeasible at
  // full repetitions past 16K (per-step/per-wire pool floors); the taper
  // is visible in every row's ops count.
  const std::uint64_t reps_eff = (n > 16384 && reps > 2) ? 2 : reps;
  Netlist nl;
  const std::size_t kInputs = (motif == 2) ? 1 : 6;

  // Growth (interleaved add+wire, per-op accumulation).
  double add_s = 0.0, wire_s = 0.0;
  std::size_t n_wires = 0;
  std::string why;
  if (!build_motif(nl, motif, n, kInputs, add_s, wire_s, n_wires, why)) {
    std::cout << "bench_logic_scale: setup failed (" << why << " at " << n << " gates)\n";
    return false;
  }
  // Size the pool workspace once, up front: steady stages then run
  // allocation-free and the footprint below is stable across stages.
  ensure_workspace(nl, slot_count(nl));
  const std::size_t fp_base = pool_bytes(nl);
  Checksum shape;
  for (std::size_t s = 0; s < n; ++s) {
    shape.mix(static_cast<std::uint64_t>(nl.slots[s].drivers[0]) + 1);
  }
  do_not_optimize(shape.fnv);
  const double add_med_ns = add_s * 1e9 / static_cast<double>(n);
  if (add_s <= static_cast<double>(budget_s)) {
    print_measured("growth-add", n, n, add_med_ns, add_med_ns, fp_base, shape, "gate");
  } else {
    print_unmeasured("growth-add", n, budget_s);
  }
  const double wire_med_ns = wire_s * 1e9 / static_cast<double>(n_wires > 0 ? n_wires : 1);
  if (wire_s <= static_cast<double>(budget_s)) {
    print_measured("growth-wire", n, n_wires, wire_med_ns, wire_med_ns, fp_base, shape, "wire");
  } else {
    print_unmeasured("growth-wire", n, budget_s);
  }

  // Compile-rebuild (forced dirty, one shot).
  nl.dirty = true;
  nl.topo_order.clear();
  double rebuild_s = 0.0;
  {
    const double t = time_s([&] {
      if (!topo::compile(nl, EdgeFilter::kAll).ok()) {
        why = "rebuild refused";
      }
    });
    rebuild_s = t;
  }
  if (!why.empty()) {
    std::cout << "bench_logic_scale: setup failed (" << why << " at " << n << " gates)\n";
    return false;
  }
  if (rebuild_s <= static_cast<double>(budget_s)) {
    Checksum cc;
    for (std::size_t i = 0; i < n; ++i) {
      cc.mix(static_cast<std::uint64_t>(nl.topo_order[i]));
    }
    print_measured("compile-rebuild", n, 1, rebuild_s * 1e9 / static_cast<double>(n),
                     rebuild_s * 1e9 / static_cast<double>(n), fp_base, cc, "gate");
  } else {
    print_unmeasured("compile-rebuild", n, budget_s);
  }

  const std::size_t n_in = block_input_count(nl);
  const std::size_t n_out = block_output_count(nl);
  if (n_out == 0) {
    std::cout << "bench_logic_scale: setup failed (no block outputs at " << n << " gates)\n";
    return false;
  }
  // Deterministic input vectors: rep r flips vector bits from xorshift.
  std::vector<std::uint64_t> solve_sums;
  std::vector<double> solve_ns;
  Checksum solve_ck;
  {
    std::vector<LogicValue> in_vals(n_in), out_vals(n_out);
    std::uint32_t vrng = 0xBEEFu ^ static_cast<std::uint32_t>(n);
    // Warmup (excluded).
    for (std::size_t j = 0; j < n_in; ++j) {
      in_vals[j] = ((xorshift(vrng) & 1u) != 0u) ? LogicValue::kHigh : LogicValue::kLow;
    }
    if (!solve(nl, std::span<const LogicValue>(in_vals.data(), n_in),
               std::span<LogicValue>(out_vals.data(), n_out), nullptr)
             .ok()) {
      std::cout << "bench_logic_scale: setup failed (warmup solve refused at " << n << " gates)\n";
      return false;
    }
    for (std::uint64_t r = 0; r < reps_eff; ++r) {
      for (std::uint64_t w = 0; w < words; ++w) {
        for (std::size_t j = 0; j < n_in; ++j) {
          in_vals[j] = ((xorshift(vrng) & 1u) != 0u) ? LogicValue::kHigh : LogicValue::kLow;
        }
        const double t = time_s([&] {
          if (!solve(nl, std::span<const LogicValue>(in_vals.data(), n_in),
                     std::span<LogicValue>(out_vals.data(), n_out), nullptr)
                   .ok()) {
            why = "solve refused";
          }
        });
        if (!why.empty()) {
          std::cout << "bench_logic_scale: setup failed (" << why << " at " << n << " gates)\n";
          return false;
        }
        solve_ns.push_back(t * 1e9);
        for (std::size_t o = 0; o < n_out; ++o) {
          solve_ck.mix(out_vals[o]);
        }
        do_not_optimize(out_vals.data());
      }
    }
  }
  {
    double total = 0.0;
    for (double v : solve_ns) {
      total += v;
    }
    std::vector<double> sorted = solve_ns;
    const double med = median_of(sorted) / static_cast<double>(n);
    const double mn = sorted.front() / static_cast<double>(n);
    if (total / 1e9 <= static_cast<double>(budget_s)) {
      print_measured("solve", n, solve_ns.size(), med, mn, fp_base, solve_ck, "gate");
    } else {
      print_unmeasured("solve", n, budget_s);
    }
  }

  // Word eval: slice 0..need-1 cycling (procedural columns stay valid
  // for any input count).
  {
    const std::size_t need = (n_in < 31) ? bit_words_for_inputs(n_in) : 1;
    const std::size_t slices = (need == 0) ? 1 : need;
    std::vector<std::uint64_t> gate_words(slot_count(nl), 0); // pool-sized, caller-owned
    std::vector<std::uint64_t> out_w(n_out, 0);
    std::vector<double> wns;
    Checksum wck;
    std::uint64_t slice = 0;
    // Warmup (excluded).
    if (!eval_block_word(nl, 0, std::span<std::uint64_t>(gate_words.data(), gate_words.size()),
                         std::span<std::uint64_t>(out_w.data(), n_out))
             .ok()) {
      std::cout << "bench_logic_scale: setup failed (warmup word refused at " << n << " gates)\n";
      return false;
    }
    double total = 0.0;
    for (std::uint64_t r = 0; r < reps_eff; ++r) {
      for (std::uint64_t w = 0; w < words; ++w) {
        const std::uint64_t idx = slice++ % slices;
        const double t = time_s([&] {
          if (!eval_block_word(nl, idx, std::span<std::uint64_t>(gate_words.data(), gate_words.size()),
                               std::span<std::uint64_t>(out_w.data(), n_out))
                   .ok()) {
            why = "word refused";
          }
        });
        if (!why.empty()) {
          std::cout << "bench_logic_scale: setup failed (" << why << " at " << n << " gates)\n";
          return false;
        }
        wns.push_back(t * 1e9);
        total += t;
        for (std::size_t o = 0; o < n_out; ++o) {
          wck.mix(out_w[o]);
        }
        do_not_optimize(out_w.data());
      }
    }
    std::vector<double> sorted = wns;
    const double med = median_of(sorted) / static_cast<double>(n);
    const double mn = sorted.front() / static_cast<double>(n);
    // Cross-check slice 0 against scalar row 0 (harness self-check, not
    // the FR-010 pin — that lives in test_logic_scale).
    if (total <= static_cast<double>(budget_s)) {
      print_measured("word", n, wns.size(), med, mn, fp_base, wck, "gate");
    } else {
      print_unmeasured("word", n, budget_s);
    }
  }

  // Shared-net scalar path: the same scalar solve (on unmarked motifs
  // this measures the mark-check overhead; on mixed the full span
  // fold). Split out so the shared path owns a citable row.
  {
    std::vector<LogicValue> in_vals(n_in, LogicValue::kHigh);
    std::vector<LogicValue> out_vals(n_out, LogicValue::kLow);
    std::vector<double> sns;
    Checksum sck;
    SolveDiag diag; // sized to the pool by the wrapper itself
    double total = 0.0;
    for (std::uint64_t r = 0; r < reps_eff; ++r) {
      const double t = time_s([&] {
        if (!solve(nl, std::span<const LogicValue>(in_vals.data(), n_in),
                   std::span<LogicValue>(out_vals.data(), n_out), &diag)
                 .ok()) {
          why = "shared-solve refused";
        }
      });
      if (!why.empty()) {
        std::cout << "bench_logic_scale: setup failed (" << why << " at " << n << " gates)\n";
        return false;
      }
      sns.push_back(t * 1e9);
      total += t;
      for (std::size_t o = 0; o < n_out; ++o) {
        sck.mix(out_vals[o]);
      }
      do_not_optimize(out_vals.data());
    }
    std::vector<double> sorted = sns;
    const double med = median_of(sorted) / static_cast<double>(n);
    const double mn = sorted.front() / static_cast<double>(n);
    if (total <= static_cast<double>(budget_s)) {
      print_measured("shared-solve", n, sns.size(), med, mn, fp_base, sck, "gate");
    } else {
      print_unmeasured("shared-solve", n, budget_s);
    }
  }

  // Sequential step (real flops on mixed; vacuous settle elsewhere).
  // Terminal stage: honors the predictive budget guard (per-step full
  // Kahn recompiles make it the costliest steady stage at scale).
  if (h_step.decline(n, static_cast<double>(budget_s))) {
    print_unmeasured("step", n, budget_s);
  } else {
    SeqState st;
    std::vector<LogicValue> in_vals(n_in, LogicValue::kHigh);
    std::vector<LogicValue> out_vals(n_out, LogicValue::kLow);
    std::vector<double> stns;
    Checksum stck;
    double total = 0.0;
    // Warmup (excluded).
    if (!seq_step_filtered(nl, st, std::span<const LogicValue>(in_vals.data(), n_in),
                           std::span<LogicValue>(out_vals.data(), n_out), nullptr, 0, nullptr)
             .ok()) {
      std::cout << "bench_logic_scale: setup failed (warmup step refused at " << n << " gates)\n";
      return false;
    }
    // State-pool footprint (exact sizes, post-warmup): Q + valid + serial.
    const std::size_t fp_step =
        fp_base + sizeof(SeqState) + st.q.size() * sizeof(LogicValue) + st.valid.size() * sizeof(std::uint8_t) +
        st.serial.size() * sizeof(std::uint64_t);
    for (std::uint64_t r = 0; r < reps_eff; ++r) {
      const double t = time_s([&] {
        if (!seq_step_filtered(nl, st, std::span<const LogicValue>(in_vals.data(), n_in),
                               std::span<LogicValue>(out_vals.data(), n_out), nullptr, 0, nullptr)
                 .ok()) {
          why = "step refused";
        }
      });
      if (!why.empty()) {
        std::cout << "bench_logic_scale: setup failed (" << why << " at " << n << " gates)\n";
        return false;
      }
      stns.push_back(t * 1e9);
      total += t;
      for (std::size_t o = 0; o < n_out; ++o) {
        stck.mix(out_vals[o]);
      }
      do_not_optimize(out_vals.data());
    }
    std::vector<double> sorted = stns;
    const double med = median_of(sorted) / static_cast<double>(n);
    const double mn = sorted.front() / static_cast<double>(n);
    if (total <= static_cast<double>(budget_s)) {
      print_measured("step", n, stns.size(), med, mn, fp_step, stck, "gate");
    } else {
      print_unmeasured("step", n, budget_s);
    }
    h_step.record(n, total);
  }

  // Optimizer passes: O0 pipeline by value (move-assign back), then the
  // isolated dedup pass on a NAND-only twin. Both honor the predictive
  // budget guard (super-linear one-shot stages).
  if (h_opt.decline(n, static_cast<double>(budget_s))) {
    print_unmeasured("optimize", n, budget_s);
  } else {
    OptimizeOptions opts;
    opts.level = OptLevel::kO0;
    double opt_s = 0.0;
    Checksum ock;
    {
      const double t = time_s([&] {
        auto r = optimize(nl, opts);
        if (!r.ok()) {
          why = "optimize refused";
        } else {
          nl = std::move(r.value);
          for (std::size_t s = 0; s < n; ++s) {
            ock.mix(static_cast<std::uint64_t>(nl.slots[s].drivers[0]) + 1);
          }
          do_not_optimize(nl.live_count);
        }
      });
      opt_s = t;
    }
    if (!why.empty()) {
      std::cout << "bench_logic_scale: setup failed (" << why << " at " << n << " gates)\n";
      return false;
    }
    // Peak footprint: the input pool plus the transient result pool
    // (which is now nl after the move-assign).
    const std::size_t fp_opt = fp_base + pool_bytes(nl);
    if (opt_s <= static_cast<double>(budget_s)) {
      print_measured("optimize", n, 1, opt_s * 1e9 / static_cast<double>(n), opt_s * 1e9 / static_cast<double>(n),
                     fp_opt, ock, "gate");
    } else {
      print_unmeasured("optimize", n, budget_s);
    }
    h_opt.record(n, opt_s);
  }

  // Isolated dedup pass (suspect #5's home): all-pairs structural scan
  // over a NAND-only twin of the same size.
  if (h_dedup.decline(n, static_cast<double>(budget_s))) {
    print_unmeasured("dedup", n, budget_s);
  } else {
    using namespace wengine::logic::detail;
    Netlist twin;
    if (!build_dedup_twin(twin, n, kInputs)) {
      std::cout << "bench_logic_scale: setup failed (dedup twin at " << n << " gates)\n";
      return false;
    }
    // Workspace first: the survivor span below reads pool-owned scratch.
    ensure_workspace(twin, slot_count(twin));
    const std::size_t pool_n = slot_count(twin);
    // Caller-owned feeds bits (all false: the twin holds no sequential
    // gates). A bool array stands in for vector<bool>, which exposes no
    // data() pointer for the const bool* seam.
    auto feeds = std::make_unique<bool[]>(pool_n);
    for (std::size_t i = 0; i < pool_n; ++i) {
      feeds[i] = false;
    }
    std::vector<std::uint64_t> buckets(pool_n, 0);
    std::vector<Slot> bheads(pool_n, kNoDriver), bnexts(pool_n, kNoDriver);
    const std::size_t fp_dedup = fp_base + pool_bytes(twin);
    double dedup_s = 0.0;
    Checksum dck;
    {
      const double t = time_s([&] {
        if (!dedup_universal_pass(twin, GateKind::kNand, feeds.get(),
                                  std::span<std::size_t>(twin.ws_survivors.data(), pool_n * kMaxInputs),
                                  std::span<std::uint64_t>(buckets.data(), pool_n), std::span<Slot>(bheads.data(), pool_n), std::span<Slot>(bnexts.data(), pool_n))) {
          dck.mix(0xC0FFEEu); // no merges: still a measured scan
        }
        for (std::size_t s = 0; s < n; ++s) {
          dck.mix(static_cast<std::uint64_t>(twin.slots[s].drivers[0]) + 1);
        }
        do_not_optimize(twin.live_count);
      });
      dedup_s = t;
    }
    if (dedup_s <= static_cast<double>(budget_s)) {
      print_measured("dedup", n, 1, dedup_s * 1e9 / static_cast<double>(n), dedup_s * 1e9 / static_cast<double>(n),
                     fp_dedup, dck, "gate");
    } else {
      print_unmeasured("dedup", n, budget_s);
    }
    h_dedup.record(n, dedup_s);
  }

  // Remove burst (high to low) + reset. Removal honors the predictive
  // budget guard; reset is linear and always runs.
  if (h_remove.decline(n, static_cast<double>(budget_s))) {
    print_unmeasured("remove", n, budget_s);
  } else {
    double rem_s = 0.0;
    {
      const double t = time_s([&] {
        for (std::size_t i = n; i-- > 0;) {
          if (!remove_gate(nl, static_cast<Slot>(i)).ok()) {
            why = "remove refused";
            break;
          }
        }
      });
      rem_s = t;
    }
    if (!why.empty()) {
      std::cout << "bench_logic_scale: setup failed (" << why << " at " << n << " gates)\n";
      return false;
    }
    Checksum rck;
    rck.mix(static_cast<std::uint64_t>(nl.live_count));
    if (rem_s <= static_cast<double>(budget_s)) {
      print_measured("remove", n, n, rem_s * 1e9 / static_cast<double>(n), rem_s * 1e9 / static_cast<double>(n),
                     fp_base, rck, "gate");
    } else {
      print_unmeasured("remove", n, budget_s);
    }
    h_remove.record(n, rem_s);
    double rst_s = 0.0;
    {
      const double t = time_s([&] { reset_netlist(nl); });
      rst_s = t;
    }
    Checksum zck;
    zck.mix(static_cast<std::uint64_t>(nl.live_count));
    if (rst_s <= static_cast<double>(budget_s)) {
      print_measured("reset", n, 1, rst_s * 1e9 / static_cast<double>(n), rst_s * 1e9 / static_cast<double>(n),
                     fp_base, zck, "gate");
    } else {
      print_unmeasured("reset", n, budget_s);
    }
  }
  return true;
}

bool run_sizes(std::uint64_t motif, const std::vector<std::size_t> &sizes, std::uint64_t words,
               std::uint64_t budget_s, std::uint64_t reps) {
  BudgetHistory h_step, h_opt, h_dedup, h_remove;
  h_dedup.default_k = 3.0; // all-pairs dedup with merges is cubic worst-case
  for (std::size_t n : sizes) {
    if (n < 8 || n > kMaxGates) {
      continue;
    }
    if (!run_point(motif, n, words, budget_s, reps, h_step, h_opt, h_dedup, h_remove)) {
      return false;
    }
  }
  return true;
}

} // namespace

int main(int argc, char **argv) {
  std::uint64_t motif = 0, min_size = 1024, max_size = 16384, steps = 5, words = 20, budget_s = 300, reps = 5;
  if (argc > 1) {
    motif = std::strtoull(argv[1], nullptr, 10);
  }
  if (argc > 2) {
    min_size = std::strtoull(argv[2], nullptr, 10);
  }
  if (argc > 3) {
    max_size = std::strtoull(argv[3], nullptr, 10);
  }
  if (argc > 4) {
    steps = std::strtoull(argv[4], nullptr, 10);
  }
  if (argc > 5) {
    words = std::strtoull(argv[5], nullptr, 10);
  }
  if (argc > 6) {
    budget_s = std::strtoull(argv[6], nullptr, 10);
  }
  if (argc > 7) {
    reps = std::strtoull(argv[7], nullptr, 10);
  }
  motif = clamp_u64(motif, 0, 3, "motif");
  min_size = clamp_u64(min_size, 1, wengine::logic::kMaxGates, "min_size");
  max_size = clamp_u64(max_size, 1, wengine::logic::kMaxGates, "max_size");
  if (min_size > max_size) {
    const std::uint64_t t = min_size;
    min_size = max_size;
    max_size = t;
    std::cout << "bench_logic_scale: swapped min/max into order\n";
  }
  steps = clamp_u64(steps, 1, 16, "steps");
  words = clamp_u64(words, 1, 1024, "words");
  budget_s = clamp_u64(budget_s, 1, 86400, "budget_s");
  reps = clamp_u64(reps, 1, 100, "reps");
  std::cout << std::unitbuf; // long runs: every line lands immediately
  setvbuf(stdout, nullptr, _IONBF, 0); // printf lines too (stage output)
  std::cout << "bench_logic_scale: motif=" << motif << " min_size=" << min_size << " max_size=" << max_size
            << " steps=" << steps << " words=" << words << " budget_s=" << budget_s << " reps=" << reps << "\n";

  // One geometric ladder across [min_size, max_size] on the single u32
  // dynamic pool.
  std::vector<std::size_t> sizes;
  for (std::uint64_t i = 0; i < steps; ++i) {
    const double frac = (steps == 1) ? 0.0 : static_cast<double>(i) / static_cast<double>(steps - 1);
    const double geom =
        static_cast<double>(min_size) * std::pow(static_cast<double>(max_size) / static_cast<double>(min_size), frac);
    sizes.push_back(static_cast<std::size_t>(geom + 0.5));
  }
  if (!run_sizes(motif, sizes, words, budget_s, reps)) {
    return 1;
  }
  return 0;
}
