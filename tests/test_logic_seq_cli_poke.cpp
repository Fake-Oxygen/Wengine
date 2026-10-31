// US2 poke goldens: `poke PIN=0|1|release` over session-built nets
// (contracts/poke.md). Drives `Shell::execute` one-shot lines plus direct
// `LogicPokeSpec::run` calls (same-verb same-text). Links wengine_cli.

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

int run_poke_direct(std::ostringstream &out, std::initializer_list<std::string_view> toks) {
  std::vector<std::string_view> v(toks);
  cli::ArgCursor cur{std::span<const std::string_view>(v.data(), v.size())};
  return cli::logic::LogicPokeSpec::run(cur, out);
}

void test_drive_release_list() {
  cli::Shell sh;
  no_history(sh);
  std::ostringstream out;
  assert(sh.execute("logic reset", out) == 0);
  assert(sh.execute("logic add DFF", out) == 0);
  assert(sh.execute("logic ignore DFF1.CLK", out) == 0);
  out.str("");
  out.clear();
  assert(sh.execute("logic poke DFF1.D=1", out) == 0);
  assert(contains(out.str(), "poked DFF1.D=1 (leader DFF1.D)"));
  // Numeric alias resolves to the same entry (repeat overwrites).
  out.str("");
  out.clear();
  assert(sh.execute("logic poke DFF1.1=0", out) == 0);
  assert(contains(out.str(), "updated DFF1.D 1->0"));
  // Bare poke lists the map in slot order.
  out.str("");
  out.clear();
  assert(sh.execute("logic poke", out) == 0);
  assert(contains(out.str(), "DFF1.D=0"));
  // Release drops the entry.
  out.str("");
  out.clear();
  assert(sh.execute("logic poke DFF1.D=release", out) == 0);
  assert(contains(out.str(), "released DFF1.D"));
  out.str("");
  out.clear();
  assert(sh.execute("logic poke", out) == 0);
  assert(contains(out.str(), "poke map: empty"));
}

void test_direct_matches_oneshot() {
  cli::Shell sh;
  no_history(sh);
  std::ostringstream out;
  assert(sh.execute("logic reset", out) == 0);
  assert(sh.execute("logic add DFF", out) == 0);
  assert(sh.execute("logic ignore DFF1.CLK", out) == 0);
  std::ostringstream one_shot, direct;
  assert(sh.execute("logic poke DFF1.D=1", one_shot) == 0);
  assert(sh.execute("logic reset", out) == 0);
  assert(sh.execute("logic add DFF", out) == 0);
  assert(sh.execute("logic ignore DFF1.CLK", out) == 0);
  assert(run_poke_direct(direct, {"DFF1.D=1"}) == 0);
  assert(one_shot.str() == direct.str());
}

void test_from_and_clear() {
  cli::Shell sh;
  no_history(sh);
  std::ostringstream out;
  assert(sh.execute("logic reset", out) == 0);
  assert(sh.execute("logic add AND", out) == 0);
  assert(sh.execute("logic poke --from AND1.1=1 --from AND1.2=0", out) == 0);
  assert(contains(out.str(), "poked AND1.1=1"));
  assert(contains(out.str(), "poked AND1.2=0"));
  out.str("");
  out.clear();
  assert(sh.execute("logic poke --clear", out) == 0);
  assert(contains(out.str(), "cleared 2 poke entries"));
  out.str("");
  out.clear();
  assert(sh.execute("logic poke", out) == 0);
  assert(contains(out.str(), "poke map: empty"));
}

void test_errors_changeless() {
  cli::Shell sh;
  no_history(sh);
  std::ostringstream out;
  assert(sh.execute("logic reset", out) == 0);
  assert(sh.execute("logic add DFF", out) == 0);
  assert(sh.execute("logic ignore DFF1.CLK", out) == 0);
  // Unknown pin.
  assert(sh.execute("logic poke NOPE1.1=1", out) == 2);
  assert(contains(out.str(), "bad block input pin"));
  // Output pins are not pokable.
  out.str("");
  out.clear();
  assert(sh.execute("logic poke DFF1.Q=1", out) == 2);
  assert(contains(out.str(), "not a pokable block input"));
  // Bad value.
  out.str("");
  out.clear();
  assert(sh.execute("logic poke DFF1.D=2", out) == 2);
  assert(contains(out.str(), "takes PIN=0|1|release"));
  // Named pin on a combinational gate.
  out.str("");
  out.clear();
  assert(sh.execute("logic poke DFF1.D=1", out) == 0);
  assert(sh.execute("logic add AND", out) == 0);
  assert(sh.execute("logic poke AND1.D=1", out) == 2);
  assert(contains(out.str(), "bad block input pin"));
  // Wired pins are not pokable (wire it first).
  out.str("");
  out.clear();
  assert(sh.execute("logic add NOT", out) == 0);
  assert(sh.execute("logic wire DFF1.Q NOT1.1", out) == 0);
  assert(sh.execute("logic poke NOT1.1=1", out) == 2);
  assert(contains(out.str(), "not a pokable block input"));
  // Duplicate --from for the same pin.
  out.str("");
  out.clear();
  assert(sh.execute("logic poke --from DFF1.D=1 --from DFF1.D=0", out) == 2);
  assert(contains(out.str(), "duplicate value"));
  // Ignored pins are not pokable.
  out.str("");
  out.clear();
  assert(sh.execute("logic poke DFF1.CLK=1", out) == 2);
  assert(contains(out.str(), "not a pokable block input"));
  // Errors changed nothing: the earlier DFF1.D=1 poke still holds, and a
  // step latches it (poke composes into step per data-model §3).
  out.str("");
  out.clear();
  assert(sh.execute("logic step 1", out) == 0);
  assert(contains(out.str(), "DFF1.Q: 0->1"));
}

void test_complete() {
  cli::Shell sh;
  no_history(sh);
  std::ostringstream out;
  assert(sh.execute("logic reset", out) == 0);
  assert(sh.execute("logic add DFF", out) == 0);
  assert(sh.execute("logic ignore DFF1.CLK", out) == 0);
  std::vector<std::string> names;
  {
    const std::span<const std::string_view> no_prior{};
    cli::logic::LogicPokeSpec::complete(no_prior, "DFF1.", names);
    bool saw_d = false;
    for (const std::string &n : names) {
      if (n == "DFF1.D=0" || n == "DFF1.D=1") {
        saw_d = true;
      }
    }
    assert(saw_d);
  }
  {
    const std::span<const std::string_view> no_prior{};
    names.clear();
    cli::logic::LogicPokeSpec::complete(no_prior, "--", names);
    bool saw_from = false;
    for (const std::string &n : names) {
      if (n == "--from" || n == "--from=") {
        saw_from = true;
      }
    }
    assert(saw_from);
  }
}

} // namespace

int main() {
  test_drive_release_list();
  test_direct_matches_oneshot();
  test_from_and_clear();
  test_errors_changeless();
  test_complete();
  std::cout << "test_logic_seq_cli_poke: OK\n";
  return 0;
}
