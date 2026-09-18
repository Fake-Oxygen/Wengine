#include "wengine/cli/args.hpp"
#include "wengine/cli/commands.hpp"
#include "wengine/solvers/i2c.hpp"

#include <array>
#include <cassert>
#include <cmath>
#include <iostream>
#include <span>
#include <sstream>
#include <string>

using namespace wengine::solvers;
using wengine::cli::ArgCursor;
using wengine::cli::kMaxArgs;
using wengine::cli::tokenize_into;

namespace {

constexpr bool near(double a, double b, double eps = 1e-9) {
  return std::fabs(a - b) <= eps * (1.0 + std::fabs(b));
}

void test_reference_anchors() {
  // Anchors straight from the reference documents (see i2c.hpp header).
  // UM10204 section 7.1 example: (5.5 - 0.4) / 0.003 = 1.7 kOhm.
  assert(near(I2cBusSolver::rp_min_ohm(5.5, 0.4, 3e-3), 1700.0));
  // TI SLVA689 worked example: (3.3 - 0.4) / 0.003 = 966.667 Ohm.
  assert(near(I2cBusSolver::rp_min_ohm(3.3, 0.4, 3e-3), 966.6666666666666));
  // TI SLVA689: tr = 300 ns, Cb = 200 pF -> Rp(max) = 1.77 kOhm.
  assert(near(I2cBusSolver::rp_max_ohm(300e-9, 200e-12), 1770.34, 1e-4));
  // NXP Fm+ training: tr = 1000 ns, Cb = 400 pF -> Rmax = 2.95 kOhm.
  assert(near(I2cBusSolver::rp_max_ohm(1000e-9, 400e-12), 2950.4, 1e-4));
  // NXP Fm+ training: tr = 300 ns, Cb = 400 pF -> Rmax = 885 Ohm.
  assert(near(I2cBusSolver::rp_max_ohm(300e-9, 400e-12), 885.1, 1e-4));
}

void test_helpers() {
  // Rise time round-trips through Rp(max).
  assert(near(I2cBusSolver::rise_time_s(1770.34, 200e-12), 300e-9, 1e-4));
  // Static load at the TI operating point.
  assert(near(I2cBusSolver::static_current_a(3.3, 0.4, 966.6666666666666), 3e-3));
  assert(near(I2cBusSolver::static_power_w(3.3, 0.4, 966.6666666666666), 2.9 * 3e-3));
  // E12 snapping.
  assert(I2cBusSolver::e12_nearest(1510.0) == 1500.0);
  assert(I2cBusSolver::e12_nearest(1700.0) == 1800.0);
  assert(I2cBusSolver::e12_nearest(0.0) == 0.0);
  assert(I2cBusSolver::e12_nearest(966.667) == 1000.0);
  // Geometric-mean recommendation.
  assert(near(I2cBusSolver::geometric_mean(966.667, 1770.34),
              std::sqrt(966.667 * 1770.34), 1e-9));
  // Mode limits match UM10204 Table 10.
  {
    const auto std = i2c_mode_limits(I2cMode::Standard);
    assert(std.tr_max_s == 1000e-9 && std.cb_max_f == 400e-12 && std.iol_a == 3e-3);
    const auto fast = i2c_mode_limits(I2cMode::Fast);
    assert(fast.tr_max_s == 300e-9 && fast.cb_max_f == 400e-12);
    const auto fmp = i2c_mode_limits(I2cMode::FastPlus);
    assert(fmp.tr_max_s == 120e-9 && fmp.cb_max_f == 550e-12 && fmp.iol_a == 30e-3);
  }
  // VOL defaults (Table 9 style).
  assert(i2c_default_vol_v(3.3) == 0.4);
  assert(near(i2c_default_vol_v(1.8), 0.36));
  assert(i2c_default_iol_a(3.3, I2cMode::Fast) == 3e-3);
  assert(i2c_default_iol_a(3.3, I2cMode::FastPlus) == 30e-3);
}

void test_pullup_solve() {
  // 3.3 V / 200 pF / fast: window [966.67, 1770.34].
  const auto r =
      I2cBusSolver::solve_pullup(3.3, 200e-12, 300e-9, 0.4, 3e-3);
  assert(r.feasible);
  assert(near(r.rp_min_ohm, 966.6666666666666));
  assert(near(r.rp_max_ohm, 1770.34, 1e-4));
  assert(near(r.rp_rec_ohm, std::sqrt(r.rp_min_ohm * r.rp_max_ohm), 1e-9));
  assert(r.rp_e12_ohm == 1200.0); // sqrt(966.67*1770.34) ~= 1308 -> E12 1.2k
  assert(r.rp_used_ohm == 1200.0);
  assert(r.tr_ok && r.iol_ok);
  // Explicit Rp choice is evaluated as-is.
  const auto bad = I2cBusSolver::solve_pullup(3.3, 200e-12, 300e-9, 0.4, 3e-3, 4700.0);
  assert(bad.rp_used_ohm == 4700.0);
  assert(!bad.tr_ok); // 0.8473*4700*200pF = 796 ns > 300 ns
  assert(bad.iol_ok); // weak pullup draws little current
  // Infeasible window: min > max (heavy load, e.g. 400 pF at 3.3 V fast
  // is still feasible; force it with a tiny tr).
  const auto inf = I2cBusSolver::solve_pullup(3.3, 400e-12, 100e-9, 0.4, 3e-3);
  assert(!inf.feasible);
  assert(inf.rp_e12_ohm == 0.0 && inf.rp_used_ohm == 0.0);
}

void test_series_solve() {
  // 3.3 V, VOL 0.4, IOL 3 mA, Rp 1200, Cb 200 pF, tr 300 ns, tf 300 ns.
  const auto r =
      I2cBusSolver::solve_series(3.3, 0.4, 3e-3, 1200.0, 200e-12, 300e-9, 300e-9);
  // LOW bound: (0.99 - 0.4) / 0.003 = 196.67 Ohm.
  assert(near(r.rs_max_low_ohm, 196.66666666666666));
  // Rise budget: 1770.34 - 1200 = 570.34 Ohm.
  assert(near(r.rs_max_rise_ohm, 570.34, 1e-3));
  assert(near(r.rs_max_ohm, 196.66666666666666));
  assert(r.feasible);
  // Auto pick = rs_max.
  assert(near(r.rs_used_ohm, r.rs_max_ohm));
  assert(r.low_ok && r.rise_ok);
  // Explicit Rs = 100 Ohm.
  const auto e100 =
      I2cBusSolver::solve_series(3.3, 0.4, 3e-3, 1200.0, 200e-12, 300e-9, 300e-9, 100.0);
  assert(near(e100.vol_at_bus_v, 0.7));
  assert(near(e100.vil_margin_v, 0.29));
  assert(e100.low_ok && e100.rise_ok && e100.fall_ok);
  // Oversize Rs violates both the LOW margin and the rise budget
  // (tr = 0.8473 * 2200 * 200pF = 372.8 ns > 300 ns).
  const auto big =
      I2cBusSolver::solve_series(3.3, 0.4, 3e-3, 1200.0, 200e-12, 300e-9, 300e-9, 1000.0);
  assert(!big.low_ok && !big.rise_ok);
}

void test_typed_api() {
  using namespace wengine::units::literals;
  auto rmin = I2cBusSolver::rp_min(3.3_V, 0.4_V, 3.0_mA);
  assert(near(rmin.get(), 966.6666666666666));
  auto rmax = I2cBusSolver::rp_max(300.0_ns, 200.0_pF);
  assert(near(rmax.get(), 1770.34, 1e-4));
  auto tr = I2cBusSolver::rise_time(1200.0_Ohm, 200.0_pF);
  assert(near(tr.get(), 0.8472978603872034 * 1200.0 * 200e-12));
}

std::pair<int, std::string> run_i2c(std::string line) {
  std::array<std::string_view, kMaxArgs> buf{};
  const std::size_t n = tokenize_into(line, buf);
  std::span<const std::string_view> args{buf.data(), n};
  ArgCursor cur{args};
  std::ostringstream out;
  const int rc = wengine::cli::commands::I2cSpec::run(cur, out);
  return {rc, out.str()};
}

void test_cli() {
  {
    auto [rc, s] = run_i2c("pullup --vdd 3.3V --cb 200pF --speed fast");
    assert(rc == 0);
    assert(s.find("Rp(min)") != std::string::npos);
    assert(s.find("Rp(E12)") != std::string::npos);
  }
  {
    // Reference example through the CLI: UM10204 5V bus.
    auto [rc, s] = run_i2c("pullup --vdd 5.5V --cb 100pF --speed std");
    assert(rc == 0 && s.find("Rp(min) = 1700") != std::string::npos);
  }
  {
    // Explicit Rp violating tr reports FAIL with rc 1.
    auto [rc, s] = run_i2c("pullup --vdd 3.3V --cb 200pF --rp 4.7kOhm");
    assert(rc == 1 && s.find("FAIL") != std::string::npos);
  }
  {
    auto [rc, s] =
        run_i2c("series --vdd 3.3V --cb 200pF --rp 1.2kOhm --rs 100Ohm");
    assert(rc == 0 && s.find("VOL(bus)") != std::string::npos);
  }
  {
    auto [rc, s] = run_i2c("full --vdd 3.3V --cb 200pF --rp 1.2kOhm --rs 100Ohm");
    assert(rc == 0 && s.find("summary: PASS") != std::string::npos);
  }
  {
    auto [rc, s] = run_i2c("full --vdd 3.3V --cb 200pF --rp 4.7kOhm");
    assert(rc == 1 && s.find("summary: FAIL") != std::string::npos);
  }
  {
    // Hs-mode is rejected with an explanation, not sized.
    auto [rc, s] = run_i2c("pullup --vdd 3.3V --cb 100pF --speed hs");
    assert(rc == 2 && s.find("current-source") != std::string::npos);
  }
  {
    auto [rc, s] = run_i2c("pullup --vdd 3.3V");
    assert(rc == 2); // missing --cb
  }
  {
    auto [rc, s] = run_i2c("--help");
    assert(rc == 0 && s.find("pullup") != std::string::npos);
  }
}

} // namespace

int main() {
  test_reference_anchors();
  test_helpers();
  test_pullup_solve();
  test_series_solve();
  test_typed_api();
  test_cli();
  std::cout << "test_i2c: OK\n";
  return 0;
}
