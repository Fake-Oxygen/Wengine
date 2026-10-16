#include "wengine/cli/args.hpp"
#include "wengine/cli/commands.hpp"
#include "wengine/solvers/led.hpp"

#include <array>
#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>
#include <span>
#include <sstream>
#include <string>
#include <vector>

using namespace wengine::solvers;
using wengine::cli::ArgCursor;
using wengine::cli::kMaxArgs;
using wengine::cli::tokenize_into;

namespace {

constexpr bool near(double a, double b, double eps = 1e-9) {
  return std::fabs(a - b) <= eps * (1.0 + std::fabs(b));
}

void test_reference_anchors() {
  // Hand-derived operating point: VS=5V, VF=2V (red LED), IF=20mA.
  //   headroom = 3V, Rs = 150 Ohm (exactly E12), IF' = 20mA,
  //   Pres = 60mW, Pled = 40mW, Ptot = 100mW, eff = 40%.
  const auto r = LedSolver::solve(5.0, 2.0, 0.02);
  assert(r.feasible);
  assert(near(r.headroom_v, 3.0));
  assert(near(r.rs_ohm, 150.0));
  assert(near(r.rs_e12_ohm, 150.0));
  assert(near(r.rs_used_ohm, 150.0));
  assert(near(r.if_actual_a, 0.02));
  assert(near(r.p_res_w, 0.06));
  assert(near(r.p_led_w, 0.04));
  assert(near(r.p_total_w, 0.1));
  assert(near(r.eff, 0.4));
  assert(near(r.eff * r.p_total_w, r.p_led_w, 1e-12)); // eff is the Ptot->Pled ratio
  assert(r.rating_ok && r.pass); // no rating -> no limit
}

void test_helpers() {
  // Primitives on exact integer points.
  assert(LedSolver::headroom_v(5.0, 3.0) == 2.0);
  assert(LedSolver::series_resistance_ohm(5.0, 3.0, 2.0) == 1.0);
  assert(LedSolver::actual_current_a(5.0, 3.0, 2.0) == 1.0);
  assert(LedSolver::resistor_power_w(5.0, 3.0, 2.0) == 4.0);
  assert(LedSolver::led_power_w(3.0, 2.0) == 6.0);
  assert(LedSolver::total_power_w(5.0, 2.0) == 10.0);
  // Zero current: open circuit -> infinite resistance, never NaN.
  assert(LedSolver::series_resistance_ohm(5.0, 3.0, 0.0) ==
         std::numeric_limits<double>::infinity());
  // E12 pick moves the current: 3.3V/2.1V/20mA -> Rs=60 -> E12 56.
  {
    const auto r = LedSolver::solve(3.3, 2.1, 0.02);
    assert(r.feasible);
    assert(near(r.rs_ohm, 60.0));
    assert(near(r.rs_e12_ohm, 56.0));
    assert(near(r.rs_used_ohm, 56.0));
    assert(near(r.if_actual_a, 1.2 / 56.0));
    assert(near(r.p_res_w, 1.2 * 1.2 / 56.0));
    assert(r.pass);
  }
  // Explicit Rs is evaluated as-is (resistor at hand).
  {
    const auto r = LedSolver::solve(5.0, 2.0, 0.02, 180.0);
    assert(near(r.rs_used_ohm, 180.0));
    assert(near(r.if_actual_a, 3.0 / 180.0));
    assert(near(r.p_res_w, 3.0 * 3.0 / 180.0)); // 50mW, dimmer than target
    assert(r.pass);
  }
  // Infeasible: VS <= VF leaves no positive Rs (blue LED on 2V rail).
  {
    const auto r = LedSolver::solve(2.0, 3.2, 0.02);
    assert(!r.feasible && !r.pass);
    assert(near(r.headroom_v, -1.2));
    assert(r.rs_e12_ohm == 0.0 && r.rs_used_ohm == 0.0);
  }
  // Power-rating limit honored only when supplied.
  {
    const auto ok = LedSolver::solve(5.0, 2.0, 0.02, 0.0, 0.25);
    assert(ok.rating_ok && ok.pass); // 60mW in a 1/4W part
    const auto hot = LedSolver::solve(5.0, 2.0, 0.02, 0.0, 0.05);
    assert(!hot.rating_ok && !hot.pass);
  }
}

void test_typed_api() {
  using namespace wengine::units::literals;
  auto h = LedSolver::headroom(5.0_V, 2.0_V);
  assert(h.get() == 3.0);
  auto rs = LedSolver::series_resistance(5.0_V, 2.0_V, 20.0_mA);
  assert(near(rs.get(), 150.0));
  auto i = LedSolver::actual_current(5.0_V, 2.0_V, 150.0_Ohm);
  assert(near(i.get(), 0.02));
  auto p = LedSolver::resistor_power(5.0_V, 2.0_V, 20.0_mA);
  assert(near(p.get(), 0.06));
}

std::pair<int, std::string> run_led(std::string line) {
  std::array<std::string_view, kMaxArgs> buf{};
  const std::size_t n = tokenize_into(line, buf);
  std::span<const std::string_view> args{buf.data(), n};
  ArgCursor cur{args};
  std::ostringstream out;
  const int rc = wengine::cli::commands::LedSpec::run(cur, out);
  return {rc, out.str()};
}

void test_cli() {
  {
    auto [rc, s] = run_led("--vs 5V --vf 2V --if 20mA");
    assert(rc == 0);
    assert(s.find("headroom = 3") != std::string::npos);
    assert(s.find("Rs exact = 150") != std::string::npos);
    assert(s.find("Rs(E12) = 150") != std::string::npos);
    assert(s.find("IF actual = 0.02") != std::string::npos);
    assert(s.find("eff = 40") != std::string::npos);
    assert(s.find("summary: PASS") != std::string::npos);
  }
  {
    // E12 pick recomputes the current (3.3V rail, 2.1V LED).
    auto [rc, s] = run_led("--vs 3.3V --vf 2.1V --if 20mA");
    assert(rc == 0 && s.find("Rs(E12) = 56") != std::string::npos);
    assert(s.find("(E12 pick)") != std::string::npos);
  }
  {
    // Explicit resistor at hand is evaluated as-is.
    auto [rc, s] = run_led("--vs 5V --vf 2V --if 20mA --rs 180Ohm");
    assert(rc == 0 && s.find("Rs used = 180") != std::string::npos);
    assert(s.find("(explicit)") != std::string::npos);
  }
  {
    // Underrated resistor reports FAIL with rc 1.
    auto [rc, s] = run_led("--vs 5V --vf 2V --if 20mA --prating 0.05W");
    assert(rc == 1 && s.find("P rating = 0.05") != std::string::npos);
    assert(s.find("summary: FAIL") != std::string::npos);
  }
  {
    // Generous rating passes.
    auto [rc, s] = run_led("--vs 5V --vf 2V --if 20mA --prating 0.25W");
    assert(rc == 0 && s.find("summary: PASS") != std::string::npos);
  }
  {
    // Infeasible headroom reports FAIL with rc 1.
    auto [rc, s] = run_led("--vs 2V --vf 3.2V --if 20mA");
    assert(rc == 1 && s.find("INFEASIBLE") != std::string::npos);
  }
  {
    auto [rc, s] = run_led("--vs 5V --vf 2V");
    assert(rc == 2); // missing --if
  }
  {
    auto [rc, s] = run_led("--vs 5V --vf 2V --if 20mA 150Ohm");
    assert(rc == 2); // stray positional (named-options only)
  }
  {
    auto [rc, s] = run_led("--vs 5V --vf 2V --if 20mA --rs 0Ohm");
    assert(rc == 2); // explicit Rs must be > 0
  }
  {
    auto [rc, s] = run_led("--vs 5V --vf 2V --if 20mA --bogus 1");
    assert(rc == 2); // unknown option never silently ignored
  }
  {
    auto [rc, s] = run_led("--help");
    assert(rc == 0 && s.find("headroom") != std::string::npos);
  }
}

void test_completion() {
  using wengine::cli::commands::LedSpec;
  auto complete = [](std::string_view partial) {
    std::array<std::string_view, 0> prior{};
    std::vector<std::string> out;
    LedSpec::complete(prior, partial, out);
    return out;
  };
  auto has = [](const std::vector<std::string> &v, std::string_view s) {
    for (const auto &e : v) {
      if (e == s) {
        return true;
      }
    }
    return false;
  };
  {
    const auto c = complete("--");
    assert(has(c, "--vs") && has(c, "--vf") && has(c, "--if") && has(c, "--rs") &&
           has(c, "--prating") && has(c, "--help"));
  }
  {
    const auto c = complete("--v");
    assert(c.size() == 2 && has(c, "--vs") && has(c, "--vf"));
  }
  {
    // Numeric values: nothing to complete.
    assert(complete("5V").empty());
    assert(complete("").empty());
  }
}

} // namespace

int main() {
  test_reference_anchors();
  test_helpers();
  test_typed_api();
  test_cli();
  test_completion();
  std::cout << "test_led: OK\n";
  return 0;
}
