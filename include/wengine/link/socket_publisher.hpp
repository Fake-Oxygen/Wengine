#pragma once

// TCP/UDP socket publishers: networked endpoints of the wengine-link
// snapshot bus (SIG-7, first networked consumer: Zephyr over IP).
//
// Two emit::ISink publishers, built and owned by the caller (CLI/GUI edge)
// and attached like any sink — same fan-out seam as CsvSink/ShmPublisher,
// no engine change. Both reuse link::encode_frame bytes and the SHM
// versioning wording; both are non-blocking copy-and-return with sticky
// core::Status, so a slow or absent peer never stalls the sim loop.
//
// TCP (TcpPublisher): active connect to host:port, then a hello
// (TcpHello: magic "WLT1", version == link::kVersion, NUL-terminated
// domain) followed by length-prefixed LinkFrame bytes (u32 LE byte count
// + frame). The hello IS the attach gate, in SHM wording: the reader
// validates magic/version/domain with check_tcp_hello and closes on
// mismatch — loud refusal, never silent mid-stream. Queueing is the kernel
// send buffer (256 KiB default, ~3-4 full frames — tunable only by rebuild,
// documented in signals.md); on backpressure the attempted frame is
// dropped and counted (dropped_send) WITHOUT poisoning the publisher,
// except a partially accepted frame is completed before any later frame is
// sent (TCP streams cannot discard a prefix without corrupting framing),
// while over-bound frames (dropped_over_bound, kOutOfRange) and a dead
// peer (EPIPE/reset, kInternal) are sticky like SHM.
//
// A partially accepted frame is held as pending bytes (at most one frame).
// on_snapshot retries it first but never blocks; when emission is idle the
// application edge progresses it with service_pending() (non-blocking, no
// dummy snapshot) and drains it at shutdown with finish(timeout_ms)
// (bounded select-wait, same edge ownership as connect). The destructor
// never waits: undrained pending bytes are abandoned at close and must be
// reported by the finish caller via pending_bytes()/has_pending_frame().
//
// UDP (UdpPublisher): connected datagrams to host:port (no handshake —
// UDP has none). A full LinkFrame (68,136 B) always exceeds the maximum
// UDP payload (65,507 B), so full-frame datagrams are unimplementable;
// UDP instead sends a COMPACT datagram (UdpHeader + used entries only,
// ~100 B for a few signal channels) that self-describes per datagram
// (magic "WLU1", version, domain, frame_seq, clock, counts). Readers gate
// each datagram with decode_udp (magic/version/domain/counts/length all
// exact, else kInvalidArgument) and account loss with LagStats over
// frame_seq — same vocabulary as the SHM ring. For cross-transport
// consistency the UDP path is gated on encode_frame FIRST, so an
// over-bound frame is refused by every transport identically.
//
// PORTABILITY: POSIX-only TU (src/link/socket_publisher.cpp) — sockets,
// getaddrinfo, non-blocking I/O; the Windows port replaces that TU, never
// this header. Not copyable (owns fd); movable. Single-writer model: call
// on_snapshot from one runner thread only, like every Runner-driven sink.

#include "wengine/core/status.hpp"
#include "wengine/emit/sink.hpp"
#include "wengine/link/frame.hpp"
#include "wengine/link/protocol.hpp"
#include "wengine/world/state.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

namespace wengine::link {

// TCP stream framing: magic "WLT1" (LE bytes 'W','L','T','1").
inline constexpr std::uint32_t kTcpMagic = 0x31544C57u;
// UDP datagram framing: magic "WLU1" (LE bytes 'W','L','U','1").
inline constexpr std::uint32_t kUdpMagic = 0x31554C57u;
// Wire versions track the protocol version exactly: a link::kVersion bump
// invalidates both socket framings alongside the SHM gate.
inline constexpr std::uint32_t kTcpVersion = kVersion;
inline constexpr std::uint32_t kUdpVersion = kVersion;
// Kernel send buffer sized at construction (best-effort setsockopt): room
// for ~3-4 full TCP frames before backpressure drops begin.
inline constexpr int kSocketSendBuffer = 256 * 1024;
// Bounded shutdown drain for TcpPublisher::finish (application edge owns
// the deadline; destructors never wait).
inline constexpr int kTcpFinishTimeoutMs = 2000;
// Largest UDP payload per RFC 768 (65535 - 20 IP - 8 UDP).
inline constexpr std::size_t kMaxUdpPayload = 65507;
// TCP frame bytes on the wire: length prefix + one full LinkFrame.
inline constexpr std::size_t kTcpFrameBytes = 4 + sizeof(LinkFrame);

// TCP hello: first bytes of every connection (plain struct, LE on the
// wire; see hello_bytes/check_tcp_hello below).
struct TcpHello {
  std::uint32_t magic = kTcpMagic;
  std::uint32_t version = kTcpVersion;
  char domain[kDomainSize] = {}; // NUL-terminated solver/domain tag
};

static_assert(sizeof(TcpHello) == 4 + 4 + kDomainSize);

// Compact UDP datagram header (LE multibyte fields, doubles as IEEE-754
// bits — portable by construction, unlike the same-host LinkFrame).
// Followed by item_count LinkItems (64 B each) then channel_count
// LinkChannels (40 B each), using the SAME entry layouts as the wire
// LinkFrame so readers share field code.
struct UdpHeader {
  std::uint32_t magic = kUdpMagic;
  std::uint32_t version = kUdpVersion;
  char domain[kDomainSize] = {}; // NUL-terminated solver/domain tag
  std::uint64_t frame_seq = 0; // monotonic publish id (LagStats loss source)
  std::uint64_t step_index = 0;
  double sim_time = 0.0;
  std::uint64_t item_count = 0;
  std::uint64_t channel_count = 0;
};

static_assert(sizeof(UdpHeader) == 80);

namespace socket_detail {

inline void put_u32_le(unsigned char *dst, std::uint32_t v) noexcept {
  dst[0] = static_cast<unsigned char>(v & 0xFFu);
  dst[1] = static_cast<unsigned char>((v >> 8) & 0xFFu);
  dst[2] = static_cast<unsigned char>((v >> 16) & 0xFFu);
  dst[3] = static_cast<unsigned char>((v >> 24) & 0xFFu);
}

inline void put_u64_le(unsigned char *dst, std::uint64_t v) noexcept {
  for (int k = 0; k < 8; ++k) {
    dst[k] = static_cast<unsigned char>((v >> (8 * k)) & 0xFFu);
  }
}

inline std::uint32_t get_u32_le(const unsigned char *src) noexcept {
  return static_cast<std::uint32_t>(src[0]) |
         (static_cast<std::uint32_t>(src[1]) << 8) |
         (static_cast<std::uint32_t>(src[2]) << 16) |
         (static_cast<std::uint32_t>(src[3]) << 24);
}

inline std::uint64_t get_u64_le(const unsigned char *src) noexcept {
  std::uint64_t v = 0;
  for (int k = 0; k < 8; ++k) {
    v |= static_cast<std::uint64_t>(src[k]) << (8 * k);
  }
  return v;
}

} // namespace socket_detail

// Reader-side TCP hello gate (shared with future Zephyr readers): exact
// magic, exact version, exact domain, NUL-terminated domain. Anything else
// is kInvalidArgument — the "loud refusal" behind a reader close.
[[nodiscard]] inline core::Status check_tcp_hello(const TcpHello &hello,
                                                  std::string_view want_domain) noexcept {
  if (hello.magic != kTcpMagic || hello.version != kTcpVersion) {
    return {core::StatusCode::kInvalidArgument};
  }
  if (want_domain.empty() || want_domain.size() >= kDomainSize) {
    return {core::StatusCode::kInvalidArgument};
  }
  for (std::size_t i = 0; i < kDomainSize; ++i) {
    const char c = hello.domain[i];
    if (c == '\0') {
      return i == want_domain.size() &&
                     std::string_view(hello.domain, i) == want_domain
                 ? core::Status::success()
                 : core::Status{core::StatusCode::kInvalidArgument};
    }
  }
  return {core::StatusCode::kInvalidArgument}; // unterminated: corrupt
}

// Compact UDP encode (shared with future readers): header + used entries
// only. Validates everything BEFORE touching `out` (domain shape, counts,
// name bounds, total ≤ capacity and ≤ kMaxUdpPayload); violations are
// kOutOfRange/kInvalidArgument with `out`/`out_len` untouched — loud,
// never silently truncated.
[[nodiscard]] inline core::Status
encode_udp(const Frame &in, std::string_view domain, std::uint64_t frame_seq,
           unsigned char *out, std::size_t capacity, std::size_t &out_len) noexcept {
  if (domain.empty() || domain.size() >= kDomainSize || out == nullptr) {
    return {core::StatusCode::kInvalidArgument};
  }
  if (in.items.size() > kMaxItems || in.channels.size() > kMaxChannels) {
    return {core::StatusCode::kOutOfRange};
  }
  for (const FrameChannel &c : in.channels) {
    if (c.name.size() >= kChannelNameSize) {
      return {core::StatusCode::kOutOfRange};
    }
  }
  const std::size_t need = sizeof(UdpHeader) +
                           in.items.size() * sizeof(LinkItem) +
                           in.channels.size() * sizeof(LinkChannel);
  if (need > capacity || need > kMaxUdpPayload) {
    return {core::StatusCode::kOutOfRange};
  }
  socket_detail::put_u32_le(out + 0, kUdpMagic);
  socket_detail::put_u32_le(out + 4, kUdpVersion);
  for (std::size_t i = 0; i < domain.size(); ++i) {
    out[8 + i] = static_cast<unsigned char>(domain[i]);
  }
  out[8 + domain.size()] = '\0';
  for (std::size_t i = 8 + domain.size() + 1; i < 8 + kDomainSize; ++i) {
    out[i] = '\0';
  }
  socket_detail::put_u64_le(out + 40, frame_seq);
  socket_detail::put_u64_le(out + 48, in.step);
  socket_detail::put_u64_le(out + 56, std::bit_cast<std::uint64_t>(in.time));
  socket_detail::put_u64_le(out + 64, static_cast<std::uint64_t>(in.items.size()));
  socket_detail::put_u64_le(out + 72, static_cast<std::uint64_t>(in.channels.size()));
  unsigned char *dst = out + sizeof(UdpHeader);
  for (const FrameItem &src : in.items) {
    LinkItem wire{};
    wire.x = src.x;
    wire.y = src.y;
    wire.z = src.z;
    wire.vx = src.vx;
    wire.vy = src.vy;
    wire.vz = src.vz;
    wire.size = src.size;
    wire.color = src.color;
    wire.has_velocity = src.has_velocity ? std::uint8_t{1} : std::uint8_t{0};
    std::memcpy(dst, &wire, sizeof(wire));
    dst += sizeof(wire);
  }
  for (const FrameChannel &src : in.channels) {
    LinkChannel wire{};
    for (std::size_t i = 0; i < src.name.size(); ++i) {
      wire.name[i] = src.name[i];
    }
    wire.name[src.name.size()] = '\0';
    wire.value = src.value;
    std::memcpy(dst, &wire, sizeof(wire));
    dst += sizeof(wire);
  }
  out_len = need;
  return core::Status::success();
}

// Compact UDP decode (cold reader side: may allocate, NOT noexcept — never
// call inside the sim loop). Validates magic, version, domain shape (and,
// when want_domain is non-empty, exact domain match — the per-datagram
// attach gate), count bounds, exact length match, and NUL-terminated names.
// Violations are kInvalidArgument with `out`/`out_seq` untouched.
[[nodiscard]] inline core::Status decode_udp(const unsigned char *bytes, std::size_t len,
                                            std::string_view want_domain, Frame &out,
                                            std::uint64_t &out_seq) {
  if (bytes == nullptr || len < sizeof(UdpHeader)) {
    return {core::StatusCode::kInvalidArgument};
  }
  if (socket_detail::get_u32_le(bytes + 0) != kUdpMagic ||
      socket_detail::get_u32_le(bytes + 4) != kUdpVersion) {
    return {core::StatusCode::kInvalidArgument};
  }
  std::size_t domain_len = kDomainSize;
  for (std::size_t i = 0; i < kDomainSize; ++i) {
    if (bytes[8 + i] == '\0') {
      domain_len = i;
      break;
    }
  }
  if (domain_len == 0 || domain_len >= kDomainSize) {
    return {core::StatusCode::kInvalidArgument};
  }
  if (!want_domain.empty() &&
      (want_domain.size() != domain_len ||
       std::string_view(reinterpret_cast<const char *>(bytes + 8), domain_len) !=
           want_domain)) {
    return {core::StatusCode::kInvalidArgument};
  }
  const std::uint64_t seq = socket_detail::get_u64_le(bytes + 40);
  const std::uint64_t step = socket_detail::get_u64_le(bytes + 48);
  const double sim_time = std::bit_cast<double>(socket_detail::get_u64_le(bytes + 56));
  const std::uint64_t n_items = socket_detail::get_u64_le(bytes + 64);
  const std::uint64_t n_channels = socket_detail::get_u64_le(bytes + 72);
  if (n_items > static_cast<std::uint64_t>(kMaxItems) ||
      n_channels > static_cast<std::uint64_t>(kMaxChannels)) {
    return {core::StatusCode::kInvalidArgument};
  }
  const std::size_t want =
      sizeof(UdpHeader) + static_cast<std::size_t>(n_items) * sizeof(LinkItem) +
      static_cast<std::size_t>(n_channels) * sizeof(LinkChannel);
  if (want != len) {
    return {core::StatusCode::kInvalidArgument};
  }
  const unsigned char *src = bytes + sizeof(UdpHeader);
  for (std::uint64_t k = 0; k < n_channels; ++k) {
    const unsigned char *name =
        src + static_cast<std::size_t>(n_items) * sizeof(LinkItem) +
        k * sizeof(LinkChannel);
    bool terminated = false;
    for (std::size_t i = 0; i < kChannelNameSize; ++i) {
      if (name[i] == '\0') {
        terminated = true;
        break;
      }
    }
    if (!terminated) {
      return {core::StatusCode::kInvalidArgument};
    }
  }
  out.step = step;
  out.time = sim_time;
  out.items.resize(static_cast<std::size_t>(n_items));
  for (std::uint64_t k = 0; k < n_items; ++k) {
    LinkItem wire{};
    std::memcpy(&wire, src + k * sizeof(LinkItem), sizeof(wire));
    FrameItem &dst = out.items[static_cast<std::size_t>(k)];
    dst.x = wire.x;
    dst.y = wire.y;
    dst.z = wire.z;
    dst.vx = wire.vx;
    dst.vy = wire.vy;
    dst.vz = wire.vz;
    dst.size = wire.size;
    dst.color = wire.color;
    dst.has_velocity = wire.has_velocity != std::uint8_t{0};
  }
  src += static_cast<std::size_t>(n_items) * sizeof(LinkItem);
  out.channels.resize(static_cast<std::size_t>(n_channels));
  for (std::uint64_t k = 0; k < n_channels; ++k) {
    LinkChannel wire{};
    std::memcpy(&wire, src + k * sizeof(LinkChannel), sizeof(wire));
    out.channels[static_cast<std::size_t>(k)].name.assign(wire.name);
    out.channels[static_cast<std::size_t>(k)].value = wire.value;
  }
  out_seq = seq;
  return core::Status::success();
}

// host:port endpoint (CLI --emit-tcp/--emit-udp, publisher constructors).
// Split on the LAST colon (tolerates [v6 brackets]); host must be
// non-empty, port numeric 1..65535. Shape failures are kInvalidArgument
// (CLI exit 2); unresolvable hosts fail later at connect (kInternal).
struct HostPort {
  std::string host;
  std::uint16_t port = 0;
};

[[nodiscard]] inline core::Result<HostPort> parse_endpoint(std::string_view text) {
  const std::size_t colon = text.rfind(':');
  if (colon == std::string_view::npos || colon == 0 || colon + 1 >= text.size()) {
    return core::Result<HostPort>::failure("want host:port");
  }
  std::string_view host = text.substr(0, colon);
  const std::string_view port_text = text.substr(colon + 1);
  if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
    host = host.substr(1, host.size() - 2);
  }
  if (host.empty()) {
    return core::Result<HostPort>::failure("want host:port");
  }
  if (port_text.size() > 5) {
    return core::Result<HostPort>::failure("port out of range (1..65535)");
  }
  unsigned long port = 0;
  for (char c : port_text) {
    if (c < '0' || c > '9') {
      return core::Result<HostPort>::failure("port must be numeric");
    }
    port = port * 10 + static_cast<unsigned long>(c - '0');
  }
  if (port < 1 || port > 65535) {
    return core::Result<HostPort>::failure("port out of range (1..65535)");
  }
  HostPort hp;
  hp.host.assign(host.data(), host.size());
  hp.port = static_cast<std::uint16_t>(port);
  return core::Result<HostPort>::success(std::move(hp));
}

class TcpPublisher : public emit::ISink {
public:
  explicit TcpPublisher(std::string_view endpoint, std::string_view domain,
                        FrameAdapter adapter = &frame_from_snapshot);
  ~TcpPublisher() override;

  TcpPublisher(const TcpPublisher &) = delete;
  TcpPublisher &operator=(const TcpPublisher &) = delete;
  TcpPublisher(TcpPublisher &&other) noexcept;
  TcpPublisher &operator=(TcpPublisher &&other) noexcept;

  void on_snapshot(const world::Snapshot &snapshot) override;

  // Non-blocking idle progress for a partially accepted frame: retries the
  // pending bytes once without requiring a new snapshot and without
  // counting a drop. Returns true when no pending frame remains. A false
  // return with ok() still true is transient backpressure (retry later,
  // e.g. via finish()); a false return with !ok() is a dead peer (sticky
  // kInternal). Never blocks, never throws; safe on the sim thread.
  bool service_pending() noexcept;

  // Bounded shutdown drain owned by the application edge (never the hot
  // path, never the destructor): waits up to timeout_ms for socket
  // writability while servicing pending bytes. Returns success iff no
  // pending frame remains; kInvalidArgument for timeout_ms < 0; kInternal
  // on deadline expiry or dead peer with pending preserved for explicit
  // reporting (pending_bytes()/has_pending_frame()). A timeout does NOT
  // poison the publisher (retry or abandon explicitly); a dead peer DOES
  // (sticky, like on_snapshot). timeout_ms == 0 is a single non-blocking
  // service attempt.
  [[nodiscard]] core::Status finish(int timeout_ms) noexcept;

  [[nodiscard]] bool ok() const noexcept { return status_.ok(); }
  [[nodiscard]] core::Status status() const noexcept { return status_; }
  // errno captured at the failing connect/send syscall (0 when ok).
  [[nodiscard]] int last_errno() const noexcept { return last_errno_; }
  [[nodiscard]] const PublisherStats &stats() const noexcept { return stats_; }
  [[nodiscard]] bool has_pending_frame() const noexcept { return pending_size_ != 0; }
  // Remaining unsent bytes of the pending frame (0 when idle clean).
  [[nodiscard]] std::size_t pending_bytes() const noexcept {
    return pending_size_ == 0 ? 0 : pending_size_ - pending_offset_;
  }
  [[nodiscard]] const std::string &endpoint() const noexcept { return endpoint_; }

private:
  void cleanup() noexcept;

  std::string endpoint_;
  FrameAdapter adapter_ = &frame_from_snapshot;
  int fd_ = -1;
  LinkFrame staging_{}; // encode target: no per-emit stack bloat, no alloc
  std::array<unsigned char, kTcpFrameBytes> wire_{}; // prefix + frame bytes
  std::size_t pending_offset_ = 0;
  std::size_t pending_size_ = 0;
  std::uint64_t frame_seq_ = 0;
  PublisherStats stats_{};
  core::Status status_{};
  int last_errno_ = 0;
};

class UdpPublisher : public emit::ISink {
public:
  explicit UdpPublisher(std::string_view endpoint, std::string_view domain,
                        FrameAdapter adapter = &frame_from_snapshot);
  ~UdpPublisher() override;

  UdpPublisher(const UdpPublisher &) = delete;
  UdpPublisher &operator=(const UdpPublisher &) = delete;
  UdpPublisher(UdpPublisher &&other) noexcept;
  UdpPublisher &operator=(UdpPublisher &&other) noexcept;

  void on_snapshot(const world::Snapshot &snapshot) override;

  [[nodiscard]] bool ok() const noexcept { return status_.ok(); }
  [[nodiscard]] core::Status status() const noexcept { return status_; }
  // errno captured at the failing connect/send syscall (0 when ok).
  [[nodiscard]] int last_errno() const noexcept { return last_errno_; }
  [[nodiscard]] const PublisherStats &stats() const noexcept { return stats_; }
  [[nodiscard]] const std::string &endpoint() const noexcept { return endpoint_; }

private:
  void cleanup() noexcept;

  std::string endpoint_;
  std::string domain_;
  FrameAdapter adapter_ = &frame_from_snapshot;
  int fd_ = -1;
  LinkFrame staging_{}; // over-bound gate target (cross-transport verdict)
  std::array<unsigned char, kMaxUdpPayload> datagram_{}; // compact encode target
  std::uint64_t frame_seq_ = 0;
  PublisherStats stats_{};
  core::Status status_{};
  int last_errno_ = 0;
};

} // namespace wengine::link
