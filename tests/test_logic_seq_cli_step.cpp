// US1 step goldens: `step [DOMAIN] [N]` over session-built sequential
// nets (contracts/step.md). Drives the feature through `Shell::execute`
// one-shot lines (surface parity) plus direct `LogicStepSpec::run` calls
// (same-verb same-text). Links wengine_cli like test_cli_args.

#include "wengine/cli/logic_registry.hpp"
#include "wengine/cli/shell.hpp"
#include "wengine/core/status.hpp"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace wengine;

namespace {

bool contains(const std::string &hay, const std::string &needle) {
  return hay.find(needle) != std::string::npos;
}

void no_history(cli::Shell &sh) { sh.set_history_file(""); }

// Toggle flop: NOT1.Y -> DFF1.D, then DFF1.Q -> NOT1.1 (D-first order:
// the incremental cycle check trips on Q-fanout-first wiring), CLK
// ignored, NQ explicitly `use`d (ignored by default). Power-on Q=0;
// edge 1 latches D=NOT(0)=1.
void build_toggle(cli::Shell &sh, std::ostringstream &out) {
  assert(sh.execute("logic reset", out) == 0);
  assert(sh.execute("logic add DFF", out) == 0);
  assert(sh.execute("logic add NOT", out) == 0);
  assert(sh.execute("logic wire NOT1.Y DFF1.D", out) == 0);
  assert(sh.execute("logic wire DFF1.Q NOT1.1", out) == 0);
  assert(sh.execute("logic ignore DFF1.CLK", out) == 0);
  assert(sh.execute("logic use DFF1.NQ", out) == 0);
}

int run_step_direct(std::ostringstream &out, std::initializer_list<std::string_view> toks) {
  std::vector<std::string_view> v(toks);
  cli::ArgCursor cur{std::span<const std::string_view>(v.data(), v.size())};
  return cli::logic::LogicStepSpec::run(cur, out);
}

void test_advance_toggle() {
  cli::Shell sh;
  no_history(sh);
  std::ostringstream out;
  build_toggle(sh, out);
  out.str("");
  out.clear();
  assert(sh.execute("logic step 1", out) == 0);
  const std::string s1 = out.str();
  assert(contains(s1, "clk: cycle 1 (1 edges)"));
  assert(contains(s1, "DFF1.Q: 0->1 (NQ 1->0)"));
  assert(contains(s1, "DFF1.NQ=0"));
  // Second step toggles back; the counter accumulates.
  out.str("");
  out.clear();
  assert(sh.execute("logic step 1", out) == 0);
  const std::string s2 = out.str();
  assert(contains(s2, "clk: cycle 2 (1 edges)"));
  assert(contains(s2, "DFF1.Q: 1->0 (NQ 0->1)"));
  assert(contains(s2, "DFF1.NQ=1"));
  // Multi-edge advance in one call.
  out.str("");
  out.clear();
  assert(sh.execute("logic step 4", out) == 0);
  assert(contains(out.str(), "clk: cycle 6 (4 edges)"));
  // Zero-cycle advance reports without stepping.
  out.str("");
  out.clear();
  assert(sh.execute("logic step 0", out) == 0);
  assert(contains(out.str(), "clk: cycle 6 (0 edges, no-op)"));
}

void test_direct_matches_oneshot() {
  cli::Shell sh;
  no_history(sh);
  std::ostringstream out;
  build_toggle(sh, out);
  std::ostringstream one_shot, direct;
  assert(sh.execute("logic step 2", one_shot) == 0);
  // Rebuild: direct run must see the identical starting workspace.
  build_toggle(sh, out);
  assert(run_step_direct(direct, {"2"}) == 0);
  assert(one_shot.str() == direct.str());
}

void test_selective_domains() {
  cli::Shell sh;
  no_history(sh);
  std::ostringstream out;
  assert(sh.execute("logic reset", out) == 0);
  assert(sh.execute("logic add DFF", out) == 0);   // DFF1: implicit clk
  assert(sh.execute("logic add DFF", out) == 0);   // DFF2: driven clock
  assert(sh.execute("logic add BUF", out) == 0);
  assert(sh.execute("logic ignore DFF1.CLK", out) == 0);
  assert(sh.execute("logic wire BUF1.Y DFF2.CLK", out) == 0);
  assert(sh.execute("logic ignore DFF2.D", out) == 0);
  assert(sh.execute("logic ignore DFF1.D", out) == 0);
  out.str("");
  out.clear();
  assert(sh.execute("logic step BUF1.Y 2", out) == 0);
  const std::string s = out.str();
  assert(contains(s, "BUF1.Y: cycle 2 (2 edges)"));
  assert(contains(s, "DFF2.Q:"));
  assert(!contains(s, "DFF1.Q:")); // implicit-domain flop untouched
  // Global step advances both domains at once.
  out.str("");
  out.clear();
  assert(sh.execute("logic step 1", out) == 0);
  assert(contains(out.str(), "DFF1.Q:"));
  assert(contains(out.str(), "DFF2.Q:"));
  // Explicit implicit-domain advance only touches DFF1.
  out.str("");
  out.clear();
  assert(sh.execute("logic step clk 1", out) == 0);
  assert(contains(out.str(), "clk: cycle"));
  assert(contains(out.str(), "DFF1.Q:"));
  assert(!contains(out.str(), "DFF2.Q:"));
}

void test_errors_changeless() {
  cli::Shell sh;
  no_history(sh);
  std::ostringstream out;
  build_toggle(sh, out);
  // Negative count: usage error, counter untouched.
  assert(sh.execute("logic step --cycles -1", out) == 2);
  assert(contains(out.str(), "want N >= 0"));
  assert(sh.execute("logic step -1", out) == 2); // stray option-like token
  out.str("");
  out.clear();
  assert(sh.execute("logic step 1", out) == 0);
  assert(contains(out.str(), "clk: cycle 1 (1 edges)"));
  // Unknown clock: usage error naming the clock, state untouched.
  out.str("");
  out.clear();
  assert(sh.execute("logic step nosuchclk 3", out) == 2);
  assert(contains(out.str(), "unknown clock 'nosuchclk'"));
  out.str("");
  out.clear();
  assert(sh.execute("logic step 1", out) == 0);
  assert(contains(out.str(), "clk: cycle 2 (1 edges)"));
  // Reset clears counters: the next advance restarts at 1.
  assert(sh.execute("logic reset", out) == 0);
  build_toggle(sh, out);
  out.str("");
  out.clear();
  assert(sh.execute("logic step 1", out) == 0);
  assert(contains(out.str(), "clk: cycle 1 (1 edges)"));
  // No flops at all: runtime error, exit 1.
  assert(sh.execute("logic reset", out) == 0);
  assert(sh.execute("logic add AND", out) == 0);
  out.str("");
  out.clear();
  assert(sh.execute("logic step", out) == 1);
  assert(contains(out.str(), "no flops"));
}

void test_determinism_replay() {
  cli::Shell sh;
  no_history(sh);
  std::ostringstream out;
  build_toggle(sh, out);
  out.str("");
  out.clear();
  assert(sh.execute("logic step 100", out) == 0);
  const std::string run1 = out.str();
  assert(contains(run1, "clk: cycle 100 (100 edges)"));
  build_toggle(sh, out);
  out.str("");
  out.clear();
  assert(sh.execute("logic step 100", out) == 0);
  assert(out.str() == run1);
}

void test_nq_ignored_by_default() {
  // Without an explicit `use`, a dangling NQ stays out of step reports
  // and the block interface — while `list` shows it under ignored.
  cli::Shell sh;
  no_history(sh);
  std::ostringstream out;
  assert(sh.execute("logic reset", out) == 0);
  assert(sh.execute("logic add DFF", out) == 0);
  assert(sh.execute("logic ignore DFF1.CLK", out) == 0);
  assert(sh.execute("logic ignore DFF1.D", out) == 0);
  out.str("");
  out.clear();
  assert(sh.execute("logic step 1", out) == 0);
  assert(contains(out.str(), "clk: cycle 1 (1 edges)"));
  assert(contains(out.str(), "DFF1.Q: 0 (no change)"));
  assert(!contains(out.str(), "DFF1.NQ="));
  out.str("");
  out.clear();
  assert(sh.execute("logic list --iface", out) == 0);
  assert(contains(out.str(), "outputs (1):")); // Q only; NQ hidden
  assert(contains(out.str(), "DFF1.NQ"));      // ...but reported under ignored
}

void test_mixed_kinds_session() {
  // US3 (003-jk-t-flops, quickstart §5): one DFF + one JK + one T in one
  // clock domain — one advance steps every kind per its own table under a
  // single shared cycle count; replayed scripts read back identically.
  cli::Shell sh;
  no_history(sh);
  std::ostringstream out;
  assert(sh.execute("logic reset", out) == 0);
  assert(sh.execute("logic add DFF", out) == 0);
  assert(sh.execute("logic add JK", out) == 0);
  assert(sh.execute("logic add T", out) == 0);
  assert(sh.execute("logic ignore DFF1.CLK", out) == 0);
  assert(sh.execute("logic ignore JK1.CLK", out) == 0);
  assert(sh.execute("logic ignore T1.CLK", out) == 0);
  assert(sh.execute("logic poke DFF1.D=1", out) == 0);
  assert(sh.execute("logic poke JK1.J=1", out) == 0);
  assert(sh.execute("logic poke JK1.K=1", out) == 0);
  assert(sh.execute("logic poke T1.T=1", out) == 0);
  out.str("");
  out.clear();
  assert(sh.execute("logic step 2", out) == 0);
  assert(contains(out.str(), "clk: cycle 2 (2 edges)"));
  out.str("");
  out.clear();
  assert(sh.execute("logic peek", out) == 0);
  const std::string first = out.str();
  // D captured 1; JK toggled twice (back 0); T toggled twice (back 0).
  assert(contains(first, "DFF1.Q=1"));
  assert(contains(first, "JK1.Q=0 NQ=1"));
  assert(contains(first, "T1.Q=0 NQ=1"));
  // Identical rebuild + sequence reads back byte-identically.
  assert(sh.execute("logic reset", out) == 0);
  assert(sh.execute("logic add DFF", out) == 0);
  assert(sh.execute("logic add JK", out) == 0);
  assert(sh.execute("logic add T", out) == 0);
  assert(sh.execute("logic ignore DFF1.CLK", out) == 0);
  assert(sh.execute("logic ignore JK1.CLK", out) == 0);
  assert(sh.execute("logic ignore T1.CLK", out) == 0);
  assert(sh.execute("logic poke DFF1.D=1", out) == 0);
  assert(sh.execute("logic poke JK1.J=1", out) == 0);
  assert(sh.execute("logic poke JK1.K=1", out) == 0);
  assert(sh.execute("logic poke T1.T=1", out) == 0);
  assert(sh.execute("logic step 2", out) == 0);
  out.str("");
  out.clear();
  assert(sh.execute("logic peek", out) == 0);
  assert(out.str() == first);
}

void test_jk_counter_session() {
  // US2 (003-jk-t-flops): JK truth table through session verbs, a 2-bit
  // synchronous counter, and wrong-kind pin rejection.
  cli::Shell sh;
  no_history(sh);
  std::ostringstream out;
  assert(sh.execute("logic reset", out) == 0);
  assert(sh.execute("logic add JK", out) == 0);
  assert(sh.execute("logic ignore JK1.CLK", out) == 0);
  // Truth table from Q=0: hold, reset, set, toggle.
  assert(sh.execute("logic poke JK1.J=0", out) == 0);
  assert(sh.execute("logic poke JK1.K=0", out) == 0);
  assert(sh.execute("logic step 1", out) == 0);
  assert(sh.execute("logic peek JK1", out) == 0);
  assert(contains(out.str(), "JK1.Q=0 NQ=1"));
  assert(sh.execute("logic poke JK1.K=1", out) == 0); // 01 reset
  assert(sh.execute("logic step 1", out) == 0);
  assert(sh.execute("logic peek JK1", out) == 0);
  assert(contains(out.str(), "JK1.Q=0 NQ=1"));
  assert(sh.execute("logic poke JK1.J=1", out) == 0);
  assert(sh.execute("logic poke JK1.K=0", out) == 0); // 10 set
  assert(sh.execute("logic step 1", out) == 0);
  assert(sh.execute("logic peek JK1", out) == 0);
  assert(contains(out.str(), "JK1.Q=1 NQ=0"));
  assert(sh.execute("logic poke JK1.K=1", out) == 0); // 11 toggle
  assert(sh.execute("logic step 1", out) == 0);
  assert(sh.execute("logic peek JK1", out) == 0);
  assert(contains(out.str(), "JK1.Q=0 NQ=1"));
  // 2-bit synchronous counter: JK1 toggles every edge, JK2 on carry.
  out.str("");
  out.clear();
  assert(sh.execute("logic reset", out) == 0);
  assert(sh.execute("logic add JK", out) == 0);
  assert(sh.execute("logic add JK", out) == 0);
  assert(sh.execute("logic ignore JK1.CLK", out) == 0);
  assert(sh.execute("logic ignore JK2.CLK", out) == 0);
  assert(sh.execute("logic poke JK1.J=1", out) == 0);
  assert(sh.execute("logic poke JK1.K=1", out) == 0);
  assert(sh.execute("logic wire JK1.Q JK2.J", out) == 0);
  assert(sh.execute("logic wire JK1.Q JK2.K", out) == 0);
  const char *want[4] = {"JK1.Q=1 NQ=0", "JK1.Q=0 NQ=1", "JK1.Q=1 NQ=0", "JK1.Q=0 NQ=1"};
  const char *want_hi[4] = {"JK2.Q=0 NQ=1", "JK2.Q=1 NQ=0", "JK2.Q=1 NQ=0", "JK2.Q=0 NQ=1"};
  for (int i = 0; i < 4; ++i) {
    out.str("");
    out.clear();
    assert(sh.execute("logic step 1", out) == 0);
    assert(sh.execute("logic peek", out) == 0);
    assert(contains(out.str(), want[i]));
    assert(contains(out.str(), want_hi[i]));
  }
  // Wrong-kind pins are usage errors that change no state.
  assert(sh.execute("logic add DFF", out) == 0);
  out.str("");
  out.clear();
  assert(sh.execute("logic poke DFF1.J=1", out) == 2);
  out.str("");
  out.clear();
  assert(sh.execute("logic poke JK1.D=1", out) == 2);
  out.str("");
  out.clear();
  assert(sh.execute("logic peek JK1", out) == 0);
  assert(contains(out.str(), "JK1.Q=0 NQ=1")); // still wrapped 00
}

void test_t_divider_session() {
  // US1 (003-jk-t-flops): T divider through session verbs — add, poke,
  // wire (ripple chain), step, peek — plus hold rows and usage errors.
  cli::Shell sh;
  no_history(sh);
  std::ostringstream out;
  assert(sh.execute("logic reset", out) == 0);
  assert(sh.execute("logic add T", out) == 0);
  assert(sh.execute("logic add T", out) == 0);
  assert(sh.execute("logic ignore T1.CLK", out) == 0);
  assert(sh.execute("logic ignore T2.CLK", out) == 0);
  assert(sh.execute("logic poke T1.T=1", out) == 0);
  assert(contains(out.str(), "poked T1.T=1"));
  // Ripple divide-by-4: T1.Q feeds T2.T.
  assert(sh.execute("logic wire T1.Q T2.T", out) == 0);
  out.str("");
  out.clear();
  assert(sh.execute("logic step 4", out) == 0);
  const std::string stepped = out.str();
  assert(contains(stepped, "clk: cycle 4 (4 edges)"));
  assert(contains(stepped, "T1.Q:"));
  assert(contains(stepped, "T2.Q:"));
  out.str("");
  out.clear();
  assert(sh.execute("logic peek", out) == 0);
  const std::string first_peek = out.str();
  // 4 first-stage toggles land T1 back at 0; T2 saw T=1 on two edges
  // (OLD T1.Q was High at edges 2 and 4) and lands back at 0.
  assert(contains(first_peek, "T1.Q=0 NQ=1"));
  assert(contains(first_peek, "T2.Q=0 NQ=1"));
  // Hold: T=0 freezes T1 across 3 edges (T2 follows: T1.Q stays 0).
  assert(sh.execute("logic poke T1.T=0", out) == 0);
  out.str("");
  out.clear();
  assert(sh.execute("logic step 3", out) == 0);
  assert(contains(out.str(), "clk: cycle 7 (3 edges)"));
  out.str("");
  out.clear();
  assert(sh.execute("logic peek", out) == 0);
  assert(contains(out.str(), "T1.Q=0 NQ=1"));
  assert(contains(out.str(), "T2.Q=0 NQ=1"));
  // Invalid drive value is a usage error that changes no state.
  out.str("");
  out.clear();
  assert(sh.execute("logic poke T1.T=2", out) == 2);
  out.str("");
  out.clear();
  assert(sh.execute("logic peek", out) == 0);
  assert(contains(out.str(), "T1.Q=0 NQ=1"));
  // Deterministic replay: identical rebuild + sequence reads back equal.
  assert(sh.execute("logic reset", out) == 0);
  assert(sh.execute("logic add T", out) == 0);
  assert(sh.execute("logic add T", out) == 0);
  assert(sh.execute("logic ignore T1.CLK", out) == 0);
  assert(sh.execute("logic ignore T2.CLK", out) == 0);
  assert(sh.execute("logic poke T1.T=1", out) == 0);
  assert(sh.execute("logic wire T1.Q T2.T", out) == 0);
  assert(sh.execute("logic step 4", out) == 0);
  out.str("");
  out.clear();
  assert(sh.execute("logic peek", out) == 0);
  assert(out.str() == first_peek);
}

void test_complete() {
  cli::Shell sh;
  no_history(sh);
  std::ostringstream out;
  assert(sh.execute("logic reset", out) == 0);
  assert(sh.execute("logic add DFF", out) == 0);
  assert(sh.execute("logic add BUF", out) == 0);
  assert(sh.execute("logic wire BUF1.Y DFF1.CLK", out) == 0);
  assert(sh.execute("logic ignore DFF1.D", out) == 0);
  std::vector<std::string> names;
  {
    const std::span<const std::string_view> no_prior{};
    cli::logic::LogicStepSpec::complete(no_prior, "B", names);
    bool saw_clk = false;
    for (const std::string &n : names) {
      if (n == "BUF1.Y") {
        saw_clk = true;
      }
    }
    assert(saw_clk);
  }
  {
    const std::span<const std::string_view> no_prior{};
    names.clear();
    cli::logic::LogicStepSpec::complete(no_prior, "c", names);
    bool saw_implicit = false;
    for (const std::string &n : names) {
      if (n == "clk") {
        saw_implicit = true;
      }
    }
    // No dangling CLK pins here (DFF1.CLK is wired), so only BUF1.Y exists.
    assert(!saw_implicit);
  }
  {
    const std::span<const std::string_view> no_prior{};
    names.clear();
    cli::logic::LogicStepSpec::complete(no_prior, "--c", names);
    bool saw_cycles = false;
    for (const std::string &n : names) {
      if (n == "--cycles" || n == "--cycles=") {
        saw_cycles = true;
      }
    }
    assert(saw_cycles);
  }
}

void test_dlatch_level_session() {
  // US1 (004-sr-d-latches, quickstart §1): transparent follow with no
  // advance, frozen hold across drives and advances, re-enable tracking.
  cli::Shell sh;
  no_history(sh);
  std::ostringstream out;
  assert(sh.execute("logic reset", out) == 0);
  assert(sh.execute("logic add DLATCH", out) == 0);
  // Transparent: EN=1, then D=1 moves Q at once (level settle at poke).
  assert(sh.execute("logic poke DLATCH1.EN=1", out) == 0);
  assert(contains(out.str(), "poked DLATCH1.EN=1"));
  out.str("");
  out.clear();
  assert(sh.execute("logic poke DLATCH1.D=1", out) == 0);
  assert(contains(out.str(), "DLATCH1.Q: 0->1"));
  // Transparent: D=0 follows back at once.
  out.str("");
  out.clear();
  assert(sh.execute("logic poke DLATCH1.D=0", out) == 0);
  assert(contains(out.str(), "DLATCH1.Q: 1->0"));
  // Back to 1, then freeze: EN=0 holds Q across D toggles, no advance.
  assert(sh.execute("logic poke DLATCH1.D=1", out) == 0);
  out.str("");
  out.clear();
  assert(sh.execute("logic poke DLATCH1.EN=0", out) == 0);
  assert(!contains(out.str(), "DLATCH1.Q:")); // hold: no transition
  out.str("");
  out.clear();
  assert(sh.execute("logic poke DLATCH1.D=0", out) == 0);
  assert(!contains(out.str(), "DLATCH1.Q:")); // frozen: D ignored
  out.str("");
  out.clear();
  assert(sh.execute("logic poke DLATCH1.D=1", out) == 0);
  assert(!contains(out.str(), "DLATCH1.Q:"));
  // Frozen across advances: a latch-only workspace has no clock domain,
  // so `step` reports no cycle line but still shows the held Q.
  out.str("");
  out.clear();
  assert(sh.execute("logic step 3", out) == 0);
  assert(!contains(out.str(), "cycle"));
  assert(contains(out.str(), "DLATCH1.Q: 1 (no change)"));
  out.str("");
  out.clear();
  assert(sh.execute("logic peek", out) == 0);
  assert(contains(out.str(), "DLATCH1.Q=1 NQ=0"));
  // Re-enable with D parked at 0 while frozen: Q follows down at once.
  out.str("");
  out.clear();
  assert(sh.execute("logic poke DLATCH1.D=0", out) == 0);
  assert(!contains(out.str(), "DLATCH1.Q:")); // still frozen
  out.str("");
  out.clear();
  assert(sh.execute("logic poke DLATCH1.EN=1", out) == 0);
  assert(contains(out.str(), "DLATCH1.Q: 1->0")); // resumes tracking
  out.str("");
  out.clear();
  assert(sh.execute("logic peek", out) == 0);
  // Deterministic replay: identical rebuild + sequence reads back equal.
  const std::string held = out.str();
  assert(contains(held, "DLATCH1.Q=0 NQ=1"));
  assert(sh.execute("logic reset", out) == 0);
  assert(sh.execute("logic add DLATCH", out) == 0);
  assert(sh.execute("logic poke DLATCH1.EN=1", out) == 0);
  assert(sh.execute("logic poke DLATCH1.D=1", out) == 0);
  assert(sh.execute("logic poke DLATCH1.D=0", out) == 0);
  assert(sh.execute("logic poke DLATCH1.D=1", out) == 0);
  assert(sh.execute("logic poke DLATCH1.EN=0", out) == 0);
  assert(sh.execute("logic poke DLATCH1.D=0", out) == 0);
  assert(sh.execute("logic poke DLATCH1.D=1", out) == 0);
  assert(sh.execute("logic step 3", out) == 0);
  assert(sh.execute("logic poke DLATCH1.D=0", out) == 0);
  assert(sh.execute("logic poke DLATCH1.EN=1", out) == 0);
  out.str("");
  out.clear();
  assert(sh.execute("logic peek", out) == 0);
  assert(out.str() == held);
}

void test_sr_latch_session() {
  // US2 (004-sr-d-latches, quickstart §2): set/reset/hold with no
  // advance, plus loud S=R=1 refusal with state untouched on the direct,
  // wired, and tied entry paths.
  cli::Shell sh;
  no_history(sh);
  std::ostringstream out;
  assert(sh.execute("logic reset", out) == 0);
  assert(sh.execute("logic add SRLATCH", out) == 0);
  // Set with no advance; hold across release.
  assert(sh.execute("logic poke SRLATCH1.S=1", out) == 0);
  assert(contains(out.str(), "SRLATCH1.Q: 0->1"));
  out.str("");
  out.clear();
  assert(sh.execute("logic poke SRLATCH1.S=release", out) == 0);
  out.str("");
  out.clear();
  assert(sh.execute("logic peek", out) == 0);
  assert(contains(out.str(), "SRLATCH1.Q=1 NQ=0"));
  // Reset with no advance; hold across release.
  out.str("");
  out.clear();
  assert(sh.execute("logic poke SRLATCH1.R=1", out) == 0);
  assert(contains(out.str(), "SRLATCH1.Q: 1->0"));
  assert(sh.execute("logic poke SRLATCH1.R=release", out) == 0);
  out.str("");
  out.clear();
  assert(sh.execute("logic peek", out) == 0);
  assert(contains(out.str(), "SRLATCH1.Q=0 NQ=1"));
  // Illegal via direct drive: exit 2 naming the combination, Q untouched,
  // drive reverted (release reports it was never poked).
  assert(sh.execute("logic poke SRLATCH1.S=1", out) == 0);
  out.str("");
  out.clear();
  assert(sh.execute("logic poke SRLATCH1.R=1", out) == 2);
  assert(contains(out.str(), "S=1,R=1 is illegal"));
  out.str("");
  out.clear();
  assert(sh.execute("logic peek", out) == 0);
  assert(contains(out.str(), "SRLATCH1.Q=1 NQ=0")); // set held, not cleared
  out.str("");
  out.clear();
  assert(sh.execute("logic poke SRLATCH1.R=release", out) == 0);
  assert(contains(out.str(), "(was not poked)")); // drive was reverted
  // Illegal via wired comb: BUF fans out to both S and R.
  out.str("");
  out.clear();
  assert(sh.execute("logic reset", out) == 0);
  assert(sh.execute("logic add SRLATCH", out) == 0);
  assert(sh.execute("logic add BUF", out) == 0);
  assert(sh.execute("logic poke BUF1.1=1", out) == 0);
  assert(sh.execute("logic wire BUF1.Y SRLATCH1.S", out) == 0);
  assert(contains(out.str(), "SRLATCH1.Q: 0->1")); // wired set acts at once
  out.str("");
  out.clear();
  assert(sh.execute("logic wire BUF1.Y SRLATCH1.R", out) == 2);
  assert(contains(out.str(), "S=1,R=1 is illegal"));
  assert(contains(out.str(), "wire reverted"));
  out.str("");
  out.clear();
  assert(sh.execute("logic peek", out) == 0);
  assert(contains(out.str(), "SRLATCH1.Q=1 NQ=0")); // set held
  // The R wire is gone: unwire S, then the same R wire is legal (reset).
  assert(sh.execute("logic unwire SRLATCH1.S", out) == 0);
  out.str("");
  out.clear();
  assert(sh.execute("logic wire BUF1.Y SRLATCH1.R", out) == 0);
  assert(contains(out.str(), "SRLATCH1.Q: 1->0"));
  // Illegal via tie merge: poked S=1, then tying S=R completes S=R=1.
  out.str("");
  out.clear();
  assert(sh.execute("logic reset", out) == 0);
  assert(sh.execute("logic add SRLATCH", out) == 0);
  assert(sh.execute("logic poke SRLATCH1.S=1", out) == 0);
  out.str("");
  out.clear();
  assert(sh.execute("logic tie SRLATCH1.S SRLATCH1.R", out) == 2);
  assert(contains(out.str(), "S=1,R=1 is illegal"));
  assert(contains(out.str(), "tie reverted"));
  out.str("");
  out.clear();
  assert(sh.execute("logic peek", out) == 0);
  assert(contains(out.str(), "SRLATCH1.Q=1 NQ=0"));
  // The tie is gone: release S, then R drives alone (legal reset).
  assert(sh.execute("logic poke SRLATCH1.S=release", out) == 0);
  out.str("");
  out.clear();
  assert(sh.execute("logic poke SRLATCH1.R=1", out) == 0);
  assert(contains(out.str(), "SRLATCH1.Q: 1->0"));
  // Wrong-kind pins are usage errors that change no state.
  out.str("");
  out.clear();
  assert(sh.execute("logic poke SRLATCH1.D=1", out) == 2);
  assert(sh.execute("logic poke SRLATCH1.CLK=1", out) == 2);
  assert(sh.execute("logic poke SRLATCH1.J=1", out) == 2);
  out.str("");
  out.clear();
  assert(sh.execute("logic peek", out) == 0);
  assert(contains(out.str(), "SRLATCH1.Q=0 NQ=1"));
  // Hold across advances: no clock domain for latches, Q frozen.
  out.str("");
  out.clear();
  assert(sh.execute("logic step 2", out) == 0);
  assert(!contains(out.str(), "cycle"));
  assert(contains(out.str(), "SRLATCH1.Q: 0 (no change)"));
  // Deterministic replay: identical rebuild + script reads back equal.
  out.str("");
  out.clear();
  assert(sh.execute("logic reset", out) == 0);
  assert(sh.execute("logic add SRLATCH", out) == 0);
  assert(sh.execute("logic poke SRLATCH1.S=1", out) == 0);
  assert(sh.execute("logic poke SRLATCH1.S=release", out) == 0);
  assert(sh.execute("logic poke SRLATCH1.R=1", out) == 0);
  assert(sh.execute("logic poke SRLATCH1.R=release", out) == 0);
  out.str("");
  out.clear();
  assert(sh.execute("logic peek", out) == 0);
  assert(contains(out.str(), "SRLATCH1.Q=0 NQ=1"));
}

void test_sr_illegal_file_load_keeps_workspace() {
  // US2 illegal-load path (004-sr-d-latches T014): a hand-written file
  // whose nets resolve S=R=1 (NOT with dangling input reads 0, so Y=1
  // fans out to both S and R) is refused with exit 2 and the prior
  // workspace — gates, wires, and Q — is kept exactly.
  cli::Shell sh;
  no_history(sh);
  std::ostringstream out;
  assert(sh.execute("logic reset", out) == 0);
  assert(sh.execute("logic add DFF", out) == 0);
  assert(sh.execute("logic ignore DFF1.CLK", out) == 0);
  assert(sh.execute("logic poke DFF1.D=1", out) == 0);
  assert(sh.execute("logic step 1", out) == 0);
  out.str("");
  out.clear();
  assert(sh.execute("logic peek", out) == 0);
  assert(contains(out.str(), "DFF1.Q=1 NQ=0"));
  const std::string tmp =
      (std::filesystem::temp_directory_path() / "wengine_sr_illegal.yaml").string();
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    assert(f.good());
    f << "version: 2\n"
         "gates:\n"
         "  - {id: SRLATCH1, kind: srlatch, inputs: 2, ignore: [], ignore_output: false, delay_ps: 0}\n"
         "  - {id: NOT1, kind: not, inputs: 1, ignore: [], ignore_output: false, delay_ps: 0}\n"
         "wires:\n"
         "  - {from: NOT1.Y, to: SRLATCH1.S}\n"
         "  - {from: NOT1.Y, to: SRLATCH1.R}\n"
         "ties:\n"
         "meta:\n"
         "  tool: wengine-logic\n";
  }
  out.str("");
  out.clear();
  assert(sh.execute(std::string("logic load ") + tmp, out) == 2);
  assert(contains(out.str(), "S=1,R=1 is illegal"));
  assert(contains(out.str(), "workspace kept"));
  // Prior workspace kept: DFF shape, wires, and Q all intact.
  out.str("");
  out.clear();
  assert(sh.execute("logic peek", out) == 0);
  assert(contains(out.str(), "DFF1.Q=1 NQ=0"));
  assert(!contains(out.str(), "SRLATCH1"));
  out.str("");
  out.clear();
  assert(sh.execute("logic list --wires", out) == 0);
  assert(!contains(out.str(), "SRLATCH1"));
  std::remove(tmp.c_str());
}

void test_mixed_edge_level_session() {
  // US3 (004-sr-d-latches, quickstart §3): one DFF + one SR latch + one
  // D latch in one workspace — level Q resolves at once while the DFF
  // captures at the edge, all under one shared cycle count; replay is
  // byte-identical.
  cli::Shell sh;
  no_history(sh);
  std::ostringstream out;
  assert(sh.execute("logic reset", out) == 0);
  assert(sh.execute("logic add DFF", out) == 0);
  assert(sh.execute("logic add SRLATCH", out) == 0);
  assert(sh.execute("logic add DLATCH", out) == 0);
  assert(sh.execute("logic ignore DFF1.CLK", out) == 0);
  assert(sh.execute("logic poke DFF1.D=1", out) == 0);
  assert(sh.execute("logic poke SRLATCH1.S=1", out) == 0);
  assert(contains(out.str(), "SRLATCH1.Q: 0->1")); // level: at once
  assert(sh.execute("logic poke DLATCH1.EN=1", out) == 0);
  assert(sh.execute("logic poke DLATCH1.D=1", out) == 0);
  assert(contains(out.str(), "DLATCH1.Q: 0->1")); // level: at once
  out.str("");
  out.clear();
  assert(sh.execute("logic step 1", out) == 0);
  assert(contains(out.str(), "clk: cycle 1 (1 edges)")); // edge domain only
  assert(contains(out.str(), "DFF1.Q: 0->1"));           // edge capture
  out.str("");
  out.clear();
  assert(sh.execute("logic peek", out) == 0);
  const std::string first = out.str();
  assert(contains(first, "DFF1.Q=1"));
  assert(contains(first, "SRLATCH1.Q=1 NQ=0"));
  assert(contains(first, "DLATCH1.Q=1 NQ=0"));
  // Freeze the DLATCH and re-step: only the edge kind moves.
  assert(sh.execute("logic poke DLATCH1.EN=0", out) == 0);
  assert(sh.execute("logic poke DFF1.D=0", out) == 0);
  assert(sh.execute("logic poke DLATCH1.D=0", out) == 0); // frozen: no line
  out.str("");
  out.clear();
  assert(sh.execute("logic step 1", out) == 0);
  assert(sh.execute("logic peek", out) == 0);
  assert(contains(out.str(), "DFF1.Q=0"));
  assert(contains(out.str(), "SRLATCH1.Q=1 NQ=0")); // held, not stepped
  assert(contains(out.str(), "DLATCH1.Q=1 NQ=0")); // frozen, not stepped
  // Identical rebuild + sequence reads back byte-identically.
  assert(sh.execute("logic reset", out) == 0);
  assert(sh.execute("logic add DFF", out) == 0);
  assert(sh.execute("logic add SRLATCH", out) == 0);
  assert(sh.execute("logic add DLATCH", out) == 0);
  assert(sh.execute("logic ignore DFF1.CLK", out) == 0);
  assert(sh.execute("logic poke DFF1.D=1", out) == 0);
  assert(sh.execute("logic poke SRLATCH1.S=1", out) == 0);
  assert(sh.execute("logic poke DLATCH1.EN=1", out) == 0);
  assert(sh.execute("logic poke DLATCH1.D=1", out) == 0);
  assert(sh.execute("logic step 1", out) == 0);
  out.str("");
  out.clear();
  assert(sh.execute("logic peek", out) == 0);
  assert(out.str() == first);
}

} // namespace

int main() {
  test_advance_toggle();
  test_direct_matches_oneshot();
  test_selective_domains();
  test_errors_changeless();
  test_determinism_replay();
  test_nq_ignored_by_default();
  test_t_divider_session();
  test_dlatch_level_session();
  test_sr_latch_session();
  test_sr_illegal_file_load_keeps_workspace();
  test_mixed_edge_level_session();
  test_jk_counter_session();
  test_mixed_kinds_session();
  test_complete();
  std::cout << "test_logic_seq_cli_step: OK\n";
  return 0;
}
