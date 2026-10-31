// US3 peek goldens: `peek [GATE]` at all three scopes (contracts/peek.md).
// Drives `Shell::execute` one-shot lines plus direct `LogicPokeSpec`-style
// `LogicPeekSpec::run` calls (same-verb same-text). Links wengine_cli.

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

int run_peek_direct(std::ostringstream &out, std::initializer_list<std::string_view> toks) {
  std::vector<std::string_view> v(toks);
  cli::ArgCursor cur{std::span<const std::string_view>(v.data(), v.size())};
  return cli::logic::LogicPeekSpec::run(cur, out);
}

void test_empty_and_reset_state() {
  cli::Shell sh;
  no_history(sh);
  std::ostringstream out;
  assert(sh.execute("logic reset", out) == 0);
  out.str("");
  out.clear();
  assert(sh.execute("logic peek", out) == 0);
  assert(contains(out.str(), "no flops"));
  // Known reset state reads back without any advance.
  assert(sh.execute("logic add DFF", out) == 0);
  out.str("");
  out.clear();
  assert(sh.execute("logic peek", out) == 0);
  assert(contains(out.str(), "DFF1.Q=0 NQ=1 (serial 0)"));
  assert(contains(out.str(), "flops: 1"));
}

void test_scopes_and_slot_order() {
  cli::Shell sh;
  no_history(sh);
  std::ostringstream out;
  assert(sh.execute("logic reset", out) == 0);
  assert(sh.execute("logic add DFF", out) == 0);
  assert(sh.execute("logic add DLATCH", out) == 0);
  out.str("");
  out.clear();
  assert(sh.execute("logic peek", out) == 0);
  const std::string all = out.str();
  const std::size_t p1 = all.find("DFF1.Q=");
  const std::size_t p2 = all.find("DLATCH1.Q=");
  assert(p1 != std::string::npos && p2 != std::string::npos && p1 < p2);
  assert(contains(all, "flops: 2"));
  // Per-block scope.
  out.str("");
  out.clear();
  assert(sh.execute("logic peek DLATCH1", out) == 0);
  assert(contains(out.str(), "DLATCH1.Q=0 NQ=1"));
  assert(!contains(out.str(), "DFF1.Q="));
  assert(contains(out.str(), "flops: 1"));
}

void test_direct_matches_oneshot() {
  cli::Shell sh;
  no_history(sh);
  std::ostringstream out;
  assert(sh.execute("logic reset", out) == 0);
  assert(sh.execute("logic add DFF", out) == 0);
  std::ostringstream one_shot, direct;
  assert(sh.execute("logic peek DFF1", one_shot) == 0);
  assert(run_peek_direct(direct, {"DFF1"}) == 0);
  assert(one_shot.str() == direct.str());
}

void test_serials_stable_across_steps() {
  cli::Shell sh;
  no_history(sh);
  std::ostringstream out;
  assert(sh.execute("logic reset", out) == 0);
  assert(sh.execute("logic add DFF", out) == 0);
  assert(sh.execute("logic ignore DFF1.CLK", out) == 0);
  assert(sh.execute("logic poke DFF1.D=1", out) == 0);
  out.str("");
  out.clear();
  assert(sh.execute("logic peek", out) == 0);
  assert(contains(out.str(), "DFF1.Q=0 NQ=1 (serial 0)"));
  assert(sh.execute("logic step 1", out) == 0);
  out.str("");
  out.clear();
  assert(sh.execute("logic peek", out) == 0);
  // Same serial, flipped values: serials survive stepping.
  assert(contains(out.str(), "DFF1.Q=1 NQ=0 (serial 0)"));
}

void test_errors() {
  cli::Shell sh;
  no_history(sh);
  std::ostringstream out;
  assert(sh.execute("logic reset", out) == 0);
  assert(sh.execute("logic add DFF", out) == 0);
  assert(sh.execute("logic add AND", out) == 0);
  assert(sh.execute("logic peek NOPE1", out) == 2);
  assert(contains(out.str(), "unknown gate"));
  out.str("");
  out.clear();
  assert(sh.execute("logic peek AND1", out) == 2);
  assert(contains(out.str(), "not a sequential gate"));
  out.str("");
  out.clear();
  assert(sh.execute("logic peek DFF1 extra", out) == 2);
}

void test_complete() {
  cli::Shell sh;
  no_history(sh);
  std::ostringstream out;
  assert(sh.execute("logic reset", out) == 0);
  assert(sh.execute("logic add DFF", out) == 0);
  assert(sh.execute("logic add AND", out) == 0);
  std::vector<std::string> names;
  {
    const std::span<const std::string_view> no_prior{};
    cli::logic::LogicPeekSpec::complete(no_prior, "D", names);
    bool saw_dff = false;
    bool saw_and = false;
    for (const std::string &n : names) {
      if (n == "DFF1") {
        saw_dff = true;
      }
      if (n == "AND1") {
        saw_and = true;
      }
    }
    assert(saw_dff && !saw_and); // sequential gates only
  }
}

} // namespace

int main() {
  test_empty_and_reset_state();
  test_scopes_and_slot_order();
  test_direct_matches_oneshot();
  test_serials_stable_across_steps();
  test_errors();
  test_complete();
  std::cout << "test_logic_seq_cli_peek: OK\n";
  return 0;
}
