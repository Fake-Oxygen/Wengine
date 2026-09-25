#pragma once

// Compact socket codec (PHYSI-37): transport-independent v2 record
// encode/decode for the wengine-link socket transports.
//
// WHY A SECOND SOCKET SCHEMA: v1 sockets have two defects this replaces
// (wiring into publishers/readers is PHYSI-38, not here):
//   * TCP copies all 68,136 LinkFrame bytes per emission regardless of
//     content (1 kHz x one signal = 68.14 MB/s of framing).
//   * v1 UDP is compact in size but memcpy's native LinkItem/LinkChannel
//     bodies while claiming portable little-endian encoding — padding,
//     double layout and alignment are assumed, never specified.
// v2 specifies EVERY byte: all integers little-endian, all doubles
// IEEE-754 binary64 bit patterns, no padding anywhere, only populated
// entries encoded. One signal source is ~120 bytes, not 68 KB.
//
// WIRE LAYOUT (v2, portable by construction):
//
//   record        = header item* channel*
//   header        = magic u32 "WLV2" (0x32564C57)
//                   schema u32 (= 2, socket-schema versions, see below)
//                   kind u8 (= 1 frame; 0 and 2..255 reserved)
//                   reserved u8[3] (must be zero on decode)
//                   run_id u64 (publisher-run identity, opaque)
//                   frame_seq u64 (monotonic per run; LagStats loss source)
//                   step_index u64
//                   sim_time f64 (IEEE-754 bits)
//                   domain u8[32] (NUL-terminated tag, non-empty; v1 rule)
//                   item_count u32
//                   channel_count u32                              [= 84 B]
//   item          = x,y,z,vx,vy,vz,size f64 x7, color u32,
//                   has_velocity u8 (0/1 only)                     [= 61 B]
//   channel       = name_len u8 (0..31), name u8[name_len] (raw bytes),
//                   value f64                        [= 9..40 B]
//
// VERSIONING: socket-schema versions are independent of the SHM layout
// version (link::kVersion): the SHM segment gate never sees these bytes
// and socket readers never see SegmentHeader, so the two evolve apart.
// Magic distinguishes the families on the wire ("WLT1" hello / "WLU1"
// datagram / "WLV2" record); a raw v1-TCP frame has no magic at all and
// classifies as unknown (see classify_record). Readers support exactly
// one schema and refuse everything else loudly — same policy as v1.
//
// RUN IDENTITY: run_id distinguishes publisher restarts under one address
// (PHYSI-40 session semantics): a receiver seeing run_id change resets its
// cursor instead of counting the whole previous run as missed. The codec
// treats it as opaque; GENERATION belongs to the publisher edge (PHYSI-38:
// e.g. random + pid + time). No value is reserved.
//
// RECORD KIND: kind reserves framing for later record types (PHYSI-44
// sample batches, end summaries) without a new magic. decode_record only
// understands kFrame today and reports any other kind loudly; future kinds
// get their own decode entry, never a silent reinterpretation.
//
// BOUNDS (shared with v1 vocabulary, never silently truncated):
//   items <= kMaxItems, channels <= kMaxChannels, names <= 31 chars,
//   total <= kCompactMaxBytes (65507: every v2 record fits a maximum-size
//   UDP payload, so one codec serves both socket transports).
// A maximum-shape record (1024 items + 64 longest-name channels) is 65,108
// bytes and always fits; anything over is kOutOfRange with `out` untouched.
//
// LAYERING: pure helpers over link::Frame (frame.hpp), no sockets, no SHM,
// no publishers — PHYSI-38 wires these into TcpPublisher/UdpPublisher and
// the Python decoder. Hot encode path is noexcept/allocation-free like
// encode_frame; decode may allocate (cold reader side only).

#include "wengine/core/status.hpp"
#include "wengine/link/frame.hpp"
#include "wengine/link/protocol.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace wengine::link::compact {

// v2 record magic "WLV2" (LE bytes 'W','L','V','2').
inline constexpr std::uint32_t kMagic = 0x32564C57u;
// v1 UDP datagram magic "WLU1", recognized for refusal only (never
// decoded here). Must equal kUdpMagic (socket_publisher.hpp); the equality
// is static_asserted in test_link_compact so the literals cannot drift.
// Named (not inlined) for exactly that tie.
inline constexpr std::uint32_t kV1UdpMagic = 0x31554C57u;
// Socket-schema version carried in every record (NOT link::kVersion: the
// SHM segment gate and socket readers evolve independently).
inline constexpr std::uint32_t kSchemaVersion = 2;
// Record kinds. Only kFrame decodes today; the rest are refused loudly
// until their decode entry exists (see header comment).
enum class RecordKind : std::uint8_t { kFrame = 1 };
// Fixed header size: 4+4+1+3+8+8+8+8+32+4+4.
inline constexpr std::size_t kHeaderSize = 84;
// Explicit item size: 7x8 + 4 + 1 (no padding, ever).
inline constexpr std::size_t kItemSize = 61;
// Channel name bound mirrors v1 (kChannelNameSize - 1 usable chars);
// the u8 length prefix carries 0..31 (empty names round-trip exactly,
// matching v1 acceptance).
inline constexpr std::size_t kMaxNameLen = kChannelNameSize - 1;
// Largest encodable record: fits a maximum-size UDP payload, so one codec
// serves datagram and stream transports alike (see header comment for the
// maximum-shape arithmetic).
inline constexpr std::size_t kMaxRecordBytes = 65507;

namespace detail {

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

inline void put_f64_le(unsigned char *dst, double v) noexcept {
  put_u64_le(dst, std::bit_cast<std::uint64_t>(v));
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

inline double get_f64_le(const unsigned char *src) noexcept {
  return std::bit_cast<double>(get_u64_le(src));
}

} // namespace detail

// Socket-schema family of a byte prefix (v1/v2 refusal vocabulary for
// PHYSI-38 dispatch): "WLU1" datagrams are v1, "WLV2"+schema 2 records are
// v2, everything else (raw v1-TCP frames have no magic, hellos, garbage,
// future schemas) is unknown — refused loudly, never guessed.
enum class SocketSchema : std::uint8_t { kUnknown, kV1, kV2 };

[[nodiscard]] inline SocketSchema classify_record(const unsigned char *bytes,
                                                  std::size_t len) noexcept {
  if (bytes == nullptr || len < 4) {
    return SocketSchema::kUnknown;
  }
  const std::uint32_t magic = detail::get_u32_le(bytes);
  if (magic == kV1UdpMagic) {
    return SocketSchema::kV1;
  }
  if (magic == kMagic && len >= 8 && detail::get_u32_le(bytes + 4) == kSchemaVersion) {
    return SocketSchema::kV2;
  }
  return SocketSchema::kUnknown;
}

// Cheap pre-gate for v2 dispatch (PHYSI-38 calls this before decode):
// exact magic + exact schema + at least a header. Anything else is
// kInvalidArgument — v1 bytes, truncated prefixes and future schemas all
// refuse identically here; decode_record re-checks everything anyway.
[[nodiscard]] inline core::Status check_v2_schema(const unsigned char *bytes,
                                                  std::size_t len) noexcept {
  if (classify_record(bytes, len) != SocketSchema::kV2 || len < kHeaderSize) {
    return {core::StatusCode::kInvalidArgument};
  }
  return core::Status::success();
}

// Exact encoded length for a Frame, or 0 when over-bound (counts, name
// lengths, or total > kMaxRecordBytes). Pure/noexcept: buffer sizing for
// publishers and tests without touching output.
[[nodiscard]] inline std::size_t encoded_size(const Frame &in) noexcept {
  if (in.items.size() > kMaxItems || in.channels.size() > kMaxChannels) {
    return 0;
  }
  std::size_t total = kHeaderSize + in.items.size() * kItemSize;
  for (const FrameChannel &c : in.channels) {
    if (c.name.size() > kMaxNameLen) {
      return 0;
    }
    total += 1 + c.name.size() + sizeof(double);
  }
  return total <= kMaxRecordBytes ? total : 0;
}

// Frame -> v2 record bytes. Hot publisher path: noexcept,
// allocation-free, bounded copies. Validates everything BEFORE touching
// `out` (domain shape, counts, name bounds, total vs capacity and vs
// kMaxRecordBytes); violations are kInvalidArgument/kOutOfRange with `out`
// and `out_len` untouched — loud, never silently truncated. Doubles cross
// as exact IEEE-754 bit patterns (NaN payloads preserved, never
// canonicalized); dimensional validation lives at the engine edge, not in
// the carrier.
[[nodiscard]] inline core::Status encode_record(const Frame &in, std::string_view domain,
                                                std::uint64_t run_id,
                                                std::uint64_t frame_seq,
                                                unsigned char *out,
                                                std::size_t capacity,
                                                std::size_t &out_len) noexcept {
  if (domain.empty() || domain.size() >= kDomainSize || out == nullptr) {
    return {core::StatusCode::kInvalidArgument};
  }
  const std::size_t need = encoded_size(in);
  if (need == 0 || need > capacity) {
    return {core::StatusCode::kOutOfRange};
  }
  detail::put_u32_le(out + 0, kMagic);
  detail::put_u32_le(out + 4, kSchemaVersion);
  out[8] = static_cast<unsigned char>(RecordKind::kFrame);
  out[9] = 0;
  out[10] = 0;
  out[11] = 0;
  detail::put_u64_le(out + 12, run_id);
  detail::put_u64_le(out + 20, frame_seq);
  detail::put_u64_le(out + 28, in.step);
  detail::put_f64_le(out + 36, in.time);
  for (std::size_t i = 0; i < domain.size(); ++i) {
    out[44 + i] = static_cast<unsigned char>(domain[i]);
  }
  out[44 + domain.size()] = '\0';
  for (std::size_t i = 44 + domain.size() + 1; i < 44 + kDomainSize; ++i) {
    out[i] = '\0';
  }
  detail::put_u32_le(out + 76, static_cast<std::uint32_t>(in.items.size()));
  detail::put_u32_le(out + 80, static_cast<std::uint32_t>(in.channels.size()));
  unsigned char *dst = out + kHeaderSize;
  for (const FrameItem &src : in.items) {
    detail::put_f64_le(dst + 0, src.x);
    detail::put_f64_le(dst + 8, src.y);
    detail::put_f64_le(dst + 16, src.z);
    detail::put_f64_le(dst + 24, src.vx);
    detail::put_f64_le(dst + 32, src.vy);
    detail::put_f64_le(dst + 40, src.vz);
    detail::put_f64_le(dst + 48, src.size);
    detail::put_u32_le(dst + 56, src.color);
    dst[60] = src.has_velocity ? static_cast<unsigned char>(1) : static_cast<unsigned char>(0);
    dst += kItemSize;
  }
  for (const FrameChannel &src : in.channels) {
    dst[0] = static_cast<unsigned char>(src.name.size());
    for (std::size_t i = 0; i < src.name.size(); ++i) {
      dst[1 + i] = static_cast<unsigned char>(src.name[i]);
    }
    detail::put_f64_le(dst + 1 + src.name.size(), src.value);
    dst += 1 + src.name.size() + sizeof(double);
  }
  out_len = need;
  return core::Status::success();
}

// v2 record bytes -> Frame. Cold reader side: may allocate (vectors/
// strings), so NOT noexcept — never call inside the sim loop. Validates
// the full record BEFORE touching `out` (magic, schema, kind, zero
// reserved, domain shape + optional exact match, count bounds, name
// lengths, has_velocity 0/1, EXACT total length); violations are
// kInvalidArgument with `out`/outs untouched. Non-kFrame kinds report
// through out_kind but decode nothing (their decode entry is future work).
// Round-trip is exactly equal (==), including empty records (0 items +
// 0 channels is valid) and non-finite doubles (bit-exact carrier).
[[nodiscard]] inline core::Status decode_record(const unsigned char *bytes, std::size_t len,
                                                std::string_view want_domain, Frame &out,
                                                std::uint64_t &out_run_id,
                                                std::uint64_t &out_seq,
                                                RecordKind &out_kind) {
  if (bytes == nullptr || len < kHeaderSize) {
    return {core::StatusCode::kInvalidArgument};
  }
  if (detail::get_u32_le(bytes + 0) != kMagic ||
      detail::get_u32_le(bytes + 4) != kSchemaVersion) {
    return {core::StatusCode::kInvalidArgument};
  }
  const RecordKind kind = static_cast<RecordKind>(bytes[8]);
  out_kind = kind;
  if (bytes[9] != 0 || bytes[10] != 0 || bytes[11] != 0) {
    return {core::StatusCode::kInvalidArgument};
  }
  std::size_t domain_len = kDomainSize;
  for (std::size_t i = 0; i < kDomainSize; ++i) {
    if (bytes[44 + i] == '\0') {
      domain_len = i;
      break;
    }
  }
  if (domain_len == 0 || domain_len >= kDomainSize) {
    return {core::StatusCode::kInvalidArgument};
  }
  if (!want_domain.empty() &&
      (want_domain.size() != domain_len ||
       std::string_view(reinterpret_cast<const char *>(bytes + 44), domain_len) !=
           want_domain)) {
    return {core::StatusCode::kInvalidArgument};
  }
  if (kind != RecordKind::kFrame) {
    return {core::StatusCode::kInvalidArgument}; // known kind, no decoder yet
  }
  const std::uint64_t n_items_64 =
      detail::get_u32_le(bytes + 76); // u32 read: no overflow on widen
  const std::uint64_t n_channels_64 = detail::get_u32_le(bytes + 80);
  if (n_items_64 > static_cast<std::uint64_t>(kMaxItems) ||
      n_channels_64 > static_cast<std::uint64_t>(kMaxChannels)) {
    return {core::StatusCode::kInvalidArgument};
  }
  const std::size_t n_items = static_cast<std::size_t>(n_items_64);
  const std::size_t n_channels = static_cast<std::size_t>(n_channels_64);
  // Single validation walk: item block, then every channel length, ending
  // exactly at len. All terms are bounded above, so no overflow.
  std::size_t cursor = kHeaderSize + n_items * kItemSize;
  if (cursor > len) {
    return {core::StatusCode::kInvalidArgument};
  }
  const unsigned char *base = bytes + kHeaderSize + n_items * kItemSize;
  for (std::size_t k = 0; k < n_channels; ++k) {
    if (cursor + 1 > len) {
      return {core::StatusCode::kInvalidArgument};
    }
    const std::size_t name_len = base[0];
    if (name_len > kMaxNameLen || cursor + 1 + name_len + sizeof(double) > len) {
      return {core::StatusCode::kInvalidArgument};
    }
    cursor += 1 + name_len + sizeof(double);
    base += 1 + name_len + sizeof(double);
  }
  if (cursor != len) {
    return {core::StatusCode::kInvalidArgument}; // trailing garbage or short
  }
  // has_velocity discipline: only 0/1 cross the wire.
  for (std::size_t k = 0; k < n_items; ++k) {
    const unsigned char hv = bytes[kHeaderSize + k * kItemSize + 60];
    if (hv != 0 && hv != 1) {
      return {core::StatusCode::kInvalidArgument};
    }
  }
  out.step = detail::get_u64_le(bytes + 28);
  out.time = detail::get_f64_le(bytes + 36);
  out.items.resize(n_items);
  for (std::size_t k = 0; k < n_items; ++k) {
    const unsigned char *src = bytes + kHeaderSize + k * kItemSize;
    FrameItem &dst = out.items[k];
    dst.x = detail::get_f64_le(src + 0);
    dst.y = detail::get_f64_le(src + 8);
    dst.z = detail::get_f64_le(src + 16);
    dst.vx = detail::get_f64_le(src + 24);
    dst.vy = detail::get_f64_le(src + 32);
    dst.vz = detail::get_f64_le(src + 40);
    dst.size = detail::get_f64_le(src + 48);
    dst.color = detail::get_u32_le(src + 56);
    dst.has_velocity = src[60] != 0;
  }
  out.channels.resize(n_channels);
  {
    const unsigned char *src = bytes + kHeaderSize + n_items * kItemSize;
    for (std::size_t k = 0; k < n_channels; ++k) {
      const std::size_t name_len = src[0];
      out.channels[k].name.assign(reinterpret_cast<const char *>(src + 1), name_len);
      out.channels[k].value = detail::get_f64_le(src + 1 + name_len);
      src += 1 + name_len + sizeof(double);
    }
  }
  out_run_id = detail::get_u64_le(bytes + 12);
  out_seq = detail::get_u64_le(bytes + 20);
  return core::Status::success();
}

} // namespace wengine::link::compact
