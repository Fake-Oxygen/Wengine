#pragma once

// Batched signal samples (PHYSI-44): bounded SampleBlock builder + codec
// that carries consecutive signal ticks without decimating their values.
//
// WHY A SECOND RECORD KIND: the compact v2 frame codec (compact_codec.hpp)
// produces one packet with repeated channel-name strings per emitted step.
// Raising --emit-every reduces load by discarding samples, which can alias
// waveforms. A SampleBlock stores the channel schema (stable source serial
// + quantity tag) ONCE per block, then N consecutive samples as numeric
// arrays (step, accumulated sim_time, values). Batching reduces packet
// overhead while retaining every selected sample.
//
// WIRE LAYOUT (v2 kind 2, portable by construction; same magic/schema
// family as compact frames, distinct kind so neither decoder ever
// reinterprets the other):
//
//   block         = header schema sample*
//   header        = magic u32 "WLV2" (0x32564C57, == compact::kMagic)
//                   schema u32 (= 3, == compact::kSchemaVersion)
//                   kind u8 (= 2 sample block)
//                   reserved u8[3] (must be zero on decode)
//                   run_id u64 (publisher-run identity, opaque)
//                   block_seq u64 (monotonic per run; loss source like
//                     frame_seq)
//                   domain u8[32] (NUL-terminated tag, non-empty; v1 rule)
//                   channel_count u32
//                   sample_count u32                            [= 68 B]
//   schema entry  = serial u64 (stable source id, opaque)
//                   quantity u8 (signal::QuantityId code)
//                   reserved u8[3] (must be zero)               [= 12 B]
//   sample        = step u64
//                   time f64 (IEEE-754 bits, actual accumulated
//                     sim_time per tick — never a dt multiple)
//                   value f64[channel_count] (SI, bit-exact)   [= 16+8*C B]
//
// VERSIONING: same policy as compact frames — magic/schema/kind gated
// exactly, readers support exactly one schema and refuse everything else
// loudly. Quantity codes are validated against signal::kQuantityCount;
// a future quantity in an old binary refuses loudly (exact-match policy),
// never silently re-tagged.
//
// BOUNDS (declared memory/record budget, never silently truncated):
//   channels <= kBlockChannelsMax (1024: far above the 64-channel
//     visualization LinkFrame cap, so signal sets above 63 sources
//     round-trip here), samples <= kBlockSamplesMax (1024),
//   total <= kBlockMaxBytes (65507: every block fits a maximum-size UDP
//   payload, so one codec serves datagram and stream transports alike).
// A maximum-shape block is ~8.4 MB and never encodes; the builder seals on
// its configured sample/byte limits long before that. Over-bound encode is
// kOutOfRange with `out` untouched; corrupt decode is kInvalidArgument
// with `out` untouched.
//
// LAYERING: pure helpers over world::Snapshot, no sockets, no SHM, no
// publishers, no queue — queue/endpoint wiring is a separate task. The hot
// encode path is noexcept/allocation-free like encode_record; decode and
// the builder may allocate (cold edges only). The builder never strides:
// upstream StrideSink/emit_every selects steps, the builder retains every
// pushed tick (empty selections still record ticks with zero values so
// flatten(batch(stream)) == stream exactly and reader lag stays exact).

#include "wengine/core/status.hpp"
#include "wengine/link/compact_codec.hpp"
#include "wengine/link/protocol.hpp"
#include "wengine/signal/types.hpp"
#include "wengine/world/state.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace wengine::link::sample {

// Fixed block-header size: 4+4+1+3+8+8+32+4+4.
inline constexpr std::size_t kBlockHeaderSize = 68;
static_assert(kBlockHeaderSize == compact::kMinV2HeaderSize);
// One schema entry: serial u64 + quantity u8 + reserved u8[3].
inline constexpr std::size_t kChannelEntrySize = 12;
// One sample overhead: step u64 + time f64 (values append 8 per channel).
inline constexpr std::size_t kSampleOverhead = 16;
// Declared record budget: channels far above the LinkFrame cap (64), so
// batches above 63 sources round-trip; samples bound block memory even
// before the byte limit seals; bytes keep every block datagram-sized.
inline constexpr std::size_t kBlockChannelsMax = 1024;
inline constexpr std::size_t kBlockSamplesMax = 1024;
// Largest encodable block: fits a maximum-size UDP payload, so one codec
// serves datagram and stream transports alike (same budget as frames).
inline constexpr std::size_t kBlockMaxBytes = compact::kMaxRecordBytes;

// Owned block storage: schema (serials + quantity codes in snapshot slot
// order) plus dense row-major samples. Consumers receive blocks by value /
// const-ref — never views into builder or Snapshot lifetime.
struct SampleBlock {
  std::vector<std::uint64_t> serials;
  std::vector<std::uint8_t> quantities;
  std::vector<std::uint64_t> steps;
  std::vector<double> times;
  std::vector<double> values; // row-major: values[s * channels + c]

  [[nodiscard]] std::size_t channel_count() const noexcept { return serials.size(); }
  [[nodiscard]] std::size_t sample_count() const noexcept { return steps.size(); }

  bool operator==(const SampleBlock &) const noexcept = default;
};

// Exact encoded length for a block, or 0 when over-bound (counts or total
// vs kBlockMaxBytes). Pure/noexcept: buffer sizing without touching output.
[[nodiscard]] inline std::size_t encoded_size(const SampleBlock &in) noexcept {
  const std::size_t channels = in.serials.size();
  const std::size_t samples = in.steps.size();
  if (channels != in.quantities.size() || samples != in.times.size()) {
    return 0;
  }
  if (channels > kBlockChannelsMax || samples > kBlockSamplesMax) {
    return 0;
  }
  if (!in.values.empty() && in.values.size() != samples * channels) {
    return 0;
  }
  if (in.values.empty() && samples != 0 && channels != 0) {
    // Canonical blocks always carry values when samples*channels > 0;
    // an empty values vector with pending samples is a logic error.
    // Zero-channel ticks legitimately carry no values (see below).
    if (samples * channels != 0) {
      return 0;
    }
  }
  const std::size_t total =
      kBlockHeaderSize + channels * kChannelEntrySize +
      samples * (kSampleOverhead + channels * sizeof(double));
  return total <= kBlockMaxBytes ? total : 0;
}

// Block -> v2 kind-2 record bytes. Hot path: noexcept, allocation-free,
// bounded copies. Validates everything BEFORE touching `out` (domain shape,
// schema/sample consistency, quantity codes, counts, total vs capacity and
// vs kBlockMaxBytes); violations are kInvalidArgument/kOutOfRange with `out`
// and `out_len` untouched — loud, never silently truncated. Doubles cross
// as exact IEEE-754 bit patterns (NaN payloads preserved).
[[nodiscard]] inline core::Status encode_block(const SampleBlock &in,
                                              std::string_view domain,
                                              std::uint64_t run_id,
                                              std::uint64_t block_seq,
                                              unsigned char *out,
                                              std::size_t capacity,
                                              std::size_t &out_len) noexcept {
  if (domain.empty() || domain.size() >= kDomainSize || out == nullptr) {
    return {core::StatusCode::kInvalidArgument};
  }
  const std::size_t channels = in.serials.size();
  const std::size_t samples = in.steps.size();
  if (channels != in.quantities.size() || samples != in.times.size()) {
    return {core::StatusCode::kInvalidArgument};
  }
  if (channels > kBlockChannelsMax || samples > kBlockSamplesMax) {
    return {core::StatusCode::kOutOfRange};
  }
  if (channels * samples != 0 && in.values.size() != channels * samples) {
    return {core::StatusCode::kInvalidArgument};
  }
  if (channels * samples == 0 && !in.values.empty()) {
    return {core::StatusCode::kInvalidArgument};
  }
  for (std::size_t k = 0; k < channels; ++k) {
    const std::size_t q = in.quantities[k];
    if (q >= signal::kQuantityCount) {
      return {core::StatusCode::kInvalidArgument};
    }
  }
  const std::size_t need = encoded_size(in);
  if (need == 0 || need > capacity) {
    return {core::StatusCode::kOutOfRange};
  }
  compact::detail::put_u32_le(out + 0, compact::kMagic);
  compact::detail::put_u32_le(out + 4, compact::kSchemaVersion);
  out[8] = static_cast<unsigned char>(compact::RecordKind::kSampleBlock);
  out[9] = 0;
  out[10] = 0;
  out[11] = 0;
  compact::detail::put_u64_le(out + 12, run_id);
  compact::detail::put_u64_le(out + 20, block_seq);
  for (std::size_t i = 0; i < domain.size(); ++i) {
    out[28 + i] = static_cast<unsigned char>(domain[i]);
  }
  out[28 + domain.size()] = '\0';
  for (std::size_t i = 28 + domain.size() + 1; i < 28 + kDomainSize; ++i) {
    out[i] = '\0';
  }
  compact::detail::put_u32_le(out + 60, static_cast<std::uint32_t>(channels));
  compact::detail::put_u32_le(out + 64, static_cast<std::uint32_t>(samples));
  unsigned char *dst = out + kBlockHeaderSize;
  for (std::size_t c = 0; c < channels; ++c) {
    compact::detail::put_u64_le(dst + 0, in.serials[c]);
    dst[8] = in.quantities[c];
    dst[9] = 0;
    dst[10] = 0;
    dst[11] = 0;
    dst += kChannelEntrySize;
  }
  for (std::size_t s = 0; s < samples; ++s) {
    compact::detail::put_u64_le(dst + 0, in.steps[s]);
    compact::detail::put_f64_le(dst + 8, in.times[s]);
    dst += kSampleOverhead;
    for (std::size_t c = 0; c < channels; ++c) {
      const double v = channels == 0 ? 0.0 : in.values[s * channels + c];
      compact::detail::put_f64_le(dst, v);
      dst += sizeof(double);
    }
  }
  out_len = need;
  return core::Status::success();
}

// v2 kind-2 record bytes -> SampleBlock. Cold edge: may allocate, so NOT
// noexcept — never call inside the sim loop. Validates the full record
// BEFORE touching `out` (magic, schema, kind, zero reserved, domain shape
// + optional exact match, count bounds, quantity codes, reserved bytes,
// EXACT total length); violations are kInvalidArgument with `out`/ids
// untouched. Frame bytes (kind 1) report through out_kind but decode
// nothing — same loud-kind policy as decode_record. Round-trip is exactly
// equal (==), including empty selections (0 channels + N ticks) and
// non-finite doubles (bit-exact carrier).
[[nodiscard]] inline core::Status decode_block(const unsigned char *bytes, std::size_t len,
                                              std::string_view want_domain,
                                              SampleBlock &out,
                                              std::uint64_t &out_run_id,
                                              std::uint64_t &out_seq,
                                              compact::RecordKind &out_kind) {
  if (bytes == nullptr || len < kBlockHeaderSize) {
    return {core::StatusCode::kInvalidArgument};
  }
  if (compact::detail::get_u32_le(bytes + 0) != compact::kMagic ||
      compact::detail::get_u32_le(bytes + 4) != compact::kSchemaVersion) {
    return {core::StatusCode::kInvalidArgument};
  }
  const compact::RecordKind kind =
      static_cast<compact::RecordKind>(bytes[8]);
  out_kind = kind;
  if (bytes[9] != 0 || bytes[10] != 0 || bytes[11] != 0) {
    return {core::StatusCode::kInvalidArgument};
  }
  std::size_t domain_len = kDomainSize;
  for (std::size_t i = 0; i < kDomainSize; ++i) {
    if (bytes[28 + i] == '\0') {
      domain_len = i;
      break;
    }
  }
  if (domain_len == 0 || domain_len >= kDomainSize) {
    return {core::StatusCode::kInvalidArgument};
  }
  if (!want_domain.empty() &&
      (want_domain.size() != domain_len ||
       std::string_view(reinterpret_cast<const char *>(bytes + 28), domain_len) !=
           want_domain)) {
    return {core::StatusCode::kInvalidArgument};
  }
  if (kind != compact::RecordKind::kSampleBlock) {
    return {core::StatusCode::kInvalidArgument}; // known kind, no decoder here
  }
  const std::uint64_t n_channels_64 = compact::detail::get_u32_le(bytes + 60);
  const std::uint64_t n_samples_64 = compact::detail::get_u32_le(bytes + 64);
  if (n_channels_64 > static_cast<std::uint64_t>(kBlockChannelsMax) ||
      n_samples_64 > static_cast<std::uint64_t>(kBlockSamplesMax)) {
    return {core::StatusCode::kInvalidArgument};
  }
  const std::size_t n_channels = static_cast<std::size_t>(n_channels_64);
  const std::size_t n_samples = static_cast<std::size_t>(n_samples_64);
  const std::size_t want =
      kBlockHeaderSize + n_channels * kChannelEntrySize +
      n_samples * (kSampleOverhead + n_channels * sizeof(double));
  if (want > kBlockMaxBytes || want != len) {
    return {core::StatusCode::kInvalidArgument};
  }
  // Reserved discipline + quantity codes before touching `out`.
  for (std::size_t c = 0; c < n_channels; ++c) {
    const unsigned char *e = bytes + kBlockHeaderSize + c * kChannelEntrySize;
    if (e[9] != 0 || e[10] != 0 || e[11] != 0) {
      return {core::StatusCode::kInvalidArgument};
    }
    if (static_cast<std::size_t>(e[8]) >= signal::kQuantityCount) {
      return {core::StatusCode::kInvalidArgument};
    }
  }
  SampleBlock tmp;
  tmp.serials.resize(n_channels);
  tmp.quantities.resize(n_channels);
  for (std::size_t c = 0; c < n_channels; ++c) {
    const unsigned char *e = bytes + kBlockHeaderSize + c * kChannelEntrySize;
    tmp.serials[c] = compact::detail::get_u64_le(e + 0);
    tmp.quantities[c] = e[8];
  }
  tmp.steps.resize(n_samples);
  tmp.times.resize(n_samples);
  tmp.values.resize(n_samples * n_channels);
  const unsigned char *src =
      bytes + kBlockHeaderSize + n_channels * kChannelEntrySize;
  for (std::size_t s = 0; s < n_samples; ++s) {
    tmp.steps[s] = compact::detail::get_u64_le(src + 0);
    tmp.times[s] = compact::detail::get_f64_le(src + 8);
    src += kSampleOverhead;
    for (std::size_t c = 0; c < n_channels; ++c) {
      tmp.values[s * n_channels + c] = compact::detail::get_f64_le(src);
      src += sizeof(double);
    }
  }
  out = std::move(tmp);
  out_run_id = compact::detail::get_u64_le(bytes + 12);
  out_seq = compact::detail::get_u64_le(bytes + 20);
  return core::Status::success();
}

// Builder configuration: samples-per-block (count seal) and byte budget
// (size seal). Sampling stride lives upstream (StrideSink/emit_every) —
// the builder retains every pushed tick. Defaults pack 64 consecutive
// ticks or one datagram-sized block, whichever seals first.
struct SampleBlockConfig {
  std::size_t max_samples = 64;
  std::size_t max_bytes = kBlockMaxBytes;
};

// Accumulates consecutive Snapshots into owned SampleBlocks. Pure edge
// logic (no clocks, no I/O): the caller pushes every selected tick and
// calls flush() on its own deadline/shutdown to emit the short final
// block. Seals the active block on configured sample/byte limits or on
// schema change (serial/quantity/order — add/drop/respawn all seal, so
// flatten(batch(stream)) == stream exactly). Empty selections latch an
// empty schema and still record every tick (zero values), preserving tick
// observability like SignalSubsetSink. A push that would exceed the
// channel bound or carry an unknown quantity fails loudly WITHOUT
// mutating the builder; a sample that cannot fit the byte budget seals
// prior pending first and then fails loudly if the single sample still
// does not fit (raise the budget — defaults always fit one tick).
class SampleBlockBuilder {
public:
  explicit SampleBlockBuilder(SampleBlockConfig cfg = {}) : cfg_(sanitize(cfg)) {}

  [[nodiscard]] std::size_t pending_samples() const noexcept {
    return active_.steps.size();
  }
  [[nodiscard]] std::size_t channel_count() const noexcept {
    return has_schema_ ? active_.serials.size() : 0;
  }
  [[nodiscard]] bool has_schema() const noexcept { return has_schema_; }
  [[nodiscard]] const SampleBlockConfig &config() const noexcept { return cfg_; }

  void reset() {
    active_ = SampleBlock{};
    has_schema_ = false;
  }

  // Push one tick. On success, `has_sealed` reports whether a sealed block
  // was produced (schema change or size seal); when true, `out_sealed`
  // owns it and the builder already holds the new pending state. On
  // failure `out_sealed`/`has_sealed` are untouched except the documented
  // byte-budget path below.
  //
  // Byte-budget overflow of a single sample (only possible with a custom
  // max_bytes smaller than one tick): prior pending, if any, is sealed
  // into `out_sealed` (`has_sealed` true) and kOutOfRange is returned with
  // the sample rejected — the builder holds the fresh schema with zero
  // pending. Raise the budget and re-push.
  [[nodiscard]] core::Status push(const world::Snapshot &snap, SampleBlock &out_sealed,
                                 bool &has_sealed) {
    const std::size_t channels = snap.signal_count();
    if (channels > kBlockChannelsMax) {
      return {core::StatusCode::kOutOfRange};
    }
    for (std::size_t k = 0; k < channels; ++k) {
      if (static_cast<std::size_t>(snap.sig_quantity[k]) >= signal::kQuantityCount) {
        return {core::StatusCode::kInvalidArgument};
      }
    }
    const bool same = has_schema_ && schema_equal(snap);
    if (has_schema_ && !same) {
      if (!active_.steps.empty()) {
        out_sealed = std::move(active_);
        has_sealed = true;
        active_ = SampleBlock{};
        latch_schema(snap);
        if (projected_bytes(channels, 1) > cfg_.max_bytes) {
          return {core::StatusCode::kOutOfRange};
        }
        return append_row(snap);
      }
      has_sealed = false;
      active_ = SampleBlock{};
      latch_schema(snap);
      if (projected_bytes(channels, 1) > cfg_.max_bytes) {
        return {core::StatusCode::kOutOfRange};
      }
      return append_row(snap);
    }
    if (!has_schema_) {
      has_sealed = false;
      latch_schema(snap);
      has_schema_ = true;
      if (projected_bytes(channels, 1) > cfg_.max_bytes) {
        return {core::StatusCode::kOutOfRange};
      }
      return append_row(snap);
    }
    // Same schema: seal first when the next row would breach a limit.
    if (active_.steps.size() + 1 > cfg_.max_samples ||
        projected_bytes(channels, active_.steps.size() + 1) > cfg_.max_bytes) {
      if (!active_.steps.empty()) {
        const std::size_t single = projected_bytes(channels, 1);
        out_sealed = std::move(active_);
        has_sealed = true;
        active_ = SampleBlock{};
        latch_schema(snap);
        if (single > cfg_.max_bytes) {
          return {core::StatusCode::kOutOfRange};
        }
        return append_row(snap);
      }
      has_sealed = false;
      return {core::StatusCode::kOutOfRange};
    }
    has_sealed = false;
    return append_row(snap);
  }

  // Seal pending ticks into an owned block. Empty pending produces no
  // block (`has_block` false) and keeps the latched schema for the next
  // push; non-empty pending moves out and keeps the schema latched.
  [[nodiscard]] core::Status flush(SampleBlock &out, bool &has_block) {
    if (!has_schema_ || active_.steps.empty()) {
      has_block = false;
      return core::Status::success();
    }
    out = std::move(active_);
    has_block = true;
    active_ = SampleBlock{};
    // Re-latch the schema the move just carried away (flush keeps the
    // schema for subsequent pushes; reset() drops it entirely).
    active_.serials = out.serials;
    active_.quantities = out.quantities;
    return core::Status::success();
  }

private:
  static SampleBlockConfig sanitize(SampleBlockConfig cfg) noexcept {
    if (cfg.max_samples == 0 || cfg.max_samples > kBlockSamplesMax) {
      cfg.max_samples = kBlockSamplesMax;
    }
    if (cfg.max_bytes == 0 || cfg.max_bytes > kBlockMaxBytes) {
      cfg.max_bytes = kBlockMaxBytes;
    }
    return cfg;
  }

  void latch_schema(const world::Snapshot &snap) {
    active_.serials.resize(snap.signal_count());
    active_.quantities.resize(snap.signal_count());
    for (std::size_t k = 0; k < snap.signal_count(); ++k) {
      active_.serials[k] = snap.sig_serial[k];
      active_.quantities[k] = snap.sig_quantity[k];
    }
  }

  [[nodiscard]] bool schema_equal(const world::Snapshot &snap) const noexcept {
    if (active_.serials.size() != snap.signal_count()) {
      return false;
    }
    for (std::size_t k = 0; k < snap.signal_count(); ++k) {
      if (active_.serials[k] != snap.sig_serial[k] ||
          active_.quantities[k] != snap.sig_quantity[k]) {
        return false;
      }
    }
    return true;
  }

  static std::size_t projected_bytes(std::size_t channels, std::size_t samples) noexcept {
    return kBlockHeaderSize + channels * kChannelEntrySize +
           samples * (kSampleOverhead + channels * sizeof(double));
  }

  core::Status append_row(const world::Snapshot &snap) {
    active_.steps.push_back(snap.step_index);
    active_.times.push_back(snap.sim_time);
    const std::size_t channels = active_.serials.size();
    for (std::size_t k = 0; k < channels; ++k) {
      active_.values.push_back(snap.sig_value[k]);
    }
    return core::Status::success();
  }

  SampleBlockConfig cfg_;
  SampleBlock active_;
  bool has_schema_ = false;
};

} // namespace wengine::link::sample
