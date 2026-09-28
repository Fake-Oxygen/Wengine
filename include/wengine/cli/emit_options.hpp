#pragma once

// Per-output sampling options shared by `sim run` and `sig run`
// (PHYSI-34): independent step-aligned strides + signal subscriptions.
//
// Each requested output (csv/shm/tcp/udp) may select:
//   --emit-<out>-stride N  : 0 = muted, >=1 = forward step%N==0 (default 1)
//   --emit-<out>-select L  : comma-separated stable source serials (signal only)
//
// Semantics (see docs/delivery.md sec. 2):
//   * --emit-every stays the global Runner gate; a per-output stride is an
//     edge-side StrideSink applied after it, so an output observes steps
//     divisible by BOTH gates (intersection). Decimation changes sampled
//     bandwidth and may alias a waveform.
//   * A select list is an edge-side SignalSubsetSink: it filters signal rows,
//     never ticks (every emitted tick still forwards, possibly with zero rows,
//     so reader LagStats stay exact). Empty selection = muted.
//   * Composition per output is Stride(Subset(publisher)): decimated ticks skip
//     the subset copy; counters stay split per wrapper.
//   * Transports share one capacity: 64 channels (signal: sig.count + one per
//     selected source; particle: <=6 channels, <=1024 items). CSV is unbounded.

#include "wengine/cli/args.hpp"
#include "wengine/core/status.hpp"
#include "wengine/link/protocol.hpp"

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wengine::cli {

struct EmitOutputSampling {
  bool stride_given = false;
  std::uint64_t stride = 1;
  bool select_given = false;
  std::vector<std::uint64_t> serials;
};

// Parse "--emit-<out>-stride" value: integer >= 0 (0 = muted).
[[nodiscard]] inline core::Result<std::uint64_t> parse_emit_stride(std::string_view s) {
  const core::Result<long long> p = parse_i64(s);
  if (!p.ok() || p.value < 0) {
    return core::Result<std::uint64_t>::failure("emit stride must be >= 0");
  }
  return core::Result<std::uint64_t>::success(static_cast<std::uint64_t>(p.value));
}

// Parse "--emit-<out>-select" value: "0,2,5" (non-empty, unique u64 serials).
[[nodiscard]] inline core::Result<std::vector<std::uint64_t>> parse_emit_select(
    std::string_view s) {
  std::vector<std::uint64_t> out;
  if (s.empty()) {
    return core::Result<std::vector<std::uint64_t>>::failure("empty selection");
  }
  std::size_t begin = 0;
  while (true) {
    const std::size_t comma = s.find(',', begin);
    const std::string_view tok =
        (comma == std::string_view::npos) ? s.substr(begin) : s.substr(begin, comma - begin);
    if (tok.empty()) {
      return core::Result<std::vector<std::uint64_t>>::failure("empty entry in selection");
    }
    std::uint64_t v = 0;
    const char *beg = tok.data();
    const char *end = tok.data() + tok.size();
    const auto res = std::from_chars(beg, end, v);
    if (res.ec != std::errc{} || res.ptr != end) {
      return core::Result<std::vector<std::uint64_t>>::failure("invalid serial in selection");
    }
    for (std::uint64_t e : out) {
      if (e == v) {
        return core::Result<std::vector<std::uint64_t>>::failure("duplicate serial in selection");
      }
    }
    out.push_back(v);
    if (comma == std::string_view::npos) {
      break;
    }
    begin = comma + 1;
  }
  if (out.empty()) {
    return core::Result<std::vector<std::uint64_t>>::failure("empty selection");
  }
  return core::Result<std::vector<std::uint64_t>>::success(out);
}

// Format a select list back for status lines ("0,2,5").
[[nodiscard]] inline std::string format_emit_select(const std::vector<std::uint64_t> &serials) {
  std::string s;
  for (std::size_t k = 0; k < serials.size(); ++k) {
    if (k > 0) {
      s += ',';
    }
    s += std::to_string(serials[k]);
  }
  return s;
}

// Required transport channels for a signal output: sig.count + sources.
// Returns true when the selection fits the shared 64-channel bound.
[[nodiscard]] inline bool signal_channels_fit(std::size_t n_sources) noexcept {
  return (n_sources + 1) <= link::kMaxChannels;
}

}  // namespace wengine::cli
