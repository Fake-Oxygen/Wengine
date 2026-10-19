#include "wengine/core/runner.hpp"
#include "wengine/emit/routing.hpp"
#include "wengine/emit/signal_sink.hpp"
#include "wengine/link/frame.hpp"
#include "wengine/link/protocol.hpp"
#include "wengine/signal/types.hpp"
#include "wengine/solvers/run.hpp"
#include "wengine/solvers/signal.hpp"
#include "wengine/solvers/solver_registry.hpp"
#include "wengine/world/serialize.hpp"
#include "wengine/cli/args.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

using namespace wengine;

// Scaling-path proof support (file scope: explicit specializations must be
// declared at namespace scope enclosing wengine::units).
namespace scaling_proof {
using TestTemp = units::Dimension<0, 0, 0, 0, 1, 0, 0>;
constexpr units::Value<TestTemp> operator""_tC(long double v) {
  return units::Value<TestTemp>(static_cast<double>(v));
}
} // namespace scaling_proof

template <>
struct units::scales_for<scaling_proof::TestTemp> {
  static constexpr std::array<units::SuffixScale, 2> value{{{"tC", 1.0}, {"mtC", 1e-3}}};
};

namespace {

constexpr bool near(double a, double b, double eps = 1e-9) {
  return std::fabs(a - b) <= eps * (1.0 + std::fabs(b));
}

signal::SignalParams sine2v() {
  signal::SignalParams p;
  p.kind = signal::SignalKind::kSine;
  p.freq_hz = 1.0;
  p.amp = 2.0;
  return p;
}

void test_eval_goldens() {
  // Independent references: hand-derived closed forms (noise u01 verified
  // against a separate Python splitmix64 implementation 2026-09-23).
  const signal::SignalParams s = sine2v();
  assert(near(signal::eval_signal(s, 0.0, 0.0), 0.0, 1e-12)); // sin(0)
  assert(near(signal::eval_signal(s, 0.25, 0.0), 2.0, 1e-12)); // quarter period
  assert(near(signal::eval_signal(s, 0.5, 0.0), 0.0, 1e-12)); // half period
  assert(near(signal::eval_signal(s, 1.0, 0.0), 0.0, 1e-9)); // full period (libm)

  signal::SignalParams sq;
  sq.kind = signal::SignalKind::kSquare;
  sq.amp = 3.0;
  sq.offset = 1.0;
  assert(signal::eval_signal(sq, 0.0, 0.0) == 4.0); // exact: compare only
  assert(signal::eval_signal(sq, 0.75, 0.0) == -2.0);

  signal::SignalParams pu;
  pu.kind = signal::SignalKind::kPulse;
  pu.amp = 5.0;
  pu.duty = 0.25;
  assert(signal::eval_signal(pu, 0.0, 0.0) == 5.0);
  assert(signal::eval_signal(pu, 0.5, 0.0) == 0.0);

  signal::SignalParams tr;
  tr.kind = signal::SignalKind::kTriangle;
  tr.amp = 2.0;
  tr.offset = 1.0;
  assert(signal::eval_signal(tr, 0.0, 0.0) == -1.0);
  assert(signal::eval_signal(tr, 0.5, 0.0) == 3.0);
  assert(signal::eval_signal(tr, 0.25, 0.0) == 1.0);

  signal::SignalParams saw;
  saw.kind = signal::SignalKind::kSaw;
  saw.amp = 2.0;
  assert(signal::eval_signal(saw, 0.0, 0.0) == -2.0);
  assert(near(signal::eval_signal(saw, 0.5, 0.0), 0.0, 1e-12));

  signal::SignalParams dc;
  dc.kind = signal::SignalKind::kDc;
  dc.offset = 3.3;
  assert(signal::eval_signal(dc, 123.456, 0.0) == 3.3);

  // Noise golden: seed 1 -> u01 = 0.5665615751722809 (independent Python
  // reference); single rounding op past that, hence the tight epsilon.
  signal::SignalParams nz;
  nz.kind = signal::SignalKind::kNoise;
  nz.amp = 3.3;
  nz.seed = 1;
  const double u = signal::u01_from_bits(signal::rng_next(1));
  assert(near(u, 0.5665615751722809, 1e-15));
  assert(near(signal::eval_signal(nz, 0.0, u), 0.4393063961370539, 1e-12));
}

void test_parse_and_validate() {
  assert(signal::parse_kind("sine").value == signal::SignalKind::kSine);
  assert(signal::parse_kind("noise").value == signal::SignalKind::kNoise);
  assert(signal::parse_kind("expression").value == signal::SignalKind::kExpression);
  assert(signal::parse_kind("table").value == signal::SignalKind::kTable);
  assert(signal::parse_kind("lua").value == signal::SignalKind::kLua);
  assert(!signal::parse_kind("sawtooth").ok());
  assert(!signal::parse_kind("").ok());
  assert(signal::parse_quantity("U").value == signal::QuantityId::kVoltage);
  assert(signal::parse_quantity("I").value == signal::QuantityId::kCurrent);
  assert(!signal::parse_quantity("W").ok());
  assert(signal::quantity_suffix(signal::QuantityId::kVoltage) == "U");
  assert(signal::quantity_suffix(signal::QuantityId::kCurrent) == "I");

  signal::SignalParams p;
  assert(signal::validate_params(p).ok());
  p.freq_hz = 0.0;
  assert(!signal::validate_params(p).ok()); // sine needs f > 0
  p.kind = signal::SignalKind::kDc;
  assert(signal::validate_params(p).ok()); // dc ignores freq
  p.kind = signal::SignalKind::kNoise;
  assert(signal::validate_params(p).ok());
  p.kind = signal::SignalKind::kSquare;
  p.freq_hz = 1.0;
  p.duty = 1.5;
  assert(!signal::validate_params(p).ok());
  p.duty = 1.0;
  assert(signal::validate_params(p).ok());
  p.kind = signal::SignalKind::kSine;
  assert(signal::validate_params(p).ok()); // sine ignores duty
  p.kind = signal::SignalKind::kExpression;
  assert(!signal::validate_params(p).ok()); // empty program rejected
  auto expr_ok = signal::parse_expression("2*t+1");
  assert(expr_ok.ok());
  p.expr = expr_ok.value;
  assert(signal::validate_params(p).ok());
  p.kind = signal::SignalKind::kTable;
  p.expr = signal::ExprProgram{};
  assert(signal::validate_params(p).ok()); // points checked at configure
}

void test_validate_finite_and_unknown() {
  // Non-finite active numerics, unknown kinds, and unknown quantities
  // reject; valid extremes still pass; rejected configures leave the pool
  // untouched (validated before attach).
  const double inf = std::numeric_limits<double>::infinity();
  const double nan = std::numeric_limits<double>::quiet_NaN();

  signal::SignalParams p; // default sine, all finite
  assert(signal::validate_params(p).ok());
  p.freq_hz = inf;
  assert(!signal::validate_params(p).ok()); // periodic freq must be finite
  p.freq_hz = 1.0;
  p.amp = nan;
  assert(!signal::validate_params(p).ok());
  p.amp = 1.0;
  p.phase_rad = -inf;
  assert(!signal::validate_params(p).ok());
  p.phase_rad = 0.0;
  p.offset = nan;
  assert(!signal::validate_params(p).ok()); // offset stamps every kind
  p.offset = 0.0;
  assert(signal::validate_params(p).ok());

  p.amp = 1e308; // valid extremes still pass
  p.freq_hz = std::numeric_limits<double>::max();
  assert(signal::validate_params(p).ok());

  p.kind = signal::SignalKind::kDc; // dc ignores amp/freq, not offset
  p.amp = inf;
  p.freq_hz = 0.0;
  assert(signal::validate_params(p).ok());
  p.offset = inf;
  assert(!signal::validate_params(p).ok());
  p.offset = 0.0;
  p.amp = 1.0;

  p.kind = static_cast<signal::SignalKind>(250); // no switch fallthrough
  assert(!signal::validate_params(p).ok());
  p.kind = signal::SignalKind::kNoise;
  assert(signal::validate_params(p).ok());
  p.amp = nan;
  assert(!signal::validate_params(p).ok()); // noise reads amp
  p.amp = 1.0;

  p.quantity = static_cast<signal::QuantityId>(9);
  assert(!signal::validate_params(p).ok());
  assert(!signal::is_valid_quantity(static_cast<signal::QuantityId>(9)));
  assert(signal::is_valid_quantity(signal::QuantityId::kVoltage));
  p.quantity = signal::QuantityId::kCurrent;
  assert(signal::validate_params(p).ok());

  world::State s;
  signal::SignalParams bad;
  bad.kind = signal::SignalKind::kSine;
  bad.amp = inf;
  const world::Entity e = s.spawn_entity();
  assert(!world::configure_source(e, s.signals, bad, 0.0).ok());
  assert(s.signals.live_count == 0); // rejected before attach

  // Table/Lua attaches reject unknown quantity tags (pool untouched).
  std::vector<signal::TablePoint> pts;
  std::string error;
  std::istringstream good("time,value\n0,0.0\n1,10.0\n");
  assert(signal::parse_table_csv(good, pts, error).ok());
  world::State t;
  std::uint64_t off = 0;
  assert(world::append_table_points(t.signals, std::span(pts.data(), pts.size()), off)
             .ok());
  const std::uint64_t npts = static_cast<std::uint64_t>(pts.size());
  const world::Entity t0 = t.spawn_entity();
  assert(world::attach_table_source(t0, t.signals, off, npts,
                                    signal::QuantityId::kVoltage, 0.0)
             .ok());
  const world::Entity t1 = t.spawn_entity();
  const auto bad_qty = static_cast<signal::QuantityId>(9);
  assert(!world::attach_table_source(t1, t.signals, off, npts, bad_qty, 0.0).ok());
  assert(t.signals.live_count == 1);
  const std::string_view script = "return 1";
  std::uint64_t loff = 0;
  assert(world::append_lua_script(t.signals, script, loff).ok());
  const std::uint64_t nbytes = static_cast<std::uint64_t>(script.size());
  const world::Entity l0 = t.spawn_entity();
  assert(world::attach_lua_source(l0, t.signals, loff, nbytes,
                                  signal::QuantityId::kCurrent, 0.0)
             .ok());
  const world::Entity l1 = t.spawn_entity();
  assert(!world::attach_lua_source(l1, t.signals, loff, nbytes, bad_qty, 0.0).ok());
  assert(t.signals.live_count == 2);
}

world::Entity spawn(core::Runner &runner, const signal::SignalParams &p) {
  world::Entity e = runner.state().spawn_entity();
  assert(world::configure_source(e, runner.state().signals, p, runner.state().sim_time).ok());
  return e;
}

void test_solver_golden_and_snapshot() {
  // 1 Hz sine, dt = 1 ms, 1000 steps -> t = 1 s (one full period).
  core::Runner runner{{.dt = 0.001}};
  solvers::SignalSolver solver;
  assert(solver.name() == "signal");
  runner.add_solver(solver);
  spawn(runner, sine2v());
  assert(runner.run(1000).ok());

  const world::State &st = runner.state();
  assert(st.step_index == 1000 && near(st.sim_time, 1.0, 1e-9));
  // End-of-step convention: snapshot at T carries value(T) = 2*sin(2pi).
  assert(near(st.signals.value[0], 0.0, 1e-9));

  const world::Snapshot snap = world::take_snapshot(st);
  assert(snap.signal_count() == 1);
  assert(snap.sig_serial[0] == 0);
  assert(snap.sig_quantity[0] == static_cast<std::uint8_t>(signal::QuantityId::kVoltage));
  assert(snap.sig_value[0] == st.signals.value[0]);
  assert(snap.particle_count() == 0);
}

void test_noise_sequence_and_determinism() {
  auto drive = [] {
    core::Runner r{{.dt = 0.01}};
    solvers::SignalSolver s;
    r.add_solver(s);
    signal::SignalParams p;
    p.kind = signal::SignalKind::kNoise;
    p.amp = 1.0;
    p.seed = 7;
    spawn(r, p);
    assert(r.run(5).ok());
    return r;
  };
  const core::Runner r1 = drive();
  const core::Runner r2 = drive();
  assert(r1.state() == r2.state()); // exact repeat
  // Sequence advances one draw per step in slot order: rng holds draw 5.
  std::uint64_t rng = 7;
  double last = 0.0;
  for (int k = 0; k < 5; ++k) {
    rng = signal::rng_next(rng);
    last = 2.0 * signal::u01_from_bits(rng) - 1.0;
  }
  assert(r1.state().signals.rng[0] == rng);
  assert(r1.state().signals.value[0] == last);
}

void test_holes_and_bad_dt() {
  core::Runner runner{{.dt = 0.01}};
  solvers::SignalSolver solver;
  runner.add_solver(solver);
  spawn(runner, sine2v());
  signal::SignalParams dc;
  dc.kind = signal::SignalKind::kDc;
  dc.offset = 9.0;
  spawn(runner, dc);
  runner.state().signals.detach(1); // hole: solver must skip, value frozen
  assert(runner.run(10).ok());
  assert(runner.state().signals.value[1] == 9.0);

  world::State s;
  assert(!solver.step(s, 0.0).ok());
  assert(!solver.step(s, -0.01).ok());
}

void test_run_headless_signal() {
  core::RunConfig cfg;
  cfg.loop.dt = 0.01;
  cfg.steps = 10;
  cfg.solver = "signal";
  cfg.particles = 0;
  world::State out;
  assert(solvers::run_headless(cfg, std::span<emit::ISink *>{}, out).ok());
  assert(out.signals.live_count == 1);
  // Default sine 1 Hz at t = 0.1 s: sin(2*pi*0.1).
  assert(near(out.signals.value[0], 0.5877852522924731, 1e-9));

  // Batch seeds derive (seed + index): two noise sources never alias.
  core::RunConfig cfg2 = cfg;
  cfg2.signal.params.kind = signal::SignalKind::kNoise;
  cfg2.signal.params.amp = 1.0;
  cfg2.signal.count = 2;
  world::State out2;
  assert(solvers::run_headless(cfg2, std::span<emit::ISink *>{}, out2).ok());
  assert(out2.signals.live_count == 2);
  assert(out2.signals.value[0] != out2.signals.value[1]);
  assert(out2.signals.seed[0] == 1 && out2.signals.seed[1] == 2);

  // Rejections: set signal section with projectile, gravity with signal,
  // invalid params (sine at 0 Hz, square duty out of range).
  core::RunConfig bad = cfg;
  bad.solver = "projectile";
  bad.signal.set = true;
  assert(!solvers::run_headless(bad, std::span<emit::ISink *>{}, out).ok());
  bad = cfg;
  bad.has_gravity = true;
  assert(!solvers::run_headless(bad, std::span<emit::ISink *>{}, out).ok());
  bad = cfg;
  bad.signal.params.freq_hz = 0.0;
  assert(!solvers::run_headless(bad, std::span<emit::ISink *>{}, out).ok());
  bad = cfg;
  bad.signal.params.kind = signal::SignalKind::kSquare;
  bad.signal.params.duty = 2.0;
  assert(!solvers::run_headless(bad, std::span<emit::ISink *>{}, out).ok());
}

void test_frame_adapter() {
  core::Runner runner{{.dt = 0.01}};
  solvers::SignalSolver solver;
  runner.add_solver(solver);
  signal::SignalParams p = sine2v();
  p.quantity = signal::QuantityId::kCurrent;
  spawn(runner, p);
  assert(runner.run(3).ok());
  const link::Frame f =
      link::frame_from_signal_snapshot(world::take_snapshot(runner.state()));
  assert(f.step == 3 && near(f.time, 0.03, 1e-12));
  assert(f.channels.size() == 2);
  assert(f.channels[0].name == "sig.count" && f.channels[0].value == 1.0);
  assert(f.channels[1].name == "sig.0.I");
  assert(f.channels[1].value == runner.state().signals.value[0]);
  assert(f.items.empty());

  // Encode/decode round-trip is exact (existing wire path, new payload).
  link::LinkFrame wire{};
  assert(link::encode_frame(f, wire, 42).ok());
  link::Frame back;
  assert(link::decode_frame(wire, back).ok());
  assert(back == f);
}

void test_signals_csv_round_trip() {
  const std::string path = "/tmp/wengine_test_signal.csv";
  std::remove(path.c_str());
  core::Runner runner{{.dt = 0.01, .emit_every = 2}};
  solvers::SignalSolver solver;
  runner.add_solver(solver);
  spawn(runner, sine2v());
  emit::SignalsCsvSink sink(path);
  assert(sink.ok());
  runner.add_sink(sink);
  assert(runner.run(6).ok());
  assert(sink.flush().ok());
  assert(sink.rows_written() == 3); // steps 2, 4, 6

  std::ifstream in(path);
  std::string header;
  assert(static_cast<bool>(std::getline(in, header)));
  assert(header == "step,time,sig_id,value,quantity");
  std::vector<world::Snapshot> snaps;
  std::string error;
  in.clear();
  in.seekg(0);
  assert(emit::parse_signals_log(in, snaps, error).ok());
  assert(snaps.size() == 3);
  assert(snaps[0].step_index == 2 && snaps[0].signal_count() == 1);
  assert(snaps[0].sig_serial[0] == 0 && snaps[0].sig_quantity[0] == 0);
  // Exact round-trip: max_digits10 doubles re-parse bit-identically.
  assert(snaps[2].sig_value[0] == runner.state().signals.value[0]);
  std::remove(path.c_str());

  // Malformed logs rejected loudly.
  for (const std::string &bad :
       {"", "step,time,slot,x\n", "step,time,sig_id,value,quantity\n1,0.01,0,abc,U\n",
        "step,time,sig_id,value,quantity\n1,0.01,0,0.5,X\n"}) {
    std::istringstream bin(bad);
    assert(!emit::parse_signals_log(bin, snaps, error).ok());
    assert(!error.empty() && snaps.empty());
  }
}

void test_serialize_signals() {
  core::Runner runner{{.dt = 0.01}};
  solvers::SignalSolver solver;
  runner.add_solver(solver);
  spawn(runner, sine2v());
  assert(runner.run(5).ok());

  std::ostringstream bytes;
  assert(world::save_state(runner.state(), bytes).ok());
  world::State dst;
  std::istringstream in(bytes.str());
  assert(world::load_state(dst, in).ok());
  assert(dst == runner.state());
  assert(world::take_snapshot(dst) == world::take_snapshot(runner.state()));

  // v1 blob (no signal section) still loads: signals come back empty.
  std::string v1;
  v1 += 'W';
  v1 += 'E';
  v1 += 'N';
  v1 += 'G';
  const std::uint32_t ver = 1;
  for (int k = 0; k < 4; ++k) {
    v1 += static_cast<char>((ver >> (8 * k)) & 0xFFu);
  }
  v1.append(8, '\0'); // sim_time
  v1.append(8, '\0'); // step_index
  v1.append(4, '\0'); // entities
  v1.append(4, '\0'); // freelist
  v1.append(4, '\0'); // particles
  world::State old;
  std::istringstream vin(v1);
  assert(world::load_state(old, vin).ok());
  assert(old == world::State{});
  assert(world::take_snapshot(old).signal_count() == 0);
}

void test_registry() {
  static_assert(solvers::SolverRegistry::find("signal") != nullptr);
  static_assert(solvers::SolverRegistry::all().size() == 2);
  const solvers::SolverDef *def = solvers::SolverRegistry::find("signal");
  assert(def != nullptr);
  std::unique_ptr<core::ISolver> solver = def->create();
  assert(solver && solver->name() == "signal");
}

void test_quantity_table() {
  // Single-table round-trips: every spelling parses, every id has a
  // suffix/name/scales row, codes are dense (sync with DimensionFor).
  static_assert(signal::kQuantityCount == 2);
  for (const signal::QuantityDesc &d : signal::kQuantities) {
    assert(!d.suffix.empty() && !d.name.empty() && !d.scales.empty());
    for (std::string_view spelling : d.spellings) {
      assert(signal::parse_quantity(spelling).value == d.id);
    }
    assert(signal::quantity_suffix(d.id) == d.suffix);
    assert(signal::quantity_name(d.id) == d.name);
  }
  assert(signal::quantity_suffix(static_cast<signal::QuantityId>(9)) == "?");
  assert(!signal::parse_quantity("W").ok());
  assert(!signal::parse_quantity("").ok());

  // Quantity-aware value parsing through the units-owned tables (the only
  // parser commands need): voltage suffixes work for U, current suffixes
  // for I, and cross-dimension text is rejected — the dimensional firewall.
  using wengine::cli::parse_quantity_value;
  using signal::QuantityId;
  auto u = parse_quantity_value(QuantityId::kVoltage, "3.3kV");
  assert(u.ok() && u.value == 3300.0);
  assert(parse_quantity_value(QuantityId::kVoltage, "12V").value == 12.0);
  assert(parse_quantity_value(QuantityId::kVoltage, "5mV").value == 0.005);
  auto i = parse_quantity_value(QuantityId::kCurrent, "2mA");
  assert(i.ok() && i.value == 0.002);
  assert(!parse_quantity_value(QuantityId::kVoltage, "2mA").ok());
  assert(!parse_quantity_value(QuantityId::kCurrent, "3.3kV").ok());
  assert(!parse_quantity_value(QuantityId::kVoltage, "abc").ok());
  assert(!parse_quantity_value(static_cast<QuantityId>(9), "1V").ok());
}

void test_stamped_values() {
  // Typed boundary: stamp() carries the dimension tag AND a matching
  // Value<Dim>; misreading volts as amps fails to compile wherever the
  // variant is consumed (checked here at runtime over live snapshot data).
  using signal::QuantityId;
  using units::ElectricCurrent;
  using units::ElectricPotential;
  const signal::StampedValue v = signal::stamp(QuantityId::kVoltage, 3.3);
  assert(std::holds_alternative<units::Value<ElectricPotential>>(v));
  assert(std::get<units::Value<ElectricPotential>>(v).get() == 3.3);
  const signal::StampedValue c = signal::stamp(QuantityId::kCurrent, 0.05);
  assert(std::holds_alternative<units::Value<ElectricCurrent>>(c));

  // Stamp every live sample of a real run and verify dimension + value.
  core::Runner runner{{.dt = 0.01}};
  solvers::SignalSolver solver;
  runner.add_solver(solver);
  signal::SignalParams p;
  p.quantity = QuantityId::kCurrent;
  p.kind = signal::SignalKind::kDc;
  p.offset = 0.5;
  spawn(runner, p);
  assert(runner.run(2).ok());
  const world::Snapshot snap = world::take_snapshot(runner.state());
  assert(snap.signal_count() == 1);
  const signal::StampedValue s =
      signal::stamp(static_cast<QuantityId>(snap.sig_quantity[0]), snap.sig_value[0]);
  assert(std::holds_alternative<units::Value<ElectricCurrent>>(s));
  assert(std::get<units::Value<ElectricCurrent>>(s).get() == 0.5);
  // std::visit dispatches on the stamped dimension (the control-plane
  // pattern: one handler per physical quantity, chosen by the tag).
  double visited = -1.0;
  const char *seen = "";
  std::visit(
      [&](const auto &val) {
        visited = val.get();
        using V = std::decay_t<decltype(val)>;
        if constexpr (std::is_same_v<V, units::Value<ElectricCurrent>>) {
          seen = "current";
        } else {
          seen = "voltage";
        }
      },
      s);
  assert(visited == 0.5 && std::string_view(seen) == "current");
}

void test_expression_parse_and_eval() {
  // Independent references: hand-derived arithmetic (exact cases use ==,
  // libm cases use near — same discipline as the parametric goldens).
  auto eval_ok = [](std::string_view text, double t) {
    const auto pr = signal::parse_expression(text);
    assert(pr.ok());
    assert(signal::validate_program(pr.value));
    const auto r = signal::eval_expression(pr.value, t);
    assert(r.ok());
    return r.value;
  };
  assert(eval_ok("2*t+1", 0.5) == 2.0);
  assert(eval_ok("2+3*4", 0.0) == 14.0);
  assert(eval_ok("(2+3)*4", 0.0) == 20.0);
  assert(eval_ok("10-2-3", 0.0) == 5.0); // left-assoc
  assert(eval_ok("2^3", 0.0) == 8.0);
  assert(eval_ok("2^3^2", 0.0) == 512.0); // right-assoc
  assert(eval_ok("-2^2", 0.0) == -4.0); // -(2^2)
  assert(eval_ok("2^-3", 0.0) == 0.125);
  assert(eval_ok("-t", 3.0) == -3.0);
  assert(eval_ok("min(2,3)", 0.0) == 2.0);
  assert(eval_ok("max(2,3)", 0.0) == 3.0);
  assert(eval_ok("abs(-5)", 0.0) == 5.0);
  assert(eval_ok("sqrt(4)", 0.0) == 2.0);
  assert(eval_ok("pi", 0.0) == 3.141592653589793);
  assert(eval_ok("e", 0.0) == 2.718281828459045);
  assert(near(eval_ok("sin(2*pi*t)", 0.25), 1.0, 1e-12));
  assert(near(eval_ok("cos(0)", 0.0), 1.0, 1e-15));
  assert(near(eval_ok("exp(1)", 0.0), 2.718281828459045, 1e-12));
  assert(near(eval_ok("log(e)", 0.0), 1.0, 1e-12));
  assert(near(eval_ok("sqrt(2)", 0.0), 1.4142135623730951, 1e-12));
  assert(near(eval_ok("  sin ( t ) ", 0.0), 0.0, 1e-15)); // whitespace

  // Malformed text is rejected loudly (never a partial program).
  const char *bad[] = {"",
                       "   ",
                       "2+",
                       "2*",
                       "(1",
                       "1)",
                       "sin(",
                       "sin(1",
                       "min(1)",
                       "min(1,2,3)",
                       "foo",
                       "2t",
                       "2foo",
                       "+t",
                       "()",
                       "2 2",
                       "log",
                       "2^",
                       "^2",
                       "min(,)",
                       "3 + ",
                       "2..3"};
  for (const char *text : bad) {
    assert(!signal::parse_expression(text).ok());
  }

  // Well-formed but failing evaluation: div-by-zero, out-of-domain,
  // overflow — Status, never NaN/inf samples.
  auto eval_fail = [](std::string_view text, double t) {
    const auto pr = signal::parse_expression(text);
    assert(pr.ok());
    assert(!signal::eval_expression(pr.value, t).ok());
  };
  eval_fail("1/t", 0.0);
  eval_fail("1/(t-1)", 1.0);
  eval_fail("0/0", 0.0);
  eval_fail("log(t)", 0.0);
  eval_fail("log(0-1)", 0.0);
  eval_fail("sqrt(0-1)", 0.0);
  eval_fail("sqrt(t)", -1.0);
  eval_fail("exp(10000)", 0.0);
  eval_fail("10^10000", 0.0);

  // Structural validation rejects empty and stack-broken programs.
  signal::ExprProgram empty;
  assert(!signal::validate_program(empty));
  signal::ExprProgram broken;
  broken.size = 1;
  broken.nodes[0] = signal::ExprNode{signal::ExprOp::kAdd, 0.0};
  assert(!signal::validate_program(broken));
}

world::Entity spawn_expr(core::Runner &runner, std::string_view text) {
  signal::SignalParams p;
  p.kind = signal::SignalKind::kExpression;
  const auto pr = signal::parse_expression(text);
  assert(pr.ok());
  p.expr = pr.value;
  world::Entity e = runner.state().spawn_entity();
  assert(world::configure_source(e, runner.state().signals, p, runner.state().sim_time)
             .ok());
  return e;
}

void test_expression_solver() {
  core::Runner runner{{.dt = 0.1}};
  solvers::SignalSolver solver;
  runner.add_solver(solver);
  const world::Entity e = spawn_expr(runner, "2*t+1");
  assert(runner.state().signals.value[e.index] == 1.0); // eval(0)
  assert(runner.run(2).ok());
  // t = 0.2 (exact: two 0.1 steps): 2*0.2+1.
  assert(near(runner.state().signals.value[e.index], 1.4, 1e-9));

  // Determinism: an identical run is bit-identical (pool columns included).
  core::Runner runner2{{.dt = 0.1}};
  solvers::SignalSolver solver2;
  runner2.add_solver(solver2);
  spawn_expr(runner2, "2*t+1");
  assert(runner2.run(2).ok());
  assert(runner2.state() == runner.state());

  // Expression sine golden matches the parametric sine at shared times.
  core::Runner re{{.dt = 0.001}};
  solvers::SignalSolver se;
  re.add_solver(se);
  spawn_expr(re, "2*sin(2*pi*t)");
  assert(re.run(250).ok());
  assert(near(re.state().signals.value[0], 2.0, 1e-9)); // quarter period
}

void test_expression_divzero_freezes_step() {
  // Overflow-based failure over an interval (robust to FP accumulation):
  // exp(t) overflows past t ~= 709.78; dt = 1 s keeps times exact.
  core::Runner runner{{.dt = 1.0}};
  solvers::SignalSolver solver;
  runner.add_solver(solver);
  spawn_expr(runner, "exp(t)");
  assert(runner.state().signals.value[0] == 1.0);
  const core::Status st = runner.run(1000);
  assert(!st.ok() && st.code == core::StatusCode::kInvalidArgument);
  // The failed step advanced neither time nor index and emitted nothing:
  // steps 1..709 applied (t = 1..709 exact), step 710 (t = 710) failed.
  assert(runner.state().step_index == 709);
  assert(runner.state().sim_time == 709.0);
  assert(std::isfinite(runner.state().signals.value[0]));

  // A source already failing at t = 0 is rejected at configure time.
  signal::SignalParams p;
  p.kind = signal::SignalKind::kExpression;
  p.expr = signal::parse_expression("1/t").value;
  world::Entity e = runner.state().spawn_entity();
  assert(!world::configure_source(e, runner.state().signals, p, 0.0).ok());
  // kTable needs points: the params-only path rejects it.
  signal::SignalParams tp;
  tp.kind = signal::SignalKind::kTable;
  world::Entity e2 = runner.state().spawn_entity();
  assert(!world::configure_source(e2, runner.state().signals, tp, 0.0).ok());
}

void test_table_csv_and_interp() {
  std::vector<signal::TablePoint> pts;
  std::string error;
  std::istringstream good("time,value\n0,0.0\n1,10.0\n2,20.0\n");
  assert(signal::parse_table_csv(good, pts, error).ok());
  assert(pts.size() == 3);
  assert(pts[1].t == 1.0 && pts[1].v == 10.0);

  const char *bad[] = {"",
                       "time,value\n",
                       "step,time\n0,1\n",
                       "time,value\n0,0\n0,1\n",
                       "time,value\n1,0\n0,1\n",
                       "time,value\n0,0,0\n",
                       "time,value\n0\n",
                       "time,value\n0,abc\n",
                       "time,value\n0,nan\n",
                       "time,value\n,1\n",
                       "time,value\n\n"};
  for (const char *text : bad) {
    std::istringstream bin(text);
    assert(!signal::parse_table_csv(bin, pts, error).ok());
    assert(!error.empty() && pts.empty());
  }
  assert(!signal::validate_table_points(std::span<const signal::TablePoint>{}).ok());

  // Hand-derived linear goldens: exact hits (==), mids, clamps.
  const std::vector<double> ts{0.0, 1.0, 2.0};
  const std::vector<double> vs{0.0, 10.0, 20.0};
  auto ev = [&](double t) {
    return signal::eval_table(std::span(ts.data(), ts.size()),
                              std::span(vs.data(), vs.size()), t);
  };
  assert(ev(-5.0) == 0.0);
  assert(ev(0.0) == 0.0);
  assert(ev(0.5) == 5.0);
  assert(ev(1.0) == 10.0);
  assert(ev(1.25) == 12.5);
  assert(ev(2.0) == 20.0);
  assert(ev(99.0) == 20.0);
}

void test_table_solver_and_replay() {
  // Byte-exact replay: exact-hit times return stored values bit-identically.
  const std::vector<signal::TablePoint> pts{
      {0.0, 0.0}, {0.1, 0.5877852522924731}, {0.2, 0.9510565162951535}};
  core::Runner runner{{.dt = 0.1}};
  solvers::SignalSolver solver;
  runner.add_solver(solver);
  world::Entity e = runner.state().spawn_entity();
  assert(world::configure_table_source(e, runner.state().signals,
                                       std::span(pts.data(), pts.size()),
                                       signal::QuantityId::kVoltage, 0.0)
             .ok());
  assert(runner.state().signals.value[e.index] == 0.0); // table(0), clamped
  assert(runner.run(1).ok());
  assert(runner.state().signals.value[e.index] == 0.5877852522924731);
  assert(runner.run(1).ok());
  assert(runner.state().signals.value[e.index] == 0.9510565162951535);
  assert(runner.run(1).ok()); // past the end: holds last
  assert(runner.state().signals.value[e.index] == 0.9510565162951535);

  // Determinism across an identical run (shared-nothing rebuild).
  core::Runner runner2{{.dt = 0.1}};
  solvers::SignalSolver solver2;
  runner2.add_solver(solver2);
  world::Entity e2 = runner2.state().spawn_entity();
  assert(world::configure_table_source(e2, runner2.state().signals,
                                       std::span(pts.data(), pts.size()),
                                       signal::QuantityId::kVoltage, 0.0)
             .ok());
  assert(runner2.run(3).ok());
  assert(runner2.state() == runner.state());

  // Validation: empty and non-increasing spans rejected, pool untouched.
  const std::size_t live_before = runner.state().signals.live_count;
  world::Entity e3 = runner.state().spawn_entity();
  assert(!world::configure_table_source(e3, runner.state().signals,
                                        std::span<const signal::TablePoint>{},
                                        signal::QuantityId::kVoltage, 0.0)
              .ok());
  const std::vector<signal::TablePoint> rev{{1.0, 0.0}, {0.0, 1.0}};
  assert(!world::configure_table_source(e3, runner.state().signals,
                                        std::span(rev.data(), rev.size()),
                                        signal::QuantityId::kVoltage, 0.0)
              .ok());
  assert(runner.state().signals.live_count == live_before);
}

void test_run_headless_custom() {
  // Expression batch: 2*t+1 at dt = 0.1, 2 steps -> t = 0.2.
  core::RunConfig cfg;
  cfg.loop.dt = 0.1;
  cfg.steps = 2;
  cfg.solver = "signal";
  cfg.particles = 0;
  cfg.signal.params.kind = signal::SignalKind::kExpression;
  cfg.signal.params.expr = signal::parse_expression("2*t+1").value;
  world::State out;
  assert(solvers::run_headless(cfg, std::span<emit::ISink *>{}, out).ok());
  assert(out.signals.live_count == 1);
  assert(near(out.signals.value[0], 1.4, 1e-9));

  // Table batch: shared range, linear mid.
  core::RunConfig cfg2;
  cfg2.loop.dt = 0.1;
  cfg2.steps = 5;
  cfg2.solver = "signal";
  cfg2.particles = 0;
  cfg2.signal.params.kind = signal::SignalKind::kTable;
  const std::vector<signal::TablePoint> pts{{0.0, 1.0}, {1.0, 3.0}};
  cfg2.signal.table = pts;
  world::State out2;
  assert(solvers::run_headless(cfg2, std::span<emit::ISink *>{}, out2).ok());
  assert(out2.signals.live_count == 1);
  assert(near(out2.signals.value[0], 2.0, 1e-9));

  // Rejections: table kind without points, points with a non-table kind,
  // program with a non-expression kind, empty program, program+points mix.
  world::State sink_state;
  core::RunConfig bad = cfg2;
  bad.signal.table.clear();
  assert(!solvers::run_headless(bad, std::span<emit::ISink *>{}, sink_state).ok());
  bad = cfg2;
  bad.signal.params.kind = signal::SignalKind::kSine;
  assert(!solvers::run_headless(bad, std::span<emit::ISink *>{}, sink_state).ok());
  bad = cfg;
  bad.signal.table = pts;
  assert(!solvers::run_headless(bad, std::span<emit::ISink *>{}, sink_state).ok());
  bad = cfg;
  bad.signal.params.expr = signal::ExprProgram{};
  assert(!solvers::run_headless(bad, std::span<emit::ISink *>{}, sink_state).ok());
  bad = cfg2;
  bad.signal.params.kind = signal::SignalKind::kSine;
  bad.signal.params.expr = signal::parse_expression("t").value;
  assert(!solvers::run_headless(bad, std::span<emit::ISink *>{}, sink_state).ok());
}

void test_serialize_v4_custom() {
  core::Runner runner{{.dt = 0.1}};
  solvers::SignalSolver solver;
  runner.add_solver(solver);
  spawn_expr(runner, "2*t+1");
  const std::vector<signal::TablePoint> pts{{0.0, 1.0}, {1.0, 3.0}};
  world::Entity e = runner.state().spawn_entity();
  assert(world::configure_table_source(e, runner.state().signals,
                                       std::span(pts.data(), pts.size()),
                                       signal::QuantityId::kCurrent, 0.0)
             .ok());
  assert(runner.run(3).ok());

  std::ostringstream bytes;
  assert(world::save_state(runner.state(), bytes).ok());
  world::State dst;
  std::istringstream in(bytes.str());
  assert(world::load_state(dst, in).ok());
  assert(dst == runner.state());
  assert(world::take_snapshot(dst) == world::take_snapshot(runner.state()));

  // Truncation is still a loud failure with the target cleared.
  world::State cut_state;
  const std::string cut = bytes.str().substr(0, bytes.str().size() / 2);
  std::istringstream cut_in(cut);
  assert(!world::load_state(cut_state, cut_in).ok());

  // v3 blob fallback: one sine slot, no custom columns — loads with empty
  // programs, empty ranges, synthesized nothing (serials stored since v3).
  std::ostringstream v3;
  v3.put('W');
  v3.put('E');
  v3.put('N');
  v3.put('G');
  world::detail::put_u32_le(v3, 3);
  world::detail::put_f64_le(v3, 0.5);
  world::detail::put_u64_le(v3, 7);
  world::detail::put_u32_le(v3, 0); // entities
  world::detail::put_u32_le(v3, 0); // freelist
  world::detail::put_u32_le(v3, 0); // particles
  world::detail::put_u32_le(v3, 1); // one signal slot
  world::detail::put_u8(v3, 1); // present
  world::detail::put_u8(v3, 0); // kind sine
  world::detail::put_u8(v3, 0); // quantity U
  world::detail::put_u8(v3, 1); // running
  world::detail::put_u8(v3, 0); // op_count
  world::detail::put_f64_le(v3, 1.0); // freq
  world::detail::put_f64_le(v3, 2.0); // amp
  world::detail::put_f64_le(v3, 0.0); // offset
  world::detail::put_f64_le(v3, 0.0); // phase
  world::detail::put_f64_le(v3, 0.5); // duty
  world::detail::put_f64_le(v3, 0.0); // t0
  world::detail::put_f64_le(v3, 0.25); // value
  world::detail::put_u64_le(v3, 1); // seed
  world::detail::put_u64_le(v3, 9); // rng
  world::detail::put_u64_le(v3, 5); // serial
  for (int k = 0; k < 8; ++k) {
    world::detail::put_u64_le(v3, 0); // ops
  }
  world::State legacy;
  std::istringstream vin(v3.str());
  assert(world::load_state(legacy, vin).ok());
  assert(legacy.signals.live_count == 1);
  assert(legacy.signals.kind[0] == static_cast<std::uint8_t>(signal::SignalKind::kSine));
  assert(legacy.signals.serial[0] == 5);
  assert(legacy.signals.value[0] == 0.25);
  assert(legacy.signals.next_serial == 6);
  assert(legacy.signals.expr_len[0] == 0);
  assert(legacy.signals.table_len[0] == 0);
  assert(legacy.signals.table_times.empty());
}

void test_serialize_dead_custom_round_trip() {
  // detach() retains dead-slot columns while save_state writes them: the
  // load must accept that representation exactly (sizes/ranges checked, no
  // live-source invariant on dead slots).
  core::Runner runner{{.dt = 0.1}};
  solvers::SignalSolver solver;
  runner.add_solver(solver);
  const world::Entity xe = spawn_expr(runner, "2*t+1"); // serial 0
  const std::vector<signal::TablePoint> pts{{0.0, 1.0}, {1.0, 3.0}};
  const world::Entity te = runner.state().spawn_entity(); // serial 1
  assert(world::configure_table_source(te, runner.state().signals,
                                       std::span(pts.data(), pts.size()),
                                       signal::QuantityId::kVoltage, 0.0)
              .ok());
  assert(runner.run(2).ok());
  assert(runner.state().despawn_entity(xe));
  assert(runner.state().despawn_entity(te));
  assert(runner.state().signals.live_count == 0);

  std::ostringstream bytes;
  assert(world::save_state(runner.state(), bytes).ok());
  world::State dst;
  std::istringstream in(bytes.str());
  assert(world::load_state(dst, in).ok());
  assert(dst == runner.state()); // retained programs/ranges verbatim
  assert(world::take_snapshot(dst) == world::take_snapshot(runner.state()));
  assert(dst.signals.next_serial == 2); // dead serials still advance minting

  // Slot reuse after load: a fresh source mints the next serial (never a
  // retained dead one) and evaluates its own payload, never dead leftovers.
  world::State fork = runner.state();
  world::State dfork = dst;
  for (world::State *sp : {&fork, &dfork}) {
    const world::Entity ne = sp->spawn_entity();
    signal::SignalParams dp;
    dp.kind = signal::SignalKind::kDc;
    dp.offset = 7.0;
    assert(world::configure_source(ne, sp->signals, dp, sp->sim_time).ok());
    assert(sp->signals.serial[ne.index] == 2);
  }
  assert(dfork == fork);
  // Reuse with custom kinds too: the new table range shares the store with
  // the retained dead range, and the new program overwrites dead columns.
  for (world::State *sp : {&fork, &dfork}) {
    const world::Entity ne = sp->spawn_entity();
    const std::vector<signal::TablePoint> pts2{{0.0, 9.0}, {2.0, 1.0}};
    assert(world::configure_table_source(ne, sp->signals,
                                         std::span(pts2.data(), pts2.size()),
                                         signal::QuantityId::kCurrent, sp->sim_time)
                .ok());
    const world::Entity ee = sp->spawn_entity();
    signal::SignalParams xp;
    xp.kind = signal::SignalKind::kExpression;
    xp.expr = signal::parse_expression("t*3").value;
    assert(world::configure_source(ee, sp->signals, xp, sp->sim_time).ok());
  }
  std::ostringstream bytes2;
  assert(world::save_state(fork, bytes2).ok());
  world::State dst2;
  std::istringstream in2(bytes2.str());
  assert(world::load_state(dst2, in2).ok());
  assert(dst2 == fork);
  assert(dfork == fork);
  core::Runner r2{{.dt = 0.1}};
  solvers::SignalSolver s2;
  r2.add_solver(s2);
  r2.state() = fork;
  core::Runner r3{{.dt = 0.1}};
  solvers::SignalSolver s3;
  r3.add_solver(s3);
  r3.state() = dst2;
  assert(r2.run(3).ok() && r3.run(3).ok());
  assert(r2.state() == r3.state());

  // Genuinely corrupt dead ranges still fail: tamper a dead slot behind
  // configure's back, save the tampered pool, and the load must reject it.
  {
    world::State tampered = runner.state();
    tampered.signals.table_off[te.index] =
        static_cast<std::uint64_t>(tampered.signals.table_times.size() + 100);
    std::ostringstream bad;
    assert(world::save_state(tampered, bad).ok());
    world::State rej;
    std::istringstream bin(bad.str());
    assert(!world::load_state(rej, bin).ok());
  }
  {
    world::State tampered = runner.state();
    tampered.signals.expr_len[xe.index] =
        static_cast<std::uint8_t>(signal::kMaxExprNodes + 1);
    std::ostringstream bad;
    assert(world::save_state(tampered, bad).ok());
    world::State rej;
    std::istringstream bin(bad.str());
    assert(!world::load_state(rej, bin).ok());
  }
}

// Scaling-path proof: a test-only dimension (no production change) plugs
// into the generic units machinery — Dimension + literal + one scales row
// is the whole recipe. This is the "one-entry change" SIG-3 requires.
void test_dimension_scaling() {  using scaling_proof::TestTemp;
  using wengine::cli::detail::parse_scaled_impl;
  // Generic suffix-scale parsing accepts the new dimension untouched.
  const auto a = parse_scaled_impl("12mtC", units::scales_for<TestTemp>::value);
  assert(a.ok() && a.value == 0.012);
  assert(!parse_scaled_impl("12K", units::scales_for<TestTemp>::value).ok());
  // Value arithmetic + exact comparison work like every other dimension.
  constexpr units::Value<TestTemp> t = scaling_proof::operator""_tC(2.0);
  static_assert(t.get() == 2.0);
  assert(t + t == scaling_proof::operator""_tC(4.0));
}

struct SnapshotCapture : emit::ISink {
  std::vector<world::Snapshot> snaps;
  void on_snapshot(const world::Snapshot &s) override { snaps.push_back(s); }
};

void test_signals_csv_slot_recycle_round_trip() {
  // Review repro: serials 0,1 -> despawn slot 0 -> respawn parks serial 2
  // in the recycled slot, so the valid snapshot holds serials [2,1] in
  // slot order. The writer emits slot order; the parser must accept it
  // (uniqueness per step) and preserve it exactly.
  auto dc = [](double offset) {
    signal::SignalParams p;
    p.kind = signal::SignalKind::kDc;
    p.offset = offset;
    return p;
  };
  const std::string path = "/tmp/wengine_test_signal_recycle.csv";
  std::remove(path.c_str());
  core::Runner runner{{.dt = 0.01}};
  solvers::SignalSolver solver;
  runner.add_solver(solver);
  const world::Entity e0 = spawn(runner, dc(1.0)); // serial 0
  const world::Entity e1 = spawn(runner, dc(2.0)); // serial 1
  assert(runner.state().signals.serial[e0.index] == 0);
  assert(runner.state().signals.serial[e1.index] == 1);
  assert(runner.state().despawn_entity(e0));
  const world::Entity e2 = spawn(runner, dc(3.0)); // serial 2, recycled slot
  assert(e2.index == e0.index);
  assert(runner.state().signals.serial[e2.index] == 2);
  {
    const world::Snapshot live = world::take_snapshot(runner.state());
    assert(live.signal_count() == 2);
    assert(live.sig_serial[0] == 2 && live.sig_serial[1] == 1); // slot order
  }
  SnapshotCapture live_caps;
  emit::SignalsCsvSink sink(path);
  assert(sink.ok());
  runner.add_sink(live_caps);
  runner.add_sink(sink);
  assert(runner.run(2).ok());
  // Second recycle mid-log: drop serial 1, respawn as serial 3.
  assert(runner.state().despawn_entity(e1));
  const world::Entity e3 = spawn(runner, dc(5.0)); // serial 3
  assert(e3.index == e1.index);
  assert(runner.state().signals.serial[e3.index] == 3);
  assert(runner.run(2).ok());
  assert(sink.flush().ok());
  assert(live_caps.snaps.size() == 4);
  assert(live_caps.snaps[0].sig_serial[0] == 2 && live_caps.snaps[0].sig_serial[1] == 1);
  assert(live_caps.snaps[3].sig_serial[0] == 2 && live_caps.snaps[3].sig_serial[1] == 3);
  assert(live_caps.snaps[0].sig_value[0] == 3.0 && live_caps.snaps[0].sig_value[1] == 2.0);
  assert(live_caps.snaps[3].sig_value[0] == 3.0 && live_caps.snaps[3].sig_value[1] == 5.0);

  std::vector<world::Snapshot> snaps;
  std::string error;
  {
    std::ifstream in(path);
    assert(emit::parse_signals_log(in, snaps, error).ok());
  }
  assert(snaps.size() == 4);
  for (std::size_t k = 0; k < snaps.size(); ++k) {
    assert(snaps[k] == live_caps.snaps[k]); // exact, order-preserving
  }
  std::remove(path.c_str());

  // Same recycled order through subset routing: the wrapper preserves slot
  // order, so its CSV output round-trips identically.
  const std::string sub_path = "/tmp/wengine_test_signal_recycle_sub.csv";
  std::remove(sub_path.c_str());
  core::Runner runner2{{.dt = 0.01}};
  solvers::SignalSolver solver2;
  runner2.add_solver(solver2);
  const world::Entity f0 = spawn(runner2, dc(1.0)); // serial 0
  const world::Entity f1 = spawn(runner2, dc(2.0)); // serial 1
  assert(runner2.state().despawn_entity(f0));
  const world::Entity f2 = spawn(runner2, dc(3.0)); // serial 2
  const std::uint64_t s1 = runner2.state().signals.serial[f1.index];
  const std::uint64_t s2 = runner2.state().signals.serial[f2.index];
  SnapshotCapture full_caps;
  emit::SignalsCsvSink sub_csv(sub_path);
  assert(sub_csv.ok());
  emit::SignalSubsetSink sub(sub_csv);
  sub.set_subscription({s2, s1}); // recycled pair, slot order [2,1] regardless
  runner2.add_sink(full_caps);
  runner2.add_sink(sub);
  assert(runner2.run(2).ok());
  sub.set_subscription({s1}); // narrow to one source mid-run
  assert(runner2.run(1).ok());
  assert(sub_csv.flush().ok());
  {
    std::ifstream in(sub_path);
    assert(emit::parse_signals_log(in, snaps, error).ok());
  }
  assert(snaps.size() == 3);
  assert(snaps[0] == full_caps.snaps[0]);
  assert(snaps[1] == full_caps.snaps[1]);
  assert(snaps[2].signal_count() == 1);
  assert(snaps[2].sig_serial[0] == s1 && snaps[2].sig_value[0] == 2.0);
  std::remove(sub_path.c_str());

  // Non-ascending but unique rows are valid and keep file order.
  {
    std::istringstream in("step,time,sig_id,value,quantity\n"
                          "1,0.01,2,3,U\n"
                          "1,0.01,1,2,U\n");
    assert(emit::parse_signals_log(in, snaps, error).ok());
    assert(snaps.size() == 1);
    assert(snaps[0].sig_serial[0] == 2 && snaps[0].sig_serial[1] == 1);
    assert(snaps[0].sig_value[0] == 3.0 && snaps[0].sig_value[1] == 2.0);
  }
  // Duplicates (adjacent or not), inconsistent times, and non-increasing
  // steps still fail loudly with the output cleared.
  for (const std::string &bad :
       {"step,time,sig_id,value,quantity\n1,0.01,2,3,U\n1,0.01,2,4,U\n",
        "step,time,sig_id,value,quantity\n1,0.01,2,3,U\n1,0.01,1,2,U\n1,0.01,2,5,U\n",
        "step,time,sig_id,value,quantity\n1,0.01,2,3,U\n1,0.02,1,2,U\n",
        "step,time,sig_id,value,quantity\n2,0.02,2,3,U\n1,0.01,1,2,U\n"}) {
    std::istringstream bin(bad);
    assert(!emit::parse_signals_log(bin, snaps, error).ok());
    assert(!error.empty() && snaps.empty());
  }
}

} // namespace

int main() {
  test_eval_goldens();
  test_parse_and_validate();
  test_validate_finite_and_unknown();
  test_solver_golden_and_snapshot();
  test_noise_sequence_and_determinism();
  test_holes_and_bad_dt();
  test_run_headless_signal();
  test_frame_adapter();
  test_signals_csv_round_trip();
  test_signals_csv_slot_recycle_round_trip();
  test_serialize_signals();
  test_registry();
  test_quantity_table();
  test_stamped_values();
  test_dimension_scaling();
  test_expression_parse_and_eval();
  test_expression_solver();
  test_expression_divzero_freezes_step();
  test_table_csv_and_interp();
  test_table_solver_and_replay();
  test_run_headless_custom();
  test_serialize_v4_custom();
  test_serialize_dead_custom_round_trip();
  std::cout << "test_signal: OK\n";
  return 0;
}
