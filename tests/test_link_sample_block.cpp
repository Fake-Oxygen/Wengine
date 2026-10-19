// SampleBlock builder + codec (PHYSI-44): batched consecutive signal ticks
// without decimating values. Golden bytes are hand-written from the wire
// spec (never produced by the encoder). Round-trips assert == throughout,
// including empty selections, schema changes, partial blocks and >63
// channels. Exact-binary values: NaN payloads preserved bit-exact.

#include "wengine/link/compact_codec.hpp"
#include "wengine/link/sample_block.hpp"

#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

using namespace wengine;

namespace {

world::Snapshot make_snapshot(std::uint64_t step, double time,
                              const std::vector<std::uint64_t> &serials,
                              const std::vector<std::uint8_t> &quantities,
                              const std::vector<double> &values) {
  world::Snapshot s;
  s.step_index = step;
  s.sim_time = time;
  s.sig_serial = serials;
  s.sig_quantity = quantities;
  s.sig_value = values;
  return s;
}

// Golden block, hand-written per spec: domain "signal", run_id
// 0x0102030405060708, block_seq 7, schema [(serial 9, qty 0),
// (serial 10, qty 1)], samples [(step 42, time 0.5, [2.5, -1.25]),
// (step 43, time 0.75, [3.5, 0.5])].
// Total: 68 header + 2x12 schema + 2x(16+16) values = 156 bytes.
constexpr std::array<unsigned char, 156> kGolden{{
    0x57, 0x4C, 0x56, 0x32, // magic "WLV2"
    0x03, 0x00, 0x00, 0x00, // schema 3
    0x02, // kind = sample block
    0x00, 0x00, 0x00, // reserved
    0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, // run_id
    0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // block_seq 7
    0x73, 0x69, 0x67, 0x6E, 0x61, 0x6C, 0x00, // "signal\0"
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // pad
    0x02, 0x00, 0x00, 0x00, // channel_count 2
    0x02, 0x00, 0x00, 0x00, // sample_count 2
    0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // serial 9
    0x00, 0x00, 0x00, 0x00, // qty 0 + reserved
    0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // serial 10
    0x01, 0x00, 0x00, 0x00, // qty 1 + reserved
    0x2A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // step 42
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xE0, 0x3F, // time 0.5
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x40, // 2.5
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF4, 0xBF, // -1.25
    0x2B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // step 43
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xE8, 0x3F, // time 0.75
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x40, // 3.5
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xE0, 0x3F, // 0.5
}};

link::sample::SampleBlock golden_block() {
  link::sample::SampleBlock b;
  b.serials = {9, 10};
  b.quantities = {0, 1};
  b.steps = {42, 43};
  b.times = {0.5, 0.75};
  b.values = {2.5, -1.25, 3.5, 0.5};
  return b;
}

void test_golden_fixture() {
  std::array<unsigned char, 256> buf{};
  std::size_t len = 0;
  assert(link::sample::encode_block(golden_block(), "signal", 0x0102030405060708ULL, 7,
                                    buf.data(), buf.size(), len)
             .ok());
  assert(len == kGolden.size());
  assert(link::sample::encoded_size(golden_block()) == kGolden.size());
  for (std::size_t i = 0; i < len; ++i) {
    assert(buf[i] == kGolden[i]);
  }
  link::sample::SampleBlock out;
  std::uint64_t run_id = 0;
  std::uint64_t seq = 0;
  link::compact::RecordKind kind{};
  assert(link::sample::decode_block(kGolden.data(), kGolden.size(), "signal", out,
                                    run_id, seq, kind)
             .ok());
  assert(out == golden_block());
  assert(run_id == 0x0102030405060708ULL);
  assert(seq == 7);
  assert(kind == link::compact::RecordKind::kSampleBlock);
}

void test_empty_selection_ticks() {
  // Zero channels still records every tick (step/time preserved, no values).
  link::sample::SampleBlockBuilder builder;
  for (std::uint64_t step = 0; step < 5; ++step) {
    const world::Snapshot s = make_snapshot(step, 0.25 * static_cast<double>(step),
                                            {}, {}, {});
    link::sample::SampleBlock sealed;
    bool has_sealed = true;
    assert(builder.push(s, sealed, has_sealed).ok());
    assert(!has_sealed);
  }
  assert(builder.pending_samples() == 5);
  assert(builder.channel_count() == 0);
  link::sample::SampleBlock block;
  bool has_block = false;
  assert(builder.flush(block, has_block).ok());
  assert(has_block);
  assert(block.channel_count() == 0);
  assert(block.sample_count() == 5);
  assert(block.values.empty());
  assert(link::sample::encoded_size(block) ==
         link::sample::kBlockHeaderSize + 5 * link::sample::kSampleOverhead);
  std::array<unsigned char, 1024> buf{};
  std::size_t len = 0;
  assert(link::sample::encode_block(block, "signal", 1, 2, buf.data(), buf.size(), len)
             .ok());
  link::sample::SampleBlock out;
  std::uint64_t run_id = 0;
  std::uint64_t seq = 0;
  link::compact::RecordKind kind{};
  assert(
      link::sample::decode_block(buf.data(), len, "signal", out, run_id, seq, kind)
          .ok());
  assert(out == block);
  assert(run_id == 1 && seq == 2);
  for (std::size_t k = 0; k < 5; ++k) {
    assert(out.steps[k] == k);
    assert(out.times[k] == 0.25 * static_cast<double>(k));
  }
}

void test_schema_change_seals() {
  // Source add seals the running block; drop seals again; values stay exact.
  link::sample::SampleBlockConfig cfg;
  cfg.max_samples = 64;
  link::sample::SampleBlockBuilder builder(cfg);
  const std::vector<std::uint64_t> serials_a{0};
  const std::vector<std::uint8_t> qty_a{0};
  const std::vector<std::uint64_t> serials_ab{0, 1};
  const std::vector<std::uint8_t> qty_ab{0, 0};
  const std::vector<std::uint64_t> serials_b{1};
  const std::vector<std::uint8_t> qty_b{0};

  link::sample::SampleBlock sealed;
  bool has_sealed = true;
  const world::Snapshot s0 = make_snapshot(0, 0.0, serials_a, qty_a, {1.0});
  assert(builder.push(s0, sealed, has_sealed).ok());
  assert(!has_sealed);
  const world::Snapshot s1 = make_snapshot(1, 0.1, serials_a, qty_a, {2.0});
  assert(builder.push(s1, sealed, has_sealed).ok());
  assert(!has_sealed);
  // Add source 1: seals [s0, s1], starts new schema.
  const world::Snapshot s2 = make_snapshot(2, 0.2, serials_ab, qty_ab, {3.0, 30.0});
  assert(builder.push(s2, sealed, has_sealed).ok());
  assert(has_sealed);
  assert(sealed.sample_count() == 2);
  const std::vector<std::uint64_t> want_serials_a{0};
  assert(sealed.serials == want_serials_a);
  const std::vector<double> want_values_a{1.0, 2.0};
  assert(sealed.values == want_values_a);
  // Drop source 0: seals [s2], starts schema [1].
  const world::Snapshot s3 = make_snapshot(3, 0.3, serials_b, qty_b, {40.0});
  assert(builder.push(s3, sealed, has_sealed).ok());
  assert(has_sealed);
  assert(sealed.sample_count() == 1);
  const std::vector<std::uint64_t> want_serials_ab{0, 1};
  assert(sealed.serials == want_serials_ab);
  link::sample::SampleBlock tail;
  bool has_tail = false;
  assert(builder.flush(tail, has_tail).ok());
  assert(has_tail);
  assert(tail.sample_count() == 1);
  const std::vector<std::uint64_t> want_serials_b{1};
  assert(tail.serials == want_serials_b);
  const std::vector<double> want_tail{40.0};
  assert(tail.values == want_tail);
}

void test_partial_blocks_and_count_seal() {
  // max_samples=3 over 7 ticks yields 3+3+1; the short final flush is explicit.
  link::sample::SampleBlockConfig cfg;
  cfg.max_samples = 3;
  link::sample::SampleBlockBuilder builder(cfg);
  const std::vector<std::uint64_t> serials{5};
  const std::vector<std::uint8_t> qty{1};
  std::vector<link::sample::SampleBlock> sealed;
  for (std::uint64_t step = 0; step < 7; ++step) {
    const double t = 0.01 * static_cast<double>(step);
    const world::Snapshot s =
        make_snapshot(step, t, serials, qty, {100.0 + static_cast<double>(step)});
    link::sample::SampleBlock out;
    bool has = false;
    assert(builder.push(s, out, has).ok());
    if (has) {
      assert(out.sample_count() == 3);
      sealed.push_back(out);
    }
  }
  assert(sealed.size() == 2);
  link::sample::SampleBlock tail;
  bool has_tail = false;
  assert(builder.flush(tail, has_tail).ok());
  assert(has_tail);
  assert(tail.sample_count() == 1);
  sealed.push_back(tail);
  // Flattened steps/times/values match the pushed stream exactly.
  std::vector<std::uint64_t> steps;
  std::vector<double> values;
  for (const auto &b : sealed) {
    for (std::size_t k = 0; k < b.sample_count(); ++k) {
      steps.push_back(b.steps[k]);
      values.push_back(b.values[k * b.channel_count()]);
    }
  }
  assert(steps.size() == 7);
  for (std::size_t k = 0; k < 7; ++k) {
    assert(steps[k] == k);
    assert(values[k] == 100.0 + static_cast<double>(k));
  }
  // Flushing an empty builder produces no block (no zero-sample emission).
  link::sample::SampleBlock empty;
  bool has_empty = true;
  assert(builder.flush(empty, has_empty).ok());
  assert(!has_empty);
}

void test_flatten_batch_stream_equality() {
  // 200 ticks with accumulated (non-uniform) timestamps, a mid-stream
  // add/drop, small blocks: flatten(decode(batch(stream))) == stream.
  link::sample::SampleBlockConfig cfg;
  cfg.max_samples = 16;
  link::sample::SampleBlockBuilder builder(cfg);
  std::vector<world::Snapshot> stream;
  double t = 0.0;
  for (std::uint64_t step = 0; step < 200; ++step) {
    t += 0.001 * (1.0 + static_cast<double>(step % 7) * 0.125);
    if (step < 80) {
      const std::vector<std::uint64_t> serials{0, 1};
      const std::vector<std::uint8_t> qty{0, 1};
      stream.push_back(make_snapshot(step, t, serials, qty,
                                     {std::sin(t), std::cos(t)}));
    } else if (step < 140) {
      const std::vector<std::uint64_t> serials{0, 1, 2};
      const std::vector<std::uint8_t> qty{0, 1, 0};
      stream.push_back(make_snapshot(step, t, serials, qty,
                                     {std::sin(t), std::cos(t), std::sin(2.0 * t)}));
    } else {
      const std::vector<std::uint64_t> serials{1, 2};
      const std::vector<std::uint8_t> qty{1, 0};
      stream.push_back(make_snapshot(step, t, serials, qty,
                                     {std::cos(t), std::sin(2.0 * t)}));
    }
  }
  std::vector<link::sample::SampleBlock> blocks;
  for (const auto &s : stream) {
    link::sample::SampleBlock sealed;
    bool has = false;
    assert(builder.push(s, sealed, has).ok());
    if (has) {
      blocks.push_back(sealed);
    }
  }
  {
    link::sample::SampleBlock tail;
    bool has = false;
    assert(builder.flush(tail, has).ok());
    if (has) {
      blocks.push_back(tail);
    }
  }
  // Every block round-trips through the wire codec exactly, then flattens
  // back to the original snapshot stream (step, time, schema, values).
  std::vector<world::Snapshot> flat;
  std::uint64_t seq = 0;
  for (const auto &b : blocks) {
    std::vector<unsigned char> buf(link::sample::kBlockMaxBytes);
    std::size_t len = 0;
    assert(link::sample::encode_block(b, "signal", 99, seq, buf.data(), buf.size(), len)
               .ok());
    link::sample::SampleBlock out;
    std::uint64_t run_id = 0;
    std::uint64_t out_seq = 0;
    link::compact::RecordKind kind{};
    assert(link::sample::decode_block(buf.data(), len, "signal", out, run_id, out_seq,
                                      kind)
               .ok());
    assert(out == b);
    assert(run_id == 99 && out_seq == seq);
    ++seq;
    for (std::size_t k = 0; k < out.sample_count(); ++k) {
      world::Snapshot s;
      s.step_index = out.steps[k];
      s.sim_time = out.times[k];
      s.sig_serial = out.serials;
      s.sig_quantity = out.quantities;
      s.sig_value.resize(out.channel_count());
      for (std::size_t c = 0; c < out.channel_count(); ++c) {
        s.sig_value[c] = out.values[k * out.channel_count() + c];
      }
      flat.push_back(s);
    }
  }
  assert(flat.size() == stream.size());
  for (std::size_t k = 0; k < stream.size(); ++k) {
    assert(flat[k] == stream[k]);
  }
}

void test_above_linkframe_capacity() {
  // 100 sources exceed the 64-channel LinkFrame cap but fit one block.
  constexpr std::size_t kSources = 100;
  std::vector<std::uint64_t> serials;
  std::vector<std::uint8_t> qty;
  std::vector<double> values;
  for (std::size_t k = 0; k < kSources; ++k) {
    serials.push_back(1000 + k);
    qty.push_back(static_cast<std::uint8_t>(k % 2));
    values.push_back(0.5 * static_cast<double>(k));
  }
  const world::Snapshot s = make_snapshot(7, 1.5, serials, qty, values);
  link::sample::SampleBlockBuilder builder;
  link::sample::SampleBlock sealed;
  bool has_sealed = true;
  assert(builder.push(s, sealed, has_sealed).ok());
  assert(!has_sealed);
  link::sample::SampleBlock block;
  bool has_block = false;
  assert(builder.flush(block, has_block).ok());
  assert(has_block);
  assert(block.channel_count() == kSources);
  const std::size_t need = link::sample::encoded_size(block);
  assert(need != 0 && need <= link::sample::kBlockMaxBytes);
  // Equivalent per-step frame traffic would need two LinkFrames (64 cap);
  // one block carries all 100 with schema stored once.
  assert(kSources > link::kMaxChannels);
  std::vector<unsigned char> buf(link::sample::kBlockMaxBytes);
  std::size_t len = 0;
  assert(
      link::sample::encode_block(block, "signal", 3, 4, buf.data(), buf.size(), len)
          .ok());
  assert(len == need);
  link::sample::SampleBlock out;
  std::uint64_t run_id = 0;
  std::uint64_t seq = 0;
  link::compact::RecordKind kind{};
  assert(link::sample::decode_block(buf.data(), len, "signal", out, run_id, seq, kind)
             .ok());
  assert(out == block);
  // ByteDeco: schema once (12 B/source) + 16 B step/time + 8 B/value.
  const std::size_t expect = link::sample::kBlockHeaderSize +
                             kSources * link::sample::kChannelEntrySize +
                             (link::sample::kSampleOverhead + kSources * 8);
  assert(len == expect);
}

void test_over_bound() {
  std::array<unsigned char, 256> buf;
  buf.fill(0xA5);
  std::size_t len = 0xDEADBEEF;
  auto untouched = [&] {
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
  // Too many channels for the builder: loud, builder unmutated.
  {
    link::sample::SampleBlockBuilder builder;
    world::Snapshot s;
    s.step_index = 0;
    s.sim_time = 0.0;
    s.sig_serial.resize(link::sample::kBlockChannelsMax + 1, 0);
    s.sig_quantity.resize(link::sample::kBlockChannelsMax + 1, 0);
    s.sig_value.resize(link::sample::kBlockChannelsMax + 1, 0.0);
    link::sample::SampleBlock sealed;
    bool has = true;
    assert(!builder.push(s, sealed, has).ok());
    assert(!builder.has_schema() && builder.pending_samples() == 0);
  }
  // Unknown quantity code refused at push and at encode.
  {
    link::sample::SampleBlockBuilder builder;
    const std::vector<std::uint64_t> serials{0};
    const std::vector<std::uint8_t> qty{99};
    const world::Snapshot s = make_snapshot(0, 0.0, serials, qty, {1.0});
    link::sample::SampleBlock sealed;
    bool has = true;
    assert(!builder.push(s, sealed, has).ok());
    link::sample::SampleBlock bad = golden_block();
    bad.quantities[0] = 99;
    assert(link::sample::encoded_size(bad) != 0); // size is structural
    assert(!link::sample::encode_block(bad, "signal", 0, 0, buf.data(), buf.size(), len)
                .ok());
    assert(untouched());
  }
  // Tiny byte budget: first row fits nothing after sealing — loud OutOfRange.
  {
    link::sample::SampleBlockConfig cfg;
    cfg.max_samples = 64;
    cfg.max_bytes = 10;
    link::sample::SampleBlockBuilder builder(cfg);
    const std::vector<std::uint64_t> serials{0};
    const std::vector<std::uint8_t> qty{0};
    const world::Snapshot s = make_snapshot(0, 0.0, serials, qty, {1.0});
    link::sample::SampleBlock sealed;
    bool has = true;
    const core::Status st = builder.push(s, sealed, has);
    assert(!st.ok() && st.code == core::StatusCode::kOutOfRange);
  }
  // Encode capacity too small / null / bad domain: untouched output.
  {
    assert(!link::sample::encode_block(golden_block(), "signal", 0, 0, buf.data(),
                                       kGolden.size() - 1, len)
                .ok());
    assert(untouched());
    assert(!link::sample::encode_block(golden_block(), "", 0, 0, buf.data(),
                                       buf.size(), len)
                .ok());
    assert(!link::sample::encode_block(golden_block(), std::string(32, 'd'), 0, 0,
                                       buf.data(), buf.size(), len)
                .ok());
    assert(!link::sample::encode_block(golden_block(), "signal", 0, 0, nullptr, 999,
                                       len)
                .ok());
    assert(untouched());
  }
}

void test_malformed() {
  link::sample::SampleBlock sentinel;
  sentinel.serials = {1};
  sentinel.quantities = {0};
  sentinel.steps = {9};
  sentinel.times = {1.5};
  sentinel.values = {2.5};
  auto refuses = [&](std::vector<unsigned char> bytes,
                     std::string_view domain = "signal") {
    link::sample::SampleBlock out = sentinel;
    std::uint64_t run_id = 0xBEEF;
    std::uint64_t seq = 0xBEEF;
    link::compact::RecordKind kind{};
    const bool ok =
        link::sample::decode_block(bytes.data(), bytes.size(), domain, out, run_id,
                                   seq, kind)
            .ok();
    assert(!ok);
    assert(out == sentinel);
    assert(run_id == 0xBEEF && seq == 0xBEEF);
  };
  const std::vector<unsigned char> good(kGolden.begin(), kGolden.end());
  refuses({});
  refuses(std::vector<unsigned char>(good.begin(), good.begin() + 67));
  refuses(std::vector<unsigned char>(good.begin(), good.begin() + 155));
  {
    auto b = good;
    b.push_back(0x00);
    refuses(b);
  }
  for (std::size_t i : {0, 1, 2, 3}) {
    auto b = good;
    b[i] ^= 0xFF;
    refuses(b);
  }
  {
    auto b = good;
    b[4] = 0x04;
    refuses(b);
  }
  for (std::uint8_t k : {0x00, 0x01, 0x03, 0xFF}) {
    auto b = good;
    b[8] = k;
    refuses(b);
  }
  for (std::size_t i : {9, 10, 11}) {
    auto b = good;
    b[i] = 0x01;
    refuses(b);
  }
  {
    auto b = good;
    b[28] = 0x00;
    refuses(b);
  }
  {
    auto b = good;
    for (std::size_t i = 28; i < 60; ++i) {
      b[i] = 0x78;
    }
    refuses(b);
  }
  refuses(good, "projectile");
  {
    auto b = good; // channel_count lies vs length
    b[60] = 0x03;
    refuses(b);
  }
  {
    auto b = good; // sample_count lies vs length
    b[64] = 0x03;
    refuses(b);
  }
  {
    auto b = good; // nonzero channel reserved (both entries)
    b[68 + 9] = 0x01;
    refuses(b);
  }
  {
    auto b = good; // nonzero channel reserved (second entry)
    b[68 + 12 + 9] = 0x01;
    refuses(b);
  }
  {
    auto b = good; // unknown quantity code
    b[68 + 8] = 0x7F;
    refuses(b);
  }
  // Frame bytes are not blocks (and blocks are not frames): loud kind report.
  {
    link::Frame f;
    f.step = 1;
    f.time = 2.0;
    f.channels.push_back({"sig.count", 1.0});
    std::array<unsigned char, 1024> fbuf{};
    std::size_t flen = 0;
    assert(link::compact::encode_record(f, "signal", 5, 6, fbuf.data(), fbuf.size(),
                                        flen)
               .ok());
    link::sample::SampleBlock out = sentinel;
    std::uint64_t run_id = 0;
    std::uint64_t seq = 0;
    link::compact::RecordKind kind{};
    assert(!link::sample::decode_block(fbuf.data(), flen, "signal", out, run_id, seq,
                                       kind)
                .ok());
    assert(kind == link::compact::RecordKind::kFrame);
    assert(out == sentinel);
    link::Frame fout;
    link::compact::RecordKind fkind{};
    assert(!link::compact::decode_record(good.data(), good.size(), "signal", fout,
                                         run_id, seq, fkind)
                .ok());
    assert(fkind == link::compact::RecordKind::kSampleBlock);
  }
  // Non-finite values are bit-exact carriers (incl. NaN payloads).
  {
    link::sample::SampleBlock nan_block;
    nan_block.serials = {0};
    nan_block.quantities = {0};
    nan_block.steps = {1};
    nan_block.times = {std::numeric_limits<double>::infinity()};
    const std::uint64_t kNanBits = 0x7FF8123456789ABCULL;
    double nan_value = 0.0;
    std::memcpy(&nan_value, &kNanBits, sizeof(nan_value));
    assert(std::isnan(nan_value));
    nan_block.values = {nan_value};
    std::array<unsigned char, 512> nbuf{};
    std::size_t nlen = 0;
    assert(link::sample::encode_block(nan_block, "signal", 0, 0, nbuf.data(),
                                      nbuf.size(), nlen)
               .ok());
    link::sample::SampleBlock nout;
    std::uint64_t run_id = 0;
    std::uint64_t seq = 0;
    link::compact::RecordKind kind{};
    assert(link::sample::decode_block(nbuf.data(), nlen, "", nout, run_id, seq, kind)
               .ok());
    // NaN != NaN, so == cannot hold for the payload itself: compare the
    // schema/clocks exactly and the value by bits (same as test_link_compact).
    assert(nout.serials == nan_block.serials);
    assert(nout.quantities == nan_block.quantities);
    assert(nout.steps == nan_block.steps);
    assert(nout.times == nan_block.times);
    assert(nout.values.size() == 1);
    std::uint64_t got_bits = 0;
    std::memcpy(&got_bits, &nout.values[0], sizeof(got_bits));
    assert(got_bits == kNanBits);
  }
}

void test_byte_budget_seal() {
  // Byte limit seals before the count limit: 1-channel ticks at max_bytes
  // sized for exactly 4 samples seal 4+4+2 over 10 pushes.
  const std::size_t channels = 1;
  const std::size_t want_samples = 4;
  const std::size_t budget = link::sample::kBlockHeaderSize +
                             channels * link::sample::kChannelEntrySize +
                             want_samples * (link::sample::kSampleOverhead +
                                             channels * sizeof(double));
  link::sample::SampleBlockConfig cfg;
  cfg.max_samples = 64;
  cfg.max_bytes = budget;
  link::sample::SampleBlockBuilder builder(cfg);
  const std::vector<std::uint64_t> serials{0};
  const std::vector<std::uint8_t> qty{0};
  std::vector<link::sample::SampleBlock> sealed;
  for (std::uint64_t step = 0; step < 10; ++step) {
    const world::Snapshot s =
        make_snapshot(step, 0.1 * static_cast<double>(step), serials, qty, {1.0});
    link::sample::SampleBlock out;
    bool has = false;
    assert(builder.push(s, out, has).ok());
    if (has) {
      assert(out.sample_count() == want_samples);
      sealed.push_back(out);
    }
  }
  assert(sealed.size() == 2);
  link::sample::SampleBlock tail;
  bool has_tail = false;
  assert(builder.flush(tail, has_tail).ok());
  assert(has_tail && tail.sample_count() == 2);
}

} // namespace

int main() {
  test_golden_fixture();
  test_empty_selection_ticks();
  test_schema_change_seals();
  test_partial_blocks_and_count_seal();
  test_flatten_batch_stream_equality();
  test_above_linkframe_capacity();
  test_over_bound();
  test_malformed();
  test_byte_budget_seal();
  std::cout << "test_link_sample_block: OK\n";
  return 0;
}
