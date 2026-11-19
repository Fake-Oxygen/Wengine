/**
 * @file
 * TCP/UDP socket publishers: networked endpoints of the wengine-link snapshot bus (class B ordered bounded sample streaming).
 * @note Two emit::ISink publishers (TcpPublisher/UdpPublisher), built and owned by the caller
 *       (CLI/GUI edge) and attached like any sink. Both speak the transport-independent compact
 *       v2 codec (explicit portable bytes, used entries only); both are non-blocking
 *       copy-and-return with sticky core::Status, so a slow or absent peer never stalls the sim
 *       loop. Bounded and non-blocking: TCP uses the kernel send buffer; UDP drops a backpressured
 *       datagram whole. Over-bound frames are refused on every transport identically (sticky
 *       kOutOfRange, never silently truncated); dead peer is sticky kInternal. Local socket
 *       acceptance is not remote receipt (receivers account loss with LagStats over frame_seq).
 *       Headers stay platform-free; the POSIX TU is src/link/socket_publisher.cpp under
 *       wengine_link_shm — a Windows port replaces that TU, never this header.
 *       Consumers: CLI --emit-tcp/--emit-udp, Zephyr-over-IP readers, Python receivers.
 */
#pragma once

// TCP/UDP socket publishers: networked endpoints of the wengine-link
// snapshot bus (SIG-7, first networked consumer: Zephyr over IP).
//
// Two emit::ISink publishers, built and owned by the caller (CLI/GUI edge)
// and attached like any sink — same fan-out seam as CsvSink/ShmPublisher,
// no engine change. Both speak the transport-independent compact v2 codec
// (compact_codec.hpp: explicit portable bytes, used entries only) and the
// SHM versioning wording; both are non-blocking copy-and-return with
// sticky core::Status, so a slow or absent peer never stalls the sim loop.
//
// TCP (TcpPublisher): active connect to host:port, then a hello
// (TcpHello: magic "WLT2", schema version 3, NUL-terminated domain)
// followed by length-prefixed compact records (u32 LE byte count +
// record). The hello IS the attach gate, in SHM wording: the reader
// validates magic/schema/domain with check_tcp_hello and closes on
// mismatch — loud refusal, never silent mid-stream. Queueing is the kernel
// send buffer (256 KiB default — thousands of compact records, tunable
// only by rebuild, documented in signals.md); on backpressure the
// attempted frame is dropped and counted (dropped_send) WITHOUT poisoning
// the publisher, except a partially accepted frame is completed before any
// later frame is sent (TCP streams cannot discard a prefix without
// corrupting framing), while over-bound frames (dropped_over_bound,
// kOutOfRange) and a dead peer (EPIPE/reset, kInternal) are sticky like
// SHM. run_id identifies the publisher run for receiver session resets.
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
// UDP has none), carrying the same compact v2 records (each one always
// fits its configured payload budget by construction). Datagrams
// self-describe per packet (magic/schema/run/frame_seq/clock/counts).
// Readers gate each datagram with compact::decode_record (exact length,
// else kInvalidArgument) and account loss with LagStats over frame_seq —
// same vocabulary as the SHM ring. Over-bound frames are refused by the
// compact bound verdict, identical on every transport.
//
// MTU BUDGET (PHYSI-41): kMaxUdpPayload (65,507) is the protocol maximum,
// not a safe network budget — a 64 KiB datagram fragments on ordinary
// paths and loss of one fragment loses the whole datagram. UdpPublisher
// therefore enforces an explicit per-publisher payload budget
// (payload_budget(), default kDefaultUdpPayloadBudget = 1,200 bytes):
// every emitted datagram satisfies record_bytes <= budget, headers
// included (the datagram IS the record, no extra framing). A record that
// does not fit its budget is indivisible — UDP has no fragmentation or
// batch-splitting here — so it is refused loudly (dropped_over_bound,
// sticky kOutOfRange, last_over_bound_bytes() records the needed size,
// 0 when over the codec maximum) exactly like a codec over-bound frame.
// Shrink records with --emit-udp-select (fewer channels) or
// --emit-udp-stride/--emit-every (fewer datagrams, same size); consecutive
// ticks batch without decimation via link/sample_block.hpp (PHYSI-44
// builder/codec, schema once per block). Splitting one tick across several
// budgeted datagrams is the packetizer's remaining extension point.
//
// PORTABILITY: POSIX-only TU (src/link/socket_publisher.cpp) — sockets,
// getaddrinfo, non-blocking I/O; the Windows port replaces that TU, never
// this header. Not copyable (owns fd); movable. Single-writer model: call
// on_snapshot from one runner thread only, like every Runner-driven sink.

#include "wengine/core/status.hpp"
#include "wengine/emit/sink.hpp"
#include "wengine/link/compact_codec.hpp"
#include "wengine/link/frame.hpp"
#include "wengine/link/protocol.hpp"
#include "wengine/world/state.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

namespace wengine::link {

// TCP stream framing: magic "WLT2" (LE bytes 'W','L','T','2'); payloads
// are compact v2 records (compact_codec.hpp) under one u32 LE length
// prefix per record.
/** @brief TCP stream magic "WLT2" (LE bytes 'W','L','T','2'); payloads are compact v2 records under one u32 LE length prefix per record. */
inline constexpr std::uint32_t kTcpMagic = 0x32544C57u;
// Socket-schema version, tracked independently of the SHM layout version
// (link::kVersion): socket readers gate on this, SHM readers on that.
/** @brief Socket-schema version, tracked independently of the SHM layout version (socket readers gate on this, SHM readers on link::kVersion). */
inline constexpr std::uint32_t kTcpVersion = compact::kSchemaVersion;
// Kernel send buffer sized at construction (best-effort setsockopt):
// thousands of compact records before backpressure drops begin.
/** @brief Kernel send buffer sized at construction (256 KiB default: thousands of compact records before backpressure drops begin; tunable only by rebuild). */
inline constexpr int kSocketSendBuffer = 256 * 1024;
// Bounded shutdown drain for TcpPublisher::finish (application edge owns
// the deadline; destructors never wait).
/** @brief Bounded shutdown drain for TcpPublisher::finish (application edge owns the deadline; destructors never wait). */
inline constexpr int kTcpFinishTimeoutMs = 2000;
// Largest UDP payload per RFC 768 (65535 - 20 IP - 8 UDP).
/** @brief Largest UDP payload per RFC 768 (65535 - 20 IP - 8 UDP): the protocol maximum, not a safe network budget. */
inline constexpr std::size_t kMaxUdpPayload = 65507;
// Default UDP payload budget (PHYSI-41): conservative MTU-safe profile.
// 1,200 bytes fits the 1,280-byte IPv6 minimum MTU (minus v6/UDP headers)
// and any 1,500-byte Ethernet path with margin, so datagrams avoid IP
// fragmentation (one lost fragment loses the whole datagram). One signal
// source is ~120 bytes, so the default carries ~8 sources; larger frames
// must select/stride down or raise the budget explicitly up to
// kMaxUdpPayload. Documented in docs/signals.md sec. 5.
/** @brief Default UDP payload budget (MTU-safe profile: fits the 1280-byte IPv6 minimum MTU and any 1500-byte Ethernet path with margin, ~8 signal sources at ~120 B each). */
inline constexpr std::size_t kDefaultUdpPayloadBudget = 1200;
// TCP wire buffer: length prefix + the largest encodable record.
/** @brief TCP wire buffer: length prefix + the largest encodable record. */
inline constexpr std::size_t kTcpWireBytes = 4 + compact::kMaxRecordBytes;

// TCP hello: first bytes of every connection (plain struct, LE on the
// wire; see hello_bytes/check_tcp_hello below).
/**
 * @brief TCP hello: first bytes of every connection (plain struct, LE on the wire).
 * @note The hello IS the attach gate, in SHM wording: the reader validates magic/schema/domain and closes on mismatch — loud refusal, never silent mid-stream.
 */
struct TcpHello {
/** @brief Hello magic (must equal kTcpMagic). */
  std::uint32_t magic = kTcpMagic;
/** @brief Hello schema version (must equal kTcpVersion; socket-schema versions exact-match at attach with loud found-version refusal). */
  std::uint32_t version = kTcpVersion;
/** @brief NUL-terminated solver/domain tag (exact-match gate). */
  char domain[kDomainSize] = {}; // NUL-terminated solver/domain tag
};

static_assert(sizeof(TcpHello) == 4 + 4 + kDomainSize);



namespace socket_detail {

/** @brief Store a u32 as little-endian bytes. @param[out] dst 4-byte destination buffer. @param[in] v value to store. */
inline void put_u32_le(unsigned char *dst, std::uint32_t v) noexcept {
  dst[0] = static_cast<unsigned char>(v & 0xFFu);
  dst[1] = static_cast<unsigned char>((v >> 8) & 0xFFu);
  dst[2] = static_cast<unsigned char>((v >> 16) & 0xFFu);
  dst[3] = static_cast<unsigned char>((v >> 24) & 0xFFu);
}

/** @brief Store a u64 as little-endian bytes. @param[out] dst 8-byte destination buffer. @param[in] v value to store. */
inline void put_u64_le(unsigned char *dst, std::uint64_t v) noexcept {
  for (int k = 0; k < 8; ++k) {
    dst[k] = static_cast<unsigned char>((v >> (8 * k)) & 0xFFu);
  }
}

/** @brief Load a little-endian u32. @param[in] src 4-byte source buffer. @return The decoded u32 value. */
inline std::uint32_t get_u32_le(const unsigned char *src) noexcept {
  return static_cast<std::uint32_t>(src[0]) |
         (static_cast<std::uint32_t>(src[1]) << 8) |
         (static_cast<std::uint32_t>(src[2]) << 16) |
         (static_cast<std::uint32_t>(src[3]) << 24);
}

/** @brief Load a little-endian u64. @param[in] src 8-byte source buffer. @return The decoded u64 value. */
inline std::uint64_t get_u64_le(const unsigned char *src) noexcept {
  std::uint64_t v = 0;
  for (int k = 0; k < 8; ++k) {
    v |= static_cast<std::uint64_t>(src[k]) << (8 * k);
  }
  return v;
}

} // namespace socket_detail

// Reader-side TCP hello gate (shared with future Zephyr readers): exact
// magic, exact schema version, exact domain, NUL-terminated domain.
// Anything else (garbage, foreign magics, future schemas) is
// kInvalidArgument — the "loud refusal" behind a reader close.
/**
 * @brief Reader-side TCP hello gate (shared with future Zephyr readers): exact magic, exact schema version, exact NUL-terminated domain.
 * @param[in] hello received hello struct.
 * @param[in] want_domain expected domain tag.
 * @return kOk on exact match; kInvalidArgument otherwise (garbage, foreign magics, future schemas) — the loud refusal behind a reader close.
 */
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

// host:port endpoint (CLI --emit-tcp/--emit-udp, publisher constructors).
// Split on the LAST colon (tolerates [v6 brackets]); host must be
// non-empty, port numeric 1..65535. Shape failures are kInvalidArgument
// (CLI exit 2); unresolvable hosts fail later at connect (kInternal).
/** @brief host:port endpoint (CLI --emit-tcp/--emit-udp, publisher constructors). Split on the LAST colon (tolerates [v6 brackets]); host non-empty, port numeric 1..65535. */
struct HostPort {
/** @brief Host text (brackets stripped). */
  std::string host;
/** @brief Numeric port 1..65535. */
  std::uint16_t port = 0;
};

/**
 * @brief Parse host:port endpoint text.
 * @param[in] text endpoint text (v6 hosts in brackets).
 * @return Result with HostPort on success; failure (kInvalidArgument, "want host:port" / "port must be numeric" / "port out of range") for shape failures (CLI exit 2). Unresolvable hosts fail later at connect (kInternal).
 */
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

/**
 * @brief TCP snapshot publisher: active connect to host:port, then hello + length-prefixed compact records.
 * @note Non-blocking copy-and-return with sticky core::Status; a slow peer never stalls the sim loop
 *       (queueing is the kernel send buffer). A partially accepted frame completes before any later
 *       frame is sent (stream framing safety); the destructor never waits — undrained pending bytes
 *       are abandoned at close and reported by the finish caller. Not copyable (owns fd); movable.
 *       Single-writer model: call on_snapshot from one runner thread only.
 */
class TcpPublisher : public emit::ISink {
public:
/**
 * @brief Actively connect to a listening reader and hello-gate the stream.
 * @param[in] endpoint host:port text of the listening reader.
 * @param[in] domain record domain (hello gate + every record).
 * @param[in] adapter per-snapshot mapping (default: particle adapter).
 */
  explicit TcpPublisher(std::string_view endpoint, std::string_view domain,
                        FrameAdapter adapter = &frame_from_snapshot);
/** @brief Close the socket (never waits: pending bytes are abandoned at close and must be reported via pending_bytes()/has_pending_frame()). */
  ~TcpPublisher() override;

/** @brief Non-copyable (owns fd). */
  TcpPublisher(const TcpPublisher &) = delete;
/** @brief Non-copyable (owns fd). */
  TcpPublisher &operator=(const TcpPublisher &) = delete;
/** @brief Move-construct. @param[in] other Source; left inert. */
  TcpPublisher(TcpPublisher &&other) noexcept;
/** @brief Move-assign. @param[in] other Source; left inert. */
  TcpPublisher &operator=(TcpPublisher &&other) noexcept;

/**
 * @brief Publish one emitted snapshot (retries the pending tail first, never blocks).
 * @param[in] snapshot Runner-owned emission buffer (observed synchronously; never retained).
 * @note Transient backpressure drops the attempted frame and counts dropped_send WITHOUT poisoning
 *       the publisher; over-bound frames (dropped_over_bound, sticky kOutOfRange) and a dead peer
 *       (EPIPE/reset, sticky kInternal) poison it. Local acceptance only — remote receipt unknown.
 */
  void on_snapshot(const world::Snapshot &snapshot) override;

  // Non-blocking idle progress for a partially accepted frame: retries the
  // pending bytes once without requiring a new snapshot and without
  // counting a drop. Returns true when no pending frame remains. A false
  // return with ok() still true is transient backpressure (retry later,
  // e.g. via finish()); a false return with !ok() is a dead peer (sticky
  // kInternal). Never blocks, never throws; safe on the sim thread.
/**
 * @brief Non-blocking idle progress for a partially accepted frame: retries the pending bytes once without a new snapshot and without counting a drop.
 * @return True when no pending frame remains. False with ok() still true is transient backpressure (retry later, e.g. via finish()); false with !ok() is a dead peer (sticky kInternal). Never blocks, never throws; safe on the sim thread.
 */
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
/**
 * @brief Bounded shutdown drain owned by the application edge (never the hot path, never the destructor).
 * @param[in] timeout_ms wait budget in ms for socket writability (0 = single non-blocking attempt; negative is kInvalidArgument).
 * @return kOk iff no pending frame remains; kInvalidArgument for timeout_ms < 0; kInternal on deadline expiry or dead peer with pending preserved for explicit reporting (pending_bytes()/has_pending_frame()). A timeout does NOT poison the publisher (retry or abandon explicitly); a dead peer DOES (sticky, like on_snapshot).
 */
  [[nodiscard]] core::Status finish(int timeout_ms) noexcept;

/** @brief Success predicate. @return True while no sticky failure latched. */
  [[nodiscard]] bool ok() const noexcept { return status_.ok(); }
/** @brief Latched sticky status. @return kOk, or the first sticky failure (kOutOfRange/kInternal). */
  [[nodiscard]] core::Status status() const noexcept { return status_; }
  // errno captured at the failing connect/send syscall (0 when ok).
/** @brief Errno captured at the failing connect/send syscall (0 when ok). @return Captured errno value. */
  [[nodiscard]] int last_errno() const noexcept { return last_errno_; }
/** @brief Monotonic publisher counters (attempted/published/over_bound/backpressure/pending_blocked/disabled/bytes_accepted; local acceptance only). @return Counter snapshot. */
  [[nodiscard]] const PublisherStats &stats() const noexcept { return stats_; }
/** @brief Pending-tail flag (at most one frame held for framing safety). @return True while a partial frame is held. */
  [[nodiscard]] bool has_pending_frame() const noexcept { return pending_size_ != 0; }
  // Remaining unsent bytes of the pending frame (0 when idle clean).
/** @brief Remaining unsent bytes of the pending frame (0 when idle clean). @return Unsent byte count. */
  [[nodiscard]] std::size_t pending_bytes() const noexcept {
    return pending_size_ == 0 ? 0 : pending_size_ - pending_offset_;
  }
/** @brief Configured host:port endpoint text. @return Endpoint string. */
  [[nodiscard]] const std::string &endpoint() const noexcept { return endpoint_; }
/** @brief Publisher-run identity (opaque, fixed at construction; receiver session resets). @return Run id value. */
  [[nodiscard]] std::uint64_t run_id() const noexcept { return run_id_; }

private:
  void cleanup() noexcept;

  std::string endpoint_;
  std::string domain_; // record domain (hello gate + every record)
  FrameAdapter adapter_ = &frame_from_snapshot;
  std::uint64_t run_id_ = 0; // run identity (opaque, fixed at construction)
  int fd_ = -1;
  std::array<unsigned char, kTcpWireBytes> wire_; // prefix + record bytes
  std::size_t pending_offset_ = 0;
  std::size_t pending_size_ = 0;
  std::uint64_t frame_seq_ = 0;
  PublisherStats stats_{};
  core::Status status_{};
  int last_errno_ = 0;
};

/**
 * @brief UDP snapshot publisher: connected datagrams to host:port (no handshake — UDP has none), each record within its payload budget by construction.
 * @note Datagrams self-describe per packet (magic/schema/run/frame_seq/clock/counts); readers gate
 *       each datagram and account loss with LagStats over frame_seq. No pending tail (no length
 *       framing to corrupt): backpressure is a loud drop (dropped_send), never a stall and never a
 *       poisoned publisher. Not copyable (owns fd); movable. Single-writer model.
 */
class UdpPublisher : public emit::ISink {
public:
/**
 * @brief Create connected datagrams to host:port with an explicit payload budget.
 * @param[in] endpoint host:port text of the receiver.
 * @param[in] domain record domain of every datagram.
 * @param[in] adapter per-snapshot mapping (default: particle adapter).
 * @param[in] payload_budget per-datagram payload budget in bytes (1..kMaxUdpPayload; default MTU-safe); every emitted datagram satisfies record_bytes <= budget, headers included.
 */
  explicit UdpPublisher(std::string_view endpoint, std::string_view domain,
                        FrameAdapter adapter = &frame_from_snapshot,
                        std::size_t payload_budget = kDefaultUdpPayloadBudget);
/** @brief Close the socket. */
  ~UdpPublisher() override;

/** @brief Non-copyable (owns fd). */
  UdpPublisher(const UdpPublisher &) = delete;
/** @brief Non-copyable (owns fd). */
  UdpPublisher &operator=(const UdpPublisher &) = delete;
/** @brief Move-construct. @param[in] other Source; left inert. */
  UdpPublisher(UdpPublisher &&other) noexcept;
/** @brief Move-assign. @param[in] other Source; left inert. */
  UdpPublisher &operator=(UdpPublisher &&other) noexcept;

/**
 * @brief Publish one emitted snapshot as a single datagram (never fragments or batch-splits a tick).
 * @param[in] snapshot Runner-owned emission buffer (observed synchronously; never retained).
 * @note A record beyond the payload budget is refused loudly (dropped_over_bound, sticky kOutOfRange;
 *       last_over_bound_bytes() records the needed size) — shrink records with --emit-udp-select or
 *       --emit-udp-stride/--emit-every (fewer datagrams, same size), or batch without decimation via
 *       link/sample_block.hpp. Splitting one tick across budgeted datagrams is the packetizer's extension point.
 */
  void on_snapshot(const world::Snapshot &snapshot) override;

/** @brief Success predicate. @return True while no sticky failure latched. */
  [[nodiscard]] bool ok() const noexcept { return status_.ok(); }
/** @brief Latched sticky status. @return kOk, or the first sticky failure. */
  [[nodiscard]] core::Status status() const noexcept { return status_; }
  // errno captured at the failing connect/send syscall (0 when ok).
/** @brief Errno captured at the failing connect/send syscall (0 when ok). @return Captured errno value. */
  [[nodiscard]] int last_errno() const noexcept { return last_errno_; }
/** @brief Monotonic publisher counters (local acceptance only; remote receipt unknown without a receiver report). @return Counter snapshot. */
  [[nodiscard]] const PublisherStats &stats() const noexcept { return stats_; }
/** @brief Configured host:port endpoint text. @return Endpoint string. */
  [[nodiscard]] const std::string &endpoint() const noexcept { return endpoint_; }
/** @brief Publisher-run identity (opaque, fixed at construction; receiver session resets). @return Run id value. */
  [[nodiscard]] std::uint64_t run_id() const noexcept { return run_id_; }
  // Configured datagram payload budget (bytes, 1..kMaxUdpPayload). Every
  // emitted datagram satisfies bytes <= budget, headers included.
/** @brief Configured datagram payload budget in bytes (1..kMaxUdpPayload). Every emitted datagram satisfies bytes <= budget, headers included. @return Budget value. */
  [[nodiscard]] std::size_t payload_budget() const noexcept { return payload_budget_; }
  // Needed record bytes from the most recent over-bound refusal (0 when the
  // frame exceeds the codec maximum and has no finite encoded size, or when
  // no refusal has occurred yet). Compare against payload_budget() for the
  // "need X > budget Y" diagnostic; never silently truncated.
/** @brief Needed record bytes from the most recent over-bound refusal (0 when the frame exceeds the codec maximum and has no finite encoded size, or when no refusal occurred yet). Compare against payload_budget() for the "need X > budget Y" diagnostic; never silently truncated. @return Needed byte count or 0. */
  [[nodiscard]] std::size_t last_over_bound_bytes() const noexcept {
    return last_over_bound_bytes_;
  }

private:
  void cleanup() noexcept;

  std::string endpoint_;
  std::string domain_;
  FrameAdapter adapter_ = &frame_from_snapshot;
  std::uint64_t run_id_ = 0; // run identity (opaque, fixed at construction)
  int fd_ = -1;
  std::array<unsigned char, kMaxUdpPayload> datagram_{}; // record encode target
  std::size_t payload_budget_ = kDefaultUdpPayloadBudget;
  std::size_t last_over_bound_bytes_ = 0;
  std::uint64_t frame_seq_ = 0;
  PublisherStats stats_{};
  core::Status status_{};
  int last_errno_ = 0;
};

} // namespace wengine::link
