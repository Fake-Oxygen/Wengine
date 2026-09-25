// Compact socket codec v2 (PHYSI-37): explicit little-endian framing,
// only-used-entries encoding, run identity, record kinds, v1/v2 refusal.
// Golden fixture bytes below are hand-written from the wire spec (never
// produced by the encoder), so encode/decode agree with the document, not
// just with each other. Exact-binary values throughout: round-trips assert
// ==, including empty records and non-finite doubles (bit-exact carrier).

#include "wengine/link/compact_codec.hpp"
#include "wengine/link/protocol.hpp"
#include "wengine/link/socket_publisher.hpp"

#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

using namespace wengine;

namespace {

// Golden record, hand-written per spec: domain "signal",
// run_id 0x0102030405060708, seq 7, step 42, time 0.5, no items,
// channels ("sig.count", 1.0) + ("sig.0.U", 2.5). Total 118 bytes:
// 84 header + (1+9+8) + (1+7+8).
constexpr std::array<unsigned char, 118> kGolden{{
    0x57, 0x4C, 0x56, 0x32, // magic "WLV2"
    0x02, 0x00, 0x00, 0x00, // schema 2
    0x01, // kind = frame
    0x00, 0x00, 0x00, // reserved
    0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, // run_id
    0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // frame_seq 7
    0x2A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // step 42
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xE0, 0x3F, // time 0.5
    0x73, 0x69, 0x67, 0x6E, 0x61, 0x6C, 0x00, // "signal\0"
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // pad to 32
    0x00, 0x00, 0x00, 0x00, // item_count 0
    0x02, 0x00, 0x00, 0x00, // channel_count 2
    0x09, // name_len 9
    0x73, 0x69, 0x67, 0x2E, 0x63, 0x6F, 0x75, 0x6E, 0x74, // "sig.count"
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF0, 0x3F, // 1.0
    0x07, // name_len 7
    0x73, 0x69, 0x67, 0x2E, 0x30, 0x2E, 0x55, // "sig.0.U"
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x40, // 2.5
}};

link::Frame golden_frame() {
  link::Frame f;
  f.step = 42;
  f.time = 0.5;
  f.channels.push_back({"sig.count", 1.0});
  f.channels.push_back({"sig.0.U", 2.5});
  return f;
}

void test_golden_fixture() {
  // Encoder output matches the hand-written bytes exactly.
  std::array<unsigned char, 256> buf{};
  std::size_t len = 0;
  assert(link::compact::encode_record(golden_frame(), "signal", 0x0102030405060708ULL, 7,
                                      buf.data(), buf.size(), len)
             .ok());
  assert(len == kGolden.size());
  assert(link::compact::encoded_size(golden_frame()) == kGolden.size());
  for (std::size_t i = 0; i < len; ++i) {
    assert(buf[i] == kGolden[i]);
  }
  // Decoder reads the hand-written bytes back exactly.
  link::Frame out;
  out.step = 0xDEADBEEF;
  std::uint64_t run_id = 0;
  std::uint64_t seq = 0;
  link::compact::RecordKind kind{};
  assert(link::compact::decode_record(kGolden.data(), kGolden.size(), "signal", out,
                                      run_id, seq, kind)
             .ok());
  assert(out == golden_frame());
  assert(run_id == 0x0102030405060708ULL);
  assert(seq == 7);
  assert(kind == link::compact::RecordKind::kFrame);
}

void test_empty_record() {
  // 0 items + 0 channels is valid (S5 parity): header only, every tick
  // observable even with nothing to show.
  link::Frame f;
  f.step = 9;
  f.time = 1.25;
  assert(link::compact::encoded_size(f) == link::compact::kHeaderSize);
  std::array<unsigned char, link::compact::kHeaderSize> buf{};
  std::size_t len = 0;
  assert(link::compact::encode_record(f, "projectile", 1, 2, buf.data(), buf.size(), len)
             .ok());
  assert(len == link::compact::kHeaderSize);
  link::Frame out;
  std::uint64_t run_id = 0;
  std::uint64_t seq = 0;
  link::compact::RecordKind kind{};
  assert(link::compact::decode_record(buf.data(), len, "", out, run_id, seq, kind).ok());
  assert(out == f && run_id == 1 && seq == 2);
}

void test_one_signal_size() {
  // The task's headline number: one signal source is hundreds of bytes,
  // not 68 KB. Golden shape above is 118 bytes (577x smaller than a TCP
  // full frame); bound the general one-source shape well below a KiB.
  assert(kGolden.size() == 118);
  static_assert(sizeof(link::LinkFrame) == 68136);
  assert(kGolden.size() * 500 < sizeof(link::LinkFrame));
  link::Frame f;
  f.channels.push_back({"sig.count", 1.0});
  f.channels.push_back({"sig.123456789.I", -0.0});
  assert(link::compact::encoded_size(f) <= 256);
  // Codec bound stays tied to the max UDP payload (one codec, both socket
  // transports): every encodable record fits a maximum-size datagram.
  assert(link::compact::kMaxRecordBytes == link::kMaxUdpPayload);
}

void test_round_trip_sweep() {
  // Bit-tricky values, per-item velocity flags, colors, long names,
  // non-finite doubles (bit-exact carrier, compared as bits for NaN).
  link::Frame f;
  f.step = 0xFFFFFFFFFFFFFFFFULL;
  f.time = -0.0;
  link::FrameItem a;
  a.x = 0.1 + 0.2; // inexact binary: proves bitwise fidelity
  a.y = -1.5;
  a.z = std::numeric_limits<double>::infinity();
  a.vx = 0.0;
  a.vy = 0.5;
  a.vz = -2.5;
  a.has_velocity = false;
  a.size = 2.0;
  a.color = 7;
  f.items.push_back(a);
  link::FrameItem b; // all defaults incl. has_velocity=true
  f.items.push_back(b);
  f.channels.push_back({"node_A", 3.375});
  f.channels.push_back({"energy", 0.1 + 0.2});
  f.channels.push_back({std::string(31, 'n'), -std::numeric_limits<double>::infinity()});
  f.channels.push_back({"", 0.0}); // empty names match v1 acceptance
  std::array<unsigned char, 4096> buf{};
  std::size_t len = 0;
  const std::uint64_t kRun = 0xFEDCBA9876543210ULL;
  assert(link::compact::encode_record(f, "signal", kRun, 123, buf.data(), buf.size(), len)
             .ok());
  link::Frame out;
  std::uint64_t run_id = 0;
  std::uint64_t seq = 0;
  link::compact::RecordKind kind{};
  assert(link::compact::decode_record(buf.data(), len, "signal", out, run_id, seq, kind)
             .ok());
  assert(out == f && run_id == kRun && seq == 123);
  // NaN payload bits survive untouched (never canonicalized).
  link::Frame nan_frame;
  const std::uint64_t kNanBits = 0x7FF8123456789ABCULL;
  double nan_value = 0.0;
  std::memcpy(&nan_value, &kNanBits, sizeof(nan_value));
  assert(std::isnan(nan_value));
  nan_frame.channels.push_back({"q", nan_value});
  assert(link::compact::encode_record(nan_frame, "signal", 0, 0, buf.data(), buf.size(), len)
             .ok());
  link::Frame nan_out;
  assert(link::compact::decode_record(buf.data(), len, "", nan_out, run_id, seq, kind)
             .ok());
  std::uint64_t got_bits = 0;
  std::memcpy(&got_bits, &nan_out.channels[0].value, sizeof(got_bits));
  assert(got_bits == kNanBits);
}

void test_max_shape() {
  // Maximum-shape record still fits a maximum-size datagram and
  // round-trips exactly: 84 + 1024x61 + 64x(1+31+8) = 65,108 bytes.
  link::Frame f;
  for (std::uint64_t k = 0; k < link::kMaxItems; ++k) {
    link::FrameItem it;
    it.x = static_cast<double>(k) * 0.125;
    it.color = static_cast<std::uint32_t>(k & 1u);
    it.has_velocity = (k & 1u) == 0;
    f.items.push_back(it);
  }
  for (std::uint64_t k = 0; k < link::kMaxChannels; ++k) {
    f.channels.push_back({std::string(31, static_cast<char>('a' + (k % 26))),
                          static_cast<double>(k)});
  }
  assert(link::compact::encoded_size(f) == 65108);
  assert(link::compact::encoded_size(f) <= link::compact::kMaxRecordBytes);
  std::vector<unsigned char> buf(link::compact::kMaxRecordBytes);
  std::size_t len = 0;
  assert(link::compact::encode_record(f, "signal", 9, 10, buf.data(), buf.size(), len)
             .ok());
  assert(len == 65108);
  link::Frame out;
  std::uint64_t run_id = 0;
  std::uint64_t seq = 0;
  link::compact::RecordKind kind{};
  assert(link::compact::decode_record(buf.data(), len, "signal", out, run_id, seq, kind)
             .ok());
  assert(out == f && run_id == 9 && seq == 10);
}

void test_over_bound() {
  // Over-bound input is kOutOfRange with output untouched (S7: loud,
  // never silently truncated). Canary-fill the buffer first.
  std::array<unsigned char, 256> buf;
  buf.fill(0xA5);
  std::size_t len = 0xDEADBEEF;
  const auto untouched = [&] {
    if (len != 0xDEADBEEF) {
      return false;
    }
    for (unsigned char b : buf) {
      if (b != 0xA5) {
        return false;
      }
    }
    return true;
  };
  {
    link::Frame f = golden_frame();
    for (std::size_t k = 0; k <= link::kMaxItems; ++k) {
      f.items.push_back(link::FrameItem{});
    }
    assert(link::compact::encoded_size(f) == 0);
    assert(!link::compact::encode_record(f, "signal", 0, 0, buf.data(), buf.size(), len)
                .ok());
    assert(untouched());
  }
  {
    link::Frame f = golden_frame();
    for (std::size_t k = 0; k <= link::kMaxChannels; ++k) {
      f.channels.push_back({"c", 0.0});
    }
    assert(link::compact::encoded_size(f) == 0);
    assert(!link::compact::encode_record(f, "signal", 0, 0, buf.data(), buf.size(), len)
                .ok());
    assert(untouched());
  }
  {
    link::Frame f;
    f.channels.push_back({std::string(32, 'x'), 0.0}); // 32 > 31
    assert(link::compact::encoded_size(f) == 0);
    assert(!link::compact::encode_record(f, "signal", 0, 0, buf.data(), buf.size(), len)
                .ok());
    assert(untouched());
  }
  // Capacity too small, empty domain, null buffer.
  assert(!link::compact::encode_record(golden_frame(), "signal", 0, 0, buf.data(),
                                       kGolden.size() - 1, len)
              .ok());
  assert(untouched());
  assert(!link::compact::encode_record(golden_frame(), "", 0, 0, buf.data(), buf.size(), len)
              .ok());
  assert(!link::compact::encode_record(golden_frame(), std::string(32, 'd'), 0, 0,
                                       buf.data(), buf.size(), len)
              .ok());
  assert(!link::compact::encode_record(golden_frame(), "signal", 0, 0, nullptr, 999, len)
              .ok());
  assert(untouched());
}

void test_malformed() {
  // Every structural violation refuses loudly with `out` untouched.
  // Sentinel frame: any mutation is caught by equality.
  link::Frame sentinel;
  sentinel.step = 0xBEEF;
  sentinel.time = 1.5;
  sentinel.items.push_back(link::FrameItem{});
  sentinel.channels.push_back({"s", 2.5});
  auto refuses = [&](std::vector<unsigned char> bytes, std::string_view domain = "signal") {
    link::Frame out = sentinel;
    std::uint64_t run_id = 0xBEEF;
    std::uint64_t seq = 0xBEEF;
    link::compact::RecordKind kind{};
    const bool ok = link::compact::decode_record(bytes.data(), bytes.size(), domain, out,
                                                 run_id, seq, kind)
                        .ok();
    assert(!ok);
    assert(out == sentinel);
    assert(run_id == 0xBEEF && seq == 0xBEEF);
  };
  const std::vector<unsigned char> good(kGolden.begin(), kGolden.end());
  refuses({}); // empty
  refuses(std::vector<unsigned char>(good.begin(), good.begin() + 83)); // truncated header
  refuses(std::vector<unsigned char>(good.begin(), good.begin() + 117)); // truncated tail
  {
    auto b = good;
    b.push_back(0x00); // trailing garbage
    refuses(b);
  }
  for (std::size_t i : {0, 1, 2, 3}) { // magic
    auto b = good;
    b[i] ^= 0xFF;
    refuses(b);
  }
  {
    auto b = good; // schema 3 (future): refused, not guessed
    b[4] = 0x03;
    refuses(b);
  }
  for (std::uint8_t k : {0x00, 0x02, 0xFF}) { // non-frame kinds
    auto b = good;
    b[8] = k;
    refuses(b);
  }
  for (std::size_t i : {9, 10, 11}) { // nonzero reserved
    auto b = good;
    b[i] = 0x01;
    refuses(b);
  }
  {
    auto b = good; // empty domain (NUL at first byte)
    b[44] = 0x00;
    refuses(b);
  }
  {
    auto b = good; // unterminated domain
    for (std::size_t i = 44; i < 76; ++i) {
      b[i] = 0x78;
    }
    refuses(b);
  }
  refuses(good, "projectile"); // domain mismatch
  {
    auto b = good; // item_count beyond bound
    b[76] = 0x01;
    b[77] = 0x04;
    refuses(b);
  }
  {
    auto b = good; // channel_count beyond bound
    b[80] = 0x41;
    refuses(b);
  }
  {
    auto b = good; // channel_count lies vs length
    b[80] = 0x03;
    refuses(b);
  }
  {
    auto b = good; // name_len beyond bound
    b[84] = 0x20;
    refuses(b);
  }
  {
    // has_velocity = 7 on a real item: wire discipline is 0/1 only.
    std::array<unsigned char, 512> buf{};
    std::size_t len = 0;
    link::Frame f;
    link::FrameItem it;
    it.has_velocity = true;
    f.items.push_back(it);
    f.channels.push_back({"c", 1.0});
    assert(link::compact::encode_record(f, "signal", 0, 0, buf.data(), buf.size(), len)
               .ok());
    std::vector<unsigned char> bad(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(len));
    bad[link::compact::kHeaderSize + 60] = 7;
    refuses(bad);
  }
  // Reserved kind reports through out_kind while decoding nothing.
  {
    auto b = good;
    b[8] = 0x02;
    link::Frame out = sentinel;
    std::uint64_t run_id = 0;
    std::uint64_t seq = 0;
    link::compact::RecordKind kind{};
    assert(!link::compact::decode_record(b.data(), b.size(), "signal", out, run_id, seq, kind)
                .ok());
    assert(static_cast<std::uint8_t>(kind) == 0x02);
    assert(out == sentinel);
  }
}

static_assert(link::compact::kV1UdpMagic == link::kUdpMagic,
              "v1 refusal magic must match the UDP publisher header");
static_assert(link::compact::kMaxNameLen == link::kChannelNameSize - 1);

void test_v1_v2_refusal() {
  // Families classify exactly; gates refuse everything foreign loudly.
  // A genuine v1 UDP datagram (built by the v1 encoder, not hand bytes)
  // classifies V1 and is refused by the v2 gate.
  link::Frame f = golden_frame();
  std::array<unsigned char, link::kMaxUdpPayload> v1{};
  std::size_t v1_len = 0;
  assert(link::encode_udp(f, "signal", 7, v1.data(), v1.size(), v1_len).ok());
  assert(link::compact::classify_record(v1.data(), v1_len) == link::compact::SocketSchema::kV1);
  assert(!link::compact::check_v2_schema(v1.data(), v1_len).ok());
  link::Frame out;
  std::uint64_t run_id = 0;
  std::uint64_t seq = 0;
  link::compact::RecordKind kind{};
  assert(!link::compact::decode_record(v1.data(), v1_len, "signal", out, run_id, seq, kind)
              .ok());

  assert(link::compact::classify_record(kGolden.data(), kGolden.size()) ==
         link::compact::SocketSchema::kV2);
  assert(link::compact::check_v2_schema(kGolden.data(), kGolden.size()).ok());

  const unsigned char garbage[8] = {'n', 'o', 'p', 'e', 0, 0, 0, 0};
  assert(link::compact::classify_record(garbage, sizeof(garbage)) ==
         link::compact::SocketSchema::kUnknown);
  assert(link::compact::classify_record(nullptr, 0) == link::compact::SocketSchema::kUnknown);
  assert(link::compact::classify_record(kGolden.data(), 3) ==
         link::compact::SocketSchema::kUnknown); // short prefix
  assert(!link::compact::check_v2_schema(nullptr, 0).ok());
  {
    // "WLV2" magic with a future schema refuses as unknown, never guessed.
    std::array<unsigned char, link::compact::kHeaderSize> b{};
    b[0] = 0x57;
    b[1] = 0x4C;
    b[2] = 0x56;
    b[3] = 0x32;
    b[4] = 0x03;
    assert(link::compact::classify_record(b.data(), b.size()) ==
           link::compact::SocketSchema::kUnknown);
    assert(!link::compact::check_v2_schema(b.data(), b.size()).ok());
  }
  {
    // v1 TCP hello magic is not a record of either schema.
    const unsigned char hello[4] = {0x57, 0x4C, 0x54, 0x31}; // "WLT1"
    assert(link::compact::classify_record(hello, sizeof(hello)) ==
           link::compact::SocketSchema::kUnknown);
  }
}

} // namespace

int main() {
  test_golden_fixture();
  test_empty_record();
  test_one_signal_size();
  test_round_trip_sweep();
  test_max_shape();
  test_over_bound();
  test_malformed();
  test_v1_v2_refusal();
  std::cout << "test_link_compact: OK\n";
  return 0;
}
