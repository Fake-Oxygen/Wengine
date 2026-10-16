#pragma once

// Streaming batch decoder for length-prefixed v2 record streams (PHYSI-46).
//
// A TCP worker (link/tcp_worker.hpp) emits concatenated [u32 LE byte count
// + v2 record] frames carrying BOTH kinds: kFrame visualization records
// (compact_codec.hpp) and kSampleBlock sample batches (sample_block.hpp).
// This header splits such a byte stream and decodes every complete record
// with kind dispatch — the "sample decoding for batched records" entry the
// per-record decoders cannot provide alone.
//
// Cold reader side only: may allocate (vectors/strings/records), so NOT
// noexcept — never call inside the sim loop. No sockets, no threads.

#include "wengine/core/status.hpp"
#include "wengine/link/compact_codec.hpp"
#include "wengine/link/frame.hpp"
#include "wengine/link/sample_block.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace wengine::link {

// One decoded stream record. Exactly one payload is valid, selected by
// kind: kFrame fills frame, kSampleBlock fills block. seq is the
// per-kind monotonic id (frame_seq / block_seq); run_id is the
// publisher-run identity for session resets (PHYSI-40).
struct StreamRecord {
  compact::RecordKind kind = compact::RecordKind::kFrame;
  Frame frame;
  sample::SampleBlock block;
  std::uint64_t run_id = 0;
  std::uint64_t seq = 0;
};

// Decoded prefix of a byte stream: every complete record up to `consumed`
// bytes, in wire order. A trailing incomplete length prefix or record is
// normal TCP behavior, not an error: it sets has_partial and stops the
// walk so the caller can prepend the remainder to the next read.
struct StreamDecode {
  std::vector<StreamRecord> records;
  std::size_t consumed = 0;
  bool has_partial = false;
};

// Split + decode a length-prefixed stream. `want_domain` gates every
// record exactly (empty matches any domain, like the per-record decoders).
// Malformed input — an absurd prefix length, a failed schema/kind gate, a
// undecodable record — is kInvalidArgument with `out` holding exactly the
// records decoded before the bad one and `consumed` stopping at it; the
// caller reports the byte offset, never guesses past it. A trailing
// partial prefix/record is success with has_partial set.
[[nodiscard]] inline core::Status decode_stream(const unsigned char *bytes, std::size_t len,
                                               std::string_view want_domain,
                                               StreamDecode &out) {
  out.records.clear();
  out.consumed = 0;
  out.has_partial = false;
  if (bytes == nullptr && len != 0) {
    return {core::StatusCode::kInvalidArgument};
  }
  std::size_t cursor = 0;
  while (cursor < len) {
    if (len - cursor < sizeof(std::uint32_t)) {
      out.consumed = cursor;
      out.has_partial = true; // short prefix: wait for more bytes
      return core::Status::success();
    }
    const std::uint32_t rec_len = compact::detail::get_u32_le(bytes + cursor);
    if (rec_len < compact::kMinV2HeaderSize || rec_len > compact::kMaxRecordBytes) {
      return {core::StatusCode::kInvalidArgument}; // absurd length: never size from it
    }
    const std::size_t need = sizeof(std::uint32_t) + static_cast<std::size_t>(rec_len);
    if (len - cursor < need) {
      out.consumed = cursor;
      out.has_partial = true; // short record: wait for more bytes
      return core::Status::success();
    }
    const unsigned char *rec = bytes + cursor + sizeof(std::uint32_t);
    if (!compact::check_v2_schema(rec, rec_len).ok()) {
      return {core::StatusCode::kInvalidArgument};
    }
    StreamRecord r;
    const compact::RecordKind kind =
        static_cast<compact::RecordKind>(rec[8]);
    if (kind == compact::RecordKind::kFrame) {
      if (!compact::decode_record(rec, rec_len, want_domain, r.frame, r.run_id, r.seq,
                                  r.kind)
               .ok()) {
        return {core::StatusCode::kInvalidArgument};
      }
    } else if (kind == compact::RecordKind::kSampleBlock) {
      if (!sample::decode_block(rec, rec_len, want_domain, r.block, r.run_id, r.seq,
                                r.kind)
               .ok()) {
        return {core::StatusCode::kInvalidArgument};
      }
    } else {
      return {core::StatusCode::kInvalidArgument}; // known-gated kind, no decoder
    }
    out.records.push_back(std::move(r));
    cursor += need;
  }
  out.consumed = cursor;
  return core::Status::success();
}

} // namespace wengine::link
