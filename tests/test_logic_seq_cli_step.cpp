// US1 step goldens: `step [DOMAIN] [N]` over session-built sequential
// nets (contracts/step.md). Drives the feature through `Shell::execute`
// one-shot lines (surface parity) plus direct `LogicStepSpec::run` calls
// (same-verb same-text). Links wengine_cli like test_cli_args.

#include "wengine/cli/logic_registry.hpp"
#include "wengine/cli/shell.hpp"
#include "wengine/core/status.hpp"

#include <cassert>
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
// ignored. Power-on Q=0; edge 1 latches D=NOT(0)=1.
void build_toggle(cli::Shell &sh, std::ostringstream &out) {
  assert(sh.execute("logic reset", out) == 0);
  assert(sh.execute("logic add DFF", out) == 0);
  assert(sh.execute("logic add NOT", out) == 0);
  assert(sh.execute("logic wire NOT1.Y DFF1.D", out) == 0);
  assert(sh.execute("logic wire DFF1.Q NOT1.1", out) == 0);
  assert(sh.execute("logic ignore DFF1.CLK", out) == 0);
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

} // namespace

int main() {
  test_advance_toggle();
  test_direct_matches_oneshot();
  test_selective_domains();
  test_errors_changeless();
  test_determinism_replay();
  test_complete();
  std::cout << "test_logic_seq_cli_step: OK\n";
  return 0;
}
