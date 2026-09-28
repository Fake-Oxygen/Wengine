// SIG-10 generic control plane: TCP socket integration tests.
//
// Real loopback clients against link::ControlServer glued to
// solvers::ControlBus exactly like `sig run --control-tcp`: version
// handshake, mid-run set/start/stop over the wire, malformed + overlong
// rejection, two-client conflict resolution (channel order, not arrival
// timing), LIST census, disconnect survival, and a CLI smoke run of
// `sig run --control-tcp` (flag parse, step-synchronous drive, summary).

#include "wengine/cli/args.hpp"
#include "wengine/cli/commands.hpp"
#include "wengine/core/runner.hpp"
#include "wengine/link/control_server.hpp"
#include "wengine/solvers/signal.hpp"
#include "wengine/solvers/signal_control.hpp"

#include <arpa/inet.h>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <netinet/in.h>
#include <sched.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>

using namespace wengine;

namespace {

constexpr bool near(double a, double b, double eps = 1e-12) {
  return std::fabs(a - b) <= eps * (1.0 + std::fabs(b));
}

// First free loopback port in a test range (bind success is ownership).
std::uint16_t pick_port() {
  for (int p = 19001; p < 19200; ++p) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<std::uint16_t>(p));
    const int rc = ::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    ::close(fd);
    if (rc == 0) {
      return static_cast<std::uint16_t>(p);
    }
  }
  assert(false && "no free test port");
  return 0;
}

int connect_client(std::uint16_t port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  assert(fd >= 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  const int rc =
      ::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
  assert(rc == 0);
  timeval tv{};
  tv.tv_sec = 5;
  tv.tv_usec = 0;
  (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  return fd;
}

void send_all(int fd, const std::string &s) {
  std::size_t done = 0;
  while (done < s.size()) {
    const ssize_t n = ::send(fd, s.data() + done, s.size() - done, MSG_NOSIGNAL);
    assert(n > 0);
    done += static_cast<std::size_t>(n);
  }
}

// Blocking read of one \n-terminated line (CR stripped, no newline kept).
std::string recv_line(int fd) {
  std::string out;
  char c = 0;
  for (;;) {
    const ssize_t n = ::recv(fd, &c, 1, 0);
    assert(n == 1 && "client recv timeout or disconnect");
    if (c == '\n') {
      break;
    }
    out.push_back(c);
  }
  if (!out.empty() && out.back() == '\r') {
    out.pop_back();
  }
  return out;
}

// Driver-side glue (mirrors sig run --control-tcp): poll, enqueue slot i
// as bus channel 1+i, drain at the step boundary, collect, flush.
struct Harness {
  link::ControlServer server;
  solvers::ControlBus bus;
  core::Runner runner{{.dt = 0.01}};
  solvers::SignalSolver solver;
  std::vector<link::ControlSlotLines> polled;

  explicit Harness(std::uint16_t port)
      : server("127.0.0.1:" + std::to_string(port)), bus(server.run_id()) {
    assert(server.ok());
    runner.add_solver(solver);
    bus.add_channel(); // 0: local
    for (std::uint32_t i = 0; i < server.max_clients(); ++i) {
      bus.add_channel(); // 1+i: TCP slot i
    }
  }

  // Poll until at least `want` new lines arrived (loopback: immediate).
  // Always polls at least once (transport-level verdicts like the
  // overlong-line ERR are produced by polling, not by bus lines).
  void poll_until(std::size_t want) {
    std::size_t seen = 0;
    for (int i = 0; i < 20000; ++i) {
      server.poll(polled);
      for (auto &sl : polled) {
        for (auto &ln : sl.lines) {
          const bool ok = bus.enqueue(1 + sl.slot, std::move(ln));
          assert(ok);
          ++seen;
        }
      }
      if (seen >= want) {
        break;
      }
      ::sched_yield();
    }
    assert(seen >= want);
  }

  solvers::DrainStats drain() {
    solvers::DrainStats st = bus.drain(runner.state(), runner.state().step_index);
    for (std::uint32_t c = 1; c < bus.channel_count(); ++c) {
      for (auto &r : bus.take_responses(c)) {
        server.push_response(c - 1, r);
      }
    }
    server.flush();
    return st;
  }

  void step(std::uint64_t n = 1) {
    const core::Status st = runner.run(n);
    assert(st.ok());
  }
};

void test_handshake_and_midrun_set() {
  const std::uint16_t port = pick_port();
  Harness h(port);
  const int cli = connect_client(port);
  // HELLO with a wrong version is refused loudly, never downgraded.
  send_all(cli, "HELLO 2 zephyr\n");
  h.poll_until(1);
  h.drain();
  assert(recv_line(cli).substr(0, 20) == "ERR invalid_argument");
  send_all(cli, "HELLO 1 zephyr\n");
  h.poll_until(1);
  h.drain();
  const std::string hello = recv_line(cli);
  assert(hello == "OK hello 1 run=" + std::to_string(h.server.run_id()));
  send_all(cli, "PING\n");
  h.poll_until(1);
  h.drain();
  assert(recv_line(cli) == "OK pong");
  // ADD a 1 Hz sine, step 10, then SET FREQ 2 over the wire: one step
  // later the sample is the 2 Hz closed form at t=0.11 (time base kept).
  send_all(cli, "ADD sine U FREQ 1 AMP 2\n");
  h.poll_until(1);
  h.drain();
  assert(recv_line(cli) == "OK added 0");
  h.step(10);
  send_all(cli, "SET 0 FREQ 2\n");
  h.poll_until(1);
  h.drain();
  assert(recv_line(cli) == "OK set 0 FREQ");
  h.step(1);
  const auto slot = world::find_serial(h.runner.state().signals, 0);
  assert(slot.has_value());
  const double want = 2.0 * std::sin(2.0 * 3.141592653589793 * 2.0 * 0.11);
  assert(near(h.runner.state().signals.value[*slot], want));
  ::close(cli);
}

void test_malformed_overlong_and_stop_start() {
  const std::uint16_t port = pick_port();
  Harness h(port);
  const int cli = connect_client(port);
  send_all(cli, "ADD sine U FREQ 1\n");
  h.poll_until(1);
  h.drain();
  assert(recv_line(cli) == "OK added 0");
  send_all(cli, "BOGUS VERB\n");
  h.poll_until(1);
  h.drain();
  assert(recv_line(cli).substr(0, 3) == "ERR");
  // Overlong line: refused at the transport, never reaches the bus.
  const std::uint64_t rejected_before = h.bus.rejected();
  send_all(cli, std::string(5000, 'x') + "\n");
  h.poll_until(0); // no bus line expected; still flush the transport ERR
  h.drain();
  assert(recv_line(cli) == "ERR invalid_argument line too long");
  assert(h.server.stats().overlong_dropped == 1);
  assert(h.bus.rejected() == rejected_before); // transport refusal, bus untouched
  // STOP freezes over the wire, START resumes.
  h.step(5);
  const auto slot = world::find_serial(h.runner.state().signals, 0);
  assert(slot.has_value());
  send_all(cli, "STOP 0\n");
  h.poll_until(1);
  h.drain();
  assert(recv_line(cli) == "OK stopped 0");
  const double frozen = h.runner.state().signals.value[*slot];
  h.step(4);
  assert(h.runner.state().signals.value[*slot] == frozen);
  send_all(cli, "START 0\n");
  h.poll_until(1);
  h.drain();
  assert(recv_line(cli) == "OK started 0");
  h.step(1);
  ::close(cli);
}

void test_two_client_conflict_and_list() {
  const std::uint16_t port = pick_port();
  Harness h(port);
  const int a = connect_client(port);
  const int b = connect_client(port);
  send_all(a, "ADD sine U FREQ 1 AMP 1\n");
  h.poll_until(1);
  h.drain();
  assert(recv_line(a) == "OK added 0");
  // Both clients SET AMP with no drain between: fixed channel order wins
  // (slot A -> channel 1 before slot B -> channel 2), even though B's
  // bytes arrive second here — then again with arrivals reversed.
  send_all(a, "SET 0 AMP 1\n");
  send_all(b, "SET 0 AMP 2\n");
  h.poll_until(2);
  const solvers::DrainStats st = h.drain();
  assert(st.applied == 2);
  assert(recv_line(a) == "OK set 0 AMP");
  assert(recv_line(b) == "OK set 0 AMP");
  assert(h.runner.state().signals.amp[0] == 2.0);
  send_all(b, "SET 0 AMP 4\n");
  send_all(a, "SET 0 AMP 3\n");
  h.poll_until(2);
  h.drain();
  assert(recv_line(b) == "OK set 0 AMP");
  assert(recv_line(a) == "OK set 0 AMP");
  assert(h.runner.state().signals.amp[0] == 4.0); // channel order, not timing
  // LIST census over the wire: count line + one SRC line.
  send_all(a, "LIST\n");
  h.poll_until(1);
  h.drain();
  assert(recv_line(a) == "OK list 1");
  const std::string src = recv_line(a);
  assert(src.substr(0, 12) == "SRC 0 sine U");
  // Disconnect survival: closing A keeps B (and the server) working.
  ::close(a);
  send_all(b, "PING\n");
  h.poll_until(1);
  h.drain();
  assert(recv_line(b) == "OK pong");
  assert(h.server.stats().accepted == 2);
  ::close(b);
}

void test_cli_smoke() {
  using wengine::cli::ArgCursor;
  const std::uint16_t port = pick_port();
  const std::string ep = "127.0.0.1:" + std::to_string(port);
  {
    // --control-tcp takes a value: the span covers the 9 used slots.
    std::array<std::string_view, 10> full{"run", "--sig-kind", "sine", "--steps", "3",
                                          "--dt", "0.01",     "--control-tcp", ep, "x"};
    std::span<const std::string_view> args{full.data(), 9};
    ArgCursor cur{args};
    std::ostringstream out;
    const int rc = cli::commands::SigSpec::run(cur, out);
    assert(rc == 0);
    const std::string text = out.str();
    assert(text.find("control: " + ep + " (applied=0 readonly=0 rejected=0") !=
           std::string::npos);
  }
  {
    std::array<std::string_view, 3> t{"run", "--control-tcp", "nope"};
    std::span<const std::string_view> args{t.data(), t.size()};
    ArgCursor cur{args};
    std::ostringstream out;
    const int rc = cli::commands::SigSpec::run(cur, out);
    assert(rc == 2); // usage error on bad endpoint shape
  }
}

} // namespace

int main() {
  test_handshake_and_midrun_set();
  test_malformed_overlong_and_stop_start();
  test_two_client_conflict_and_list();
  test_cli_smoke();
  std::cout << "test_control_server: OK\n";
  return 0;
}
