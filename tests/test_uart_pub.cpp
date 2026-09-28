// SIG-8 UART publisher + control tests over real pty pairs.
//
// No hardware needed: posix_openpt gives a master fd (test peer) and a
// /dev/pts/N slave (the publisher/control side, configured via termios
// exactly like /dev/ttyUSB0). Covers ASCII/COBS stream exactness, UART
// control round-trips through solvers::ControlBus, garbage resync,
// flood drops without stalls, a decimated soak via the real CLI, and
// CLI usage errors.

// posix_openpt et al. need X/Open visibility under strict -std=c++20.
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include "wengine/cli/args.hpp"
#include "wengine/cli/commands.hpp"
#include "wengine/link/uart_codec.hpp"
#include "wengine/link/uart_publisher.hpp"
#include "wengine/signal/types.hpp"
#include "wengine/solvers/signal_control.hpp"
#include "wengine/world/state.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <span>
#include <sstream>
#include <string>
#include <sys/select.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

using namespace wengine;

namespace {

constexpr bool near(double a, double b, double eps = 1e-12) {
  return std::fabs(a - b) <= eps * (1.0 + std::fabs(b));
}

// Open pty pair: master for the test peer, slave path for the publisher.
struct Pty {
  int master = -1;
  std::string slave;
  Pty() = default;
  Pty(const Pty &) = delete;
  Pty &operator=(const Pty &) = delete;
  Pty(Pty &&o) noexcept : master(o.master), slave(std::move(o.slave)) {
    o.master = -1;
  }
  ~Pty() {
    if (master >= 0) {
      ::close(master);
    }
  }
};

Pty open_pty() {
  Pty p;
  p.master = ::posix_openpt(O_RDWR | O_NOCTTY);
  assert(p.master >= 0);
  assert(::grantpt(p.master) == 0 && ::unlockpt(p.master) == 0);
  const char *name = ::ptsname(p.master);
  assert(name != nullptr);
  p.slave = name;
  return p;
}

// Read everything available within the deadline (empty when idle).
std::string read_available(int fd, int timeout_ms = 2000) {
  std::string out;
  for (;;) {
    fd_set rfds{};
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    const int rc = ::select(fd + 1, &rfds, nullptr, nullptr, &tv);
    if (rc <= 0) {
      break;
    }
    char buf[4096];
    const ssize_t n = ::read(fd, buf, sizeof(buf));
    if (n <= 0) {
      break;
    }
    out.append(buf, static_cast<std::size_t>(n));
    timeout_ms = 100; // drain burst, then short tail wait
  }
  return out;
}

void write_all(int fd, const std::string &s) {
  std::size_t done = 0;
  while (done < s.size()) {
    const ssize_t n = ::write(fd, s.data() + done, s.size() - done);
    assert(n > 0);
    done += static_cast<std::size_t>(n);
  }
}

world::Snapshot one_signal(std::uint64_t step, double time, std::uint64_t serial,
                           double value) {
  world::Snapshot s;
  s.sim_time = time;
  s.step_index = step;
  s.sig_serial = {serial};
  s.sig_value = {value};
  s.sig_quantity = {static_cast<std::uint8_t>(signal::QuantityId::kVoltage)};
  return s;
}

void test_ascii_exact() {
  Pty pty = open_pty();
  link::uart::UartPort port(pty.slave, 115200);
  assert(port.ok());
  link::uart::UartPublisher pub(port, link::uart::UartMode::kAscii);
  assert(pub.ok());
  pub.on_snapshot(one_signal(3, 0.5, 4, 1.25));
  assert(pub.stats().attempted == 1 && pub.stats().published == 1);
  assert(pub.lines_written() == 1 && pub.lines_dropped() == 0);
  const std::string got = read_available(pty.master);
  assert(got == "3,0.5,4,1.25,U\n");
  // Two rows in one emission: two lines, slot order.
  world::Snapshot s = one_signal(9, 1.5, 4, -0.5);
  s.sig_serial.push_back(7);
  s.sig_value.push_back(2.0);
  s.sig_quantity.push_back(static_cast<std::uint8_t>(signal::QuantityId::kCurrent));
  pub.on_snapshot(s);
  const std::string got2 = read_available(pty.master);
  assert(got2 == "9,1.5,4,-0.5,U\n9,1.5,7,2,I\n");
  // Empty signals: zero bytes, still a published tick.
  world::Snapshot empty;
  empty.sim_time = 2.0;
  empty.step_index = 10;
  pub.on_snapshot(empty);
  assert(pub.stats().published == 3);
  assert(read_available(pty.master, 200).empty());
}

void test_cobs_exact() {
  using link::uart::cobs_decode;
  Pty pty = open_pty();
  link::uart::UartPort port(pty.slave, 115200);
  assert(port.ok());
  link::uart::UartPublisher pub(port, link::uart::UartMode::kCobs);
  pub.on_snapshot(one_signal(3, 0.5, 4, 1.25));
  const std::string wire = read_available(pty.master);
  assert(!wire.empty() && wire.back() == '\0');
  // Exactly one frame: decode, check the 'S' type + line bytes.
  const auto d =
      cobs_decode(std::string_view(wire.data(), wire.size() - 1));
  assert(d.ok() && d.value == "S3,0.5,4,1.25,U");
}

void test_control_ascii_roundtrip() {
  Pty pty = open_pty();
  link::uart::UartPort port(pty.slave, 115200);
  assert(port.ok());
  link::uart::UartControl uctl(port, link::uart::UartMode::kAscii);
  assert(uctl.ok());
  // Live source serial 0 in the pool behind the bus.
  world::State state;
  {
    const world::Entity e = state.spawn_entity();
    signal::SignalParams p;
    assert(world::configure_source(e, state.signals, p, 0.0).ok());
  }
  solvers::ControlBus bus(uctl.run_id());
  const std::uint32_t ch = bus.add_channel();
  // Peer sends a command, garbage, then a set-param: one malformed unit,
  // two commands, newest drained at the step boundary like TCP control.
  write_all(pty.master, "!PING\nGARBAGE\n!SET 0 AMP 2\n");
  std::vector<std::string> lines;
  uctl.poll_lines(lines); // single poll suffices on loopback
  for (auto &ln : lines) {
    assert(bus.enqueue(ch, std::move(ln)));
  }
  assert(lines.size() == 2 && uctl.malformed() == 1);
  const solvers::DrainStats st = bus.drain(state, state.step_index);
  assert(st.applied == 1 && st.readonly == 1 && st.rejected == 0);
  for (auto &r : bus.take_responses(ch)) {
    uctl.push_response(r);
  }
  assert(uctl.flush());
  const std::string got = read_available(pty.master);
  assert(got == "!OK pong\n!OK set 0 AMP\n");
  assert(state.signals.amp[0] == 2.0);
  assert(uctl.commands_rx() == 2 && uctl.responses_tx() == 2);
}

void test_control_cobs_roundtrip_and_resync() {
  using link::uart::cobs_decode;
  using link::uart::cobs_encode;
  Pty pty = open_pty();
  link::uart::UartPort port(pty.slave, 115200);
  assert(port.ok());
  link::uart::UartControl uctl(port, link::uart::UartMode::kCobs);
  world::State state;
  {
    const world::Entity e = state.spawn_entity();
    signal::SignalParams p;
    assert(world::configure_source(e, state.signals, p, 0.0).ok());
  }
  solvers::ControlBus bus(uctl.run_id());
  const std::uint32_t ch = bus.add_channel();
  auto qframe = [](const std::string &line) {
    std::string f;
    assert(cobs_encode("Q" + line, f).ok());
    f.push_back('\0');
    return f;
  };
  // Garbage bytes ending at a delimiter form one bad unit; the next
  // frame then decodes cleanly (resync). Delimiter-free garbage would
  // merge into the following unit instead — one malformed either way.
  write_all(pty.master, std::string("zz\xFF", 3) + std::string("\x00", 1));
  write_all(pty.master, qframe("STOP 0"));
  std::vector<std::string> lines;
  for (int k = 0; k < 50 && lines.empty(); ++k) {
    uctl.poll_lines(lines);
    for (auto &ln : lines) {
      assert(bus.enqueue(ch, std::move(ln)));
    }
  }
  assert(lines.size() == 1 && uctl.malformed() == 1);
  bus.drain(state, state.step_index);
  for (auto &r : bus.take_responses(ch)) {
    uctl.push_response(r);
  }
  assert(uctl.flush());
  assert(state.signals.running[0] == 0);
  // Response arrives as an 'R' COBS frame; decode and check the line.
  const std::string wire = read_available(pty.master);
  assert(!wire.empty() && wire.back() == '\0');
  const auto d = cobs_decode(std::string_view(wire.data(), wire.size() - 1));
  assert(d.ok() && d.value == "ROK stopped 0");
}

void test_flood_drops_without_stall() {
  Pty pty = open_pty();
  link::uart::UartPort port(pty.slave, 115200);
  assert(port.ok());
  link::uart::UartPublisher pub(port, link::uart::UartMode::kAscii);
  // Nobody drains the master: 64 KiB of pty buffer must overflow loudly
  // while on_snapshot stays non-blocking and the publisher stays healthy.
  const world::Snapshot s = one_signal(1, 0.01, 0, 1.0);
  for (int k = 0; k < 30000; ++k) {
    pub.on_snapshot(s);
  }
  assert(pub.ok());
  assert(pub.lines_dropped() > 0);
  assert(pub.stats().attempted ==
         pub.stats().published + pub.stats().dropped_send +
             pub.stats().disabled_after_error);
  // Drain to a clean fd for the next test using this pty (none do).
  (void)read_available(pty.master, 100);
}

void test_cli_soak_and_summary() {
  using wengine::cli::ArgCursor;
  Pty pty = open_pty();
  // 2000 steps at dt=10us, every 100th emitted: 20 lines over the wire,
  // each sample-exact against the 1 Hz sine closed form.
  std::array<std::string_view, 12> t{"run", "--sig-kind", "sine", "--steps", "2000",
                                     "--dt", "10us", "--emit-every", "100", "--emit-uart",
                                     std::string_view(pty.slave.data(), pty.slave.size()), "x"};
  std::span<const std::string_view> args{t.data(), 11};
  ArgCursor cur{args};
  std::ostringstream out;
  const int rc = cli::commands::SigSpec::run(cur, out);
  assert(rc == 0);
  const std::string text = out.str();
  assert(text.find("uart: " + pty.slave + " (20 frames, 20 lines, 0 lines dropped") !=
         std::string::npos);
  const std::string wire = read_available(pty.master);
  std::vector<std::string> lines;
  std::size_t begin = 0;
  for (std::size_t k = 0; k <= wire.size(); ++k) {
    if (k == wire.size() || wire[k] == '\n') {
      if (k > begin) {
        lines.push_back(wire.substr(begin, k - begin));
      }
      begin = k + 1;
    }
  }
  assert(lines.size() == 20);
  for (std::size_t k = 0; k < lines.size(); ++k) {
    link::uart::StreamSample s;
    assert(link::uart::parse_stream_line(lines[k], s).ok());
    const std::uint64_t want_step = (k + 1) * 100;
    assert(s.step == want_step && s.sig_id == 0 && s.quantity == "U");
    const double t = static_cast<double>(want_step) * 0.00001;
    const double want = 1.0 * std::sin(2.0 * 3.141592653589793 * 1.0 * t);
    // near(), not ==: the runner accumulates sim_time by repeated += dt
    // while the reference multiplies once (1-ulp class differences are
    // possible); bit-exactness of the wire itself is proven in
    // test_uart_codec.
    assert(near(s.time, t) && near(s.value, want));
  }
}

void test_cli_control_uart_smoke() {
  using wengine::cli::ArgCursor;
  Pty pty = open_pty();
  // No peer traffic: deterministic summary with zero commands applied.
  std::array<std::string_view, 10> t{"run", "--sig-kind", "sine", "--steps", "5",
                                     "--emit-uart",
                                     std::string_view(pty.slave.data(), pty.slave.size()),
                                     "--control-uart", "--emit-uart-mode", "cobs"};
  std::span<const std::string_view> args{t.data(), t.size()};
  ArgCursor cur{args};
  std::ostringstream out;
  const int rc = cli::commands::SigSpec::run(cur, out);
  assert(rc == 0);
  const std::string text = out.str();
  assert(text.find("control-uart: " + pty.slave + " (malformed=0 commands=0") !=
         std::string::npos);
  // COBS stream frames still arrived (5 'S' frames).
  const std::string wire = read_available(pty.master);
  std::size_t frames = 0;
  for (char c : wire) {
    frames += (c == '\0') ? 1 : 0;
  }
  assert(frames == 5);
}

void test_cli_usage_errors() {
  using wengine::cli::ArgCursor;
  auto run = [](std::vector<std::string_view> v) {
    ArgCursor cur{std::span<const std::string_view>(v.data(), v.size())};
    std::ostringstream out;
    const int rc = cli::commands::SigSpec::run(cur, out);
    return rc;
  };
  auto sim = [](std::vector<std::string_view> v) {
    ArgCursor cur{std::span<const std::string_view>(v.data(), v.size())};
    std::ostringstream out;
    const int rc = cli::commands::SimSpec::run(cur, out);
    return rc;
  };
  assert(run({"run", "--control-uart"}) == 2); // needs --emit-uart
  assert(run({"run", "--emit-uart-mode", "cobs"}) == 2); // needs --emit-uart
  assert(run({"run", "--emit-uart", "/dev/pts/99", "--emit-uart-mode", "binary"}) == 2);
  assert(run({"run", "--emit-uart", "/dev/ttyUSB0,12345"}) == 2); // bad baud
  assert(run({"run", "--emit-uart", "/nonexistent-tty-xyz"}) == 1); // open fails
  assert(sim({"run", "--emit-uart", "/dev/pts/99"}) == 2); // projectile: not signal
  assert(sim({"run", "--solver", "signal", "--emit-uart-select", "7"}) == 2); // serial 7
}

} // namespace

int main() {
  test_ascii_exact();
  test_cobs_exact();
  test_control_ascii_roundtrip();
  test_control_cobs_roundtrip_and_resync();
  test_flood_drops_without_stall();
  test_cli_soak_and_summary();
  test_cli_control_uart_smoke();
  test_cli_usage_errors();
  std::cout << "test_uart_pub: OK\n";
  return 0;
}
