// Loopback throughput benchmark for the SIG-7 TCP/UDP publishers. Uses
// runtime-varying snapshot values and an active reader so compiler folding
// and receiver-buffer saturation do not dominate the measurement.

#include "wengine/link/socket_publisher.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

std::uint32_t xorshift(std::uint32_t &s) noexcept {
  s ^= s << 13;
  s ^= s >> 17;
  s ^= s << 5;
  return s;
}

bool recv_exact(int fd, unsigned char *buf, std::size_t len) {
  std::size_t got = 0;
  while (got < len) {
    const ssize_t n = ::recv(fd, buf + got, len - got, 0);
    if (n <= 0) {
      return false;
    }
    got += static_cast<std::size_t>(n);
  }
  return true;
}

int make_tcp_listener(std::uint16_t &port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 ||
      ::listen(fd, 1) != 0) {
    ::close(fd);
    return -1;
  }
  socklen_t len = sizeof(addr);
  if (::getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &len) != 0) {
    ::close(fd);
    return -1;
  }
  port = ntohs(addr.sin_port);
  return fd;
}

int make_udp_receiver(std::uint16_t &port) {
  const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    return -1;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return -1;
  }
  socklen_t len = sizeof(addr);
  if (::getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &len) != 0) {
    ::close(fd);
    return -1;
  }
  timeval timeout{};
  timeout.tv_sec = 0;
  timeout.tv_usec = 100000;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  port = ntohs(addr.sin_port);
  return fd;
}

} // namespace

int main(int argc, char **argv) {
  const std::string mode = argc > 1 ? argv[1] : "tcp";
  const std::uint64_t frames = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 2000;
  const std::size_t item_count = argc > 3 ?
      static_cast<std::size_t>(std::strtoull(argv[3], nullptr, 10)) : 64;
  if ((mode != "tcp" && mode != "udp") || frames == 0 || item_count == 0 ||
      item_count > wengine::link::kMaxItems) {
    std::cout << "usage: bench_link_sockets [tcp|udp] [frames] [items]\n";
    return 2;
  }

  wengine::world::Snapshot snapshot;
  snapshot.pos_x.resize(item_count);
  snapshot.pos_y.resize(item_count);
  snapshot.vel_x.resize(item_count);
  snapshot.vel_y.resize(item_count);
  std::uint32_t rng = 0x9E3779B9u ^ static_cast<std::uint32_t>(frames);
  for (std::size_t i = 0; i < item_count; ++i) {
    snapshot.pos_x[i] = static_cast<double>(xorshift(rng)) / 4294967296.0;
    snapshot.pos_y[i] = static_cast<double>(xorshift(rng)) / 4294967296.0;
    snapshot.vel_x[i] = static_cast<double>(xorshift(rng)) / 4294967296.0;
    snapshot.vel_y[i] = static_cast<double>(xorshift(rng)) / 4294967296.0;
  }

  std::uint16_t port = 0;
  const int peer_fd = mode == "tcp" ? make_tcp_listener(port) : make_udp_receiver(port);
  if (peer_fd < 0) {
    std::cout << "bench_link_sockets: cannot create loopback receiver\n";
    return 1;
  }
  const std::string endpoint = "127.0.0.1:" + std::to_string(port);

  std::atomic<std::uint64_t> received_bytes{0};
  std::atomic<std::uint64_t> received_frames{0};
  std::atomic<std::uint64_t> received_missed{0};
  std::thread reader;
  if (mode == "tcp") {
    reader = std::thread([&] {
      const int peer = ::accept(peer_fd, nullptr, nullptr);
      if (peer < 0) {
        return;
      }
      unsigned char hello_bytes[sizeof(wengine::link::TcpHello)]{};
      if (recv_exact(peer, hello_bytes, sizeof(hello_bytes))) {
        wengine::link::TcpHello hello{};
        std::memcpy(&hello, hello_bytes, sizeof(hello));
        if (wengine::link::check_tcp_hello(hello, "bench").ok()) {
          std::array<unsigned char, 65536> buf{};
          while (true) {
            const ssize_t n = ::recv(peer, buf.data(), buf.size(), 0);
            if (n <= 0) {
              break;
            }
            received_bytes.fetch_add(static_cast<std::uint64_t>(n),
                                     std::memory_order_relaxed);
          }
        }
      }
      ::close(peer);
    });
  } else {
    reader = std::thread([&] {
      std::array<unsigned char, wengine::link::kMaxUdpPayload> buf{};
      wengine::link::LagStats lag;
      while (true) {
        const ssize_t n = ::recvfrom(peer_fd, buf.data(), buf.size(), 0, nullptr, nullptr);
        if (n <= 0) {
          break;
        }
        wengine::link::Frame frame;
        std::uint64_t seq = 0;
        if (wengine::link::decode_udp(buf.data(), static_cast<std::size_t>(n), "bench",
                                      frame, seq)
                .ok()) {
          (void)wengine::link::lag_observe(lag, seq);
        }
        received_bytes.fetch_add(static_cast<std::uint64_t>(n),
                                 std::memory_order_relaxed);
        received_frames.fetch_add(1, std::memory_order_relaxed);
      }
      received_missed.store(lag.missed, std::memory_order_relaxed);
    });
  }

  wengine::link::PublisherStats stats{};
  bool pending = false;
  const auto start = std::chrono::steady_clock::now();
  if (mode == "tcp") {
    wengine::link::TcpPublisher publisher(endpoint, "bench");
    if (!publisher.ok()) {
      std::cout << "bench_link_sockets: TCP connect failed\n";
      ::close(peer_fd);
      reader.join();
      return 1;
    }
    for (std::uint64_t i = 1; i <= frames; ++i) {
      snapshot.step_index = i;
      snapshot.sim_time = static_cast<double>(i) * 0.001;
      snapshot.pos_x[0] += static_cast<double>(xorshift(rng) & 0xFFFFu) * 1e-9;
      publisher.on_snapshot(snapshot);
    }
    stats = publisher.stats();
    pending = publisher.has_pending_frame();
  } else {
    wengine::link::UdpPublisher publisher(endpoint, "bench");
    if (!publisher.ok()) {
      std::cout << "bench_link_sockets: UDP connect failed\n";
      ::close(peer_fd);
      reader.join();
      return 1;
    }
    for (std::uint64_t i = 1; i <= frames; ++i) {
      snapshot.step_index = i;
      snapshot.sim_time = static_cast<double>(i) * 0.001;
      snapshot.pos_x[0] += static_cast<double>(xorshift(rng) & 0xFFFFu) * 1e-9;
      publisher.on_snapshot(snapshot);
    }
    stats = publisher.stats();
  }
  const auto stop = std::chrono::steady_clock::now();
  const double seconds = std::chrono::duration<double>(stop - start).count();
  ::close(peer_fd);
  reader.join();
  if (mode == "tcp") {
    received_frames.store(received_bytes.load() / wengine::link::kTcpFrameBytes,
                          std::memory_order_relaxed);
  }

  const double frames_per_second = static_cast<double>(frames) / seconds;
  std::cout << "bench_link_sockets " << mode << ": " << frames << " frames x "
            << item_count << " items, " << frames_per_second << " frames/s, "
            << static_cast<double>(received_bytes.load()) / seconds / 1e6
            << " received MB/s; sent=" << stats.published
            << " dropped=" << stats.dropped_send + stats.dropped_over_bound
            << " pending=" << pending
            << " received=" << received_frames.load();
  if (mode == "udp") {
    std::cout << " receiver_missed=" << received_missed.load();
  }
  std::cout << " bytes=" << received_bytes.load() << '\n';
  return 0;
}
