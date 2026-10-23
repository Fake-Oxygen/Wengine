// ROS2 control-plane tests: service vocabulary + step-boundary drain,
// no rclcpp (the rmw services are covered by live HIL: ros2_bridge +
// `ros2 service call` mid-run effects).
#include "wengine/core/runner.hpp"
#include "wengine/ros2/control.hpp"
#include "wengine/solvers/signal.hpp"
#include "wengine/solvers/signal_control.hpp"
#include "wengine/world/state.hpp"

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

namespace {

namespace rc = wengine::ros2::control;
using wengine::core::StatusCode;

void test_service_names_and_verbs() {
  using V = wengine::signal::control::Verb;
  assert(rc::verb_for(rc::kServiceAdd) == V::kAdd);
  assert(rc::verb_for(rc::kServiceSet) == V::kSet);
  assert(rc::verb_for(rc::kServiceStart) == V::kStart);
  assert(rc::verb_for(rc::kServiceStop) == V::kStop);
  assert(rc::verb_for(rc::kServiceDrop) == V::kDrop);
  assert(rc::verb_for(rc::kServiceList) == V::kList);
  assert(rc::verb_for("nope") == V::kList);
  assert(rc::verb_text(rc::kServiceAdd) == "ADD");
  assert(rc::verb_text(rc::kServiceList) == "LIST");
  assert(rc::service_name("wengine", rc::kServiceAdd) == "/wengine/sig_add");
  assert(rc::service_name("wengine", rc::kServiceList) == "/wengine/sig_list");
}

wengine::world::State make_two_source_state() {
  wengine::world::State st;
  wengine::signal::SignalParams p;
  const wengine::world::Entity e0 = st.spawn_entity();
  assert(wengine::world::configure_source(e0, st.signals, p, st.sim_time).ok());
  const wengine::world::Entity e1 = st.spawn_entity();
  assert(wengine::world::configure_source(e1, st.signals, p, st.sim_time).ok());
  return st;
}

void test_request_lines() {
  assert(rc::request_line(rc::kServiceAdd, "sine freq 1 amp 3.3") == "ADD sine freq 1 amp 3.3");
  assert(rc::request_line(rc::kServiceSet, "3 AMP 1.5") == "SET 3 AMP 1.5");
  assert(rc::request_line(rc::kServiceStart, "3") == "START 3");
  assert(rc::request_line(rc::kServiceStop, "3") == "STOP 3");
  assert(rc::request_line(rc::kServiceDrop, "3") == "DROP 3");
  assert(rc::request_line(rc::kServiceList, "") == "LIST");
}

void test_validate_shapes() {
  std::string line;
  assert(rc::validate_service_line(rc::kServiceAdd, "sine U FREQ 1 AMP 2", line).ok());
  assert(line == "ADD sine U FREQ 1 AMP 2");
  assert(rc::validate_service_line(rc::kServiceSet, "0 AMP 1.5", line).ok());
  assert(rc::validate_service_line(rc::kServiceStop, "0", line).ok());
  assert(rc::validate_service_line(rc::kServiceList, "", line).ok());
  assert(line == "LIST");
  // Enqueue-time validation is shape-only (same parser as TCP/UART):
  // semantic checks (freq > 0, unknown serials) happen at drain time.
  // FREQ -1 parses fine here and is refused by the drain (see below).
  assert(rc::validate_service_line(rc::kServiceAdd, "sine U FREQ -1", line).ok());
  wengine::world::State neg = make_two_source_state();
  wengine::solvers::ControlBus negbus(7);
  const std::uint32_t negch = negbus.add_channel();
  assert(negbus.enqueue(negch, line));
  assert(negbus.drain(neg, 0).rejected == 1);
  assert(negbus.take_responses(negch).at(0).rfind("ERR invalid_argument", 0) == 0);
  // Shape failures stay kInvalidArgument (same parser as TCP/UART).
  assert(rc::validate_service_line(rc::kServiceSet, "0 AMP", line).code ==
         StatusCode::kInvalidArgument);
  assert(rc::validate_service_line(rc::kServiceSet, "0 AMP abc", line).code ==
         StatusCode::kInvalidArgument);
  assert(rc::validate_service_line(rc::kServiceSet, "0 BOGUS 1", line).code ==
         StatusCode::kInvalidArgument);
  assert(rc::validate_service_line(rc::kServiceList, "junk", line).code ==
         StatusCode::kInvalidArgument);
  // Table/Lua ADD is refused as not_implemented (bulk paths own those).
  assert(rc::validate_service_line(rc::kServiceAdd, "table U", line).code ==
         StatusCode::kNotImplemented);
  assert(rc::validate_service_line(rc::kServiceAdd, "lua U", line).code ==
         StatusCode::kNotImplemented);
  // Overlong lines never reach the bus.
  const std::string big(5000, 'x');
  assert(rc::validate_service_line(rc::kServiceAdd, big, line).code ==
         StatusCode::kInvalidArgument);
}

void test_outcomes() {
  const rc::ServiceOutcome q = rc::outcome_enqueued();
  assert(q.ok && q.message == "OK queued" && q.code == 0);
  const rc::ServiceOutcome e =
      rc::outcome_error({StatusCode::kInvalidArgument}, "bad arguments");
  assert(!e.ok && e.message == "ERR invalid_argument bad arguments" && e.code == 1);
  const rc::ServiceOutcome n =
      rc::outcome_error({StatusCode::kNotImplemented}, "table/lua need bulk payloads");
  assert(!n.ok && n.code == 3);
  const rc::ServiceOutcome i = rc::outcome_error({StatusCode::kInternal}, "service failed");
  assert(!i.ok && i.code == 4);
}

void test_census_matches_bus_list() {
  // The no-drift lock: the cache builder and the bus LIST apply answer
  // byte-identically.
  wengine::world::State st = make_two_source_state();
  wengine::solvers::ControlBus bus(7);
  const std::uint32_t ch = bus.add_channel();
  assert(bus.enqueue(ch, "LIST"));
  const wengine::solvers::DrainStats ds = bus.drain(st, 0);
  assert(ds.readonly == 1);
  const std::vector<std::string> resps = bus.take_responses(ch);
  rc::CensusCache cache;
  rc::refresh_census(st, 0, cache);
  assert(cache.step == 0 && cache.sim_time == st.sim_time);
  assert(cache.lines == resps);
  assert(!rc::census_text(cache).empty());
  // Text starts with the protocol verdict line.
  assert(rc::census_text(cache).rfind("OK list 2\nSRC", 0) == 0);
}

void test_step_boundary_add_set_stop_start_drop() {
  wengine::world::State st = make_two_source_state();
  wengine::core::Runner run({.dt = 0.01, .emit_every = 1});
  auto solver = wengine::solvers::SignalSolver::create();
  run.add_solver(*solver);
  // Drive the runner's own state: move the two sources in.
  run.state() = st;
  wengine::solvers::ControlBus bus(7);
  const std::uint32_t ch = bus.add_channel();

  // Enqueue-only: nothing mutates at service time.
  std::string line;
  assert(rc::validate_service_line(rc::kServiceAdd, "sine U FREQ 2 AMP 1", line).ok());
  assert(bus.enqueue(ch, line));
  assert(run.state().signals.live_count == 2);

  // Driver: drain at the boundary in slot order, then step exactly once.
  wengine::solvers::DrainStats ds = bus.drain(run.state(), 0);
  assert(ds.applied == 1);
  assert(run.state().signals.live_count == 3);
  assert(bus.take_responses(ch).at(0) == "OK added 2");
  assert(run.run(1).ok());

  // Unknown serials pass enqueue (shape-valid) and fail loudly at drain.
  assert(rc::validate_service_line(rc::kServiceSet, "99 AMP 5", line).ok());
  assert(bus.enqueue(ch, line));
  ds = bus.drain(run.state(), 1);
  assert(ds.rejected == 1);
  const std::vector<std::string> r1 = bus.take_responses(ch);
  assert(r1.size() == 1 && r1[0].rfind("ERR invalid_argument", 0) == 0);

  // Retune live: SET applies at the next boundary.
  assert(rc::validate_service_line(rc::kServiceSet, "2 AMP 9", line).ok());
  assert(bus.enqueue(ch, line));
  ds = bus.drain(run.state(), 1);
  assert(ds.applied == 1);
  assert(bus.take_responses(ch).at(0) == "OK set 2 AMP");
  const auto slot = wengine::world::find_serial(run.state().signals, 2);
  assert(slot.has_value());
  assert(run.state().signals.amp[*slot] == 9.0);

  // Freeze + resume are phase-exact and idempotent-safe.
  assert(rc::validate_service_line(rc::kServiceStop, "2", line).ok());
  assert(bus.enqueue(ch, line));
  assert(bus.drain(run.state(), 1).applied == 1);
  assert(bus.take_responses(ch).at(0) == "OK stopped 2");
  assert(run.state().signals.running[*slot] == 0);
  assert(rc::validate_service_line(rc::kServiceStart, "2", line).ok());
  assert(bus.enqueue(ch, line));
  assert(bus.drain(run.state(), 1).applied == 1);
  assert(bus.take_responses(ch).at(0) == "OK started 2");
  assert(run.state().signals.running[*slot] != 0);

  // Drop is final: the serial never comes back.
  assert(rc::validate_service_line(rc::kServiceDrop, "2", line).ok());
  assert(bus.enqueue(ch, line));
  assert(bus.drain(run.state(), 1).applied == 1);
  assert(bus.take_responses(ch).at(0) == "OK dropped 2");
  assert(!wengine::world::find_serial(run.state().signals, 2).has_value());
  assert(rc::validate_service_line(rc::kServiceAdd, "dc U OFFSET 1", line).ok());
  assert(bus.enqueue(ch, line));
  assert(bus.drain(run.state(), 1).applied == 1);
  assert(bus.take_responses(ch).at(0) == "OK added 3");
}

} // namespace

int main() {
  test_service_names_and_verbs();
  test_request_lines();
  test_validate_shapes();
  test_outcomes();
  test_census_matches_bus_list();
  test_step_boundary_add_set_stop_start_drop();
  std::printf("test_ros2_control: OK\n");
  return 0;
}
