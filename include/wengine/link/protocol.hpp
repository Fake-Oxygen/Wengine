#pragma once

// External-app transport link ("wengine-link"): wire protocol v1.
//
// SCOPE: versioned snapshot bus for realtime local consumers (GUI, sample
// readers, future TCP bridge). This header is the protocol spec plus the
// transport-independent API sketch: wire layout, seqlock slot mechanics,
// encode/decode, versioning, and drop-oldest/lag policy. OS endpoints
// (SHM segment open/attach, TCP framing) are separate tasks and reuse
// everything defined here — no endpoint code lives in this header.
//
// SOLVER-AGNOSTIC BY DESIGN (no particle coupling):
//   The wire carries no particle model: a frame is clock (step/time) plus
//   generic spatial items (position, velocity, glyph hints) plus named
//   scalar channels. This mirrors the engine -> GUI transport exactly —
//   views consume link::Frame (link/frame.hpp) and never see solver
//   internals, and each solver plugs in through one adapter entry, with
//   frame_from_snapshot() as the particle adapter (the only one today).
//   Future solvers (circuits, EM, FEA, rigid bodies, ...) add one adapter
//   each. A circuit run publishes probe items (possibly none) plus
//   node-voltage channels; an EM run publishes sample-point items plus
//   sample-point items plus energy/peak channels — zero protocol change,
//   zero reader change, zero view change. Versions bump only when the
//   generic structs themselves need new field kinds. The shared model
//   lives in link/frame.hpp: link::Frame is consumed directly by GUI
//   views AND the encoder below — one source of truth, not twins.
//
// WHERE IT SITS (dependency direction preserved):
//   link -> world (+ core/status vocabulary). Engine never includes link.
//   Publishers are emit::ISink implementations built and owned by the
//   caller (CLI/GUI edge) and attached via Runner::add_sink /
//   run_headless(sinks) / SimController::add_sink, exactly like CsvSink.
//   Readers decode to link::Frame, which GUI views consume directly —
//   views never see the wire, just as they never see Snapshot internals.
//
// MULTI-SINK / MULTI-TRANSPORT DESIGN (the scalability seam):
//   * Engine fan-out (one run -> N sinks) already exists: Runner calls each
//     attached ISink with the same Snapshot (console + CSV proven together
//     in test_emit). A headless run can attach CsvSink + SHM publisher +
//     future TCP publisher simultaneously; no engine change needed.
//   * Transport fan-out (one publisher -> M readers) is this protocol:
//     single-writer / multi-reader seqlock ring. Readers never
//     back-pressure the writer; the writer never waits for readers.
//   * One segment per publisher run (and, in the future, one segment per
//     domain rather than a wider frame — fixed-size frames stay memcpy-able
//     and each domain keeps its own adapter entry).
//   * TCP later: another ISink publisher reusing LinkFrame bytes framed
//     over the stream (length-prefixed); same encode, same versioning,
//     same drop-oldest. Nothing here assumes shared memory except the
//     Slot/SegmentHeader placement, which also works as a plain in-process
//     ring (see the unit test).
//
// CRITICAL SNAPSHOT REVIEW (why the wire is NOT a Snapshot copy):
//   S1. Snapshot is host-side only AND domain-shaped (particle SoA today):
//       heap vectors, unbounded, not trivially copyable, and already one
//       solver's model. There is no defined byte layout to share, so the
//       wire cannot be "sized from Snapshot" — instead each domain adapts
//       into the generic Frame, and only the fixed-capacity POD LinkFrame
//       ever crosses the transport (never memcpy(&snapshot), never
//       memcpy(&frame)).
//   S2. Neither Snapshot nor Frame carries provenance: no solver/domain
//       tag, no version, no run id. Provenance attaches once at publish
//       time (SegmentHeader: domain + version + capacities); per-frame
//       identity is frame_seq + step_index + sim_time inside each LinkFrame.
//   S3. Adapter output is deterministic (particle adapter preserves dense
//       slot order); encode preserves item/channel order, decode restores
//       it, so a round-trip is exactly equal (==), not approximately equal.
//   S4. take_snapshot() already allocates per emit; the particle adapter
//       allocates in the same cost class (one items vector, a handful of
//       short channel strings). encode itself stays noexcept,
//       allocation-free, bounded memcpy. Decode may allocate (cold reader
//       side, outside the sim loop) — the asymmetry is deliberate and
//       documented on each function.
//   S5. Empty frames (no items, no channels) are valid, so readers observe
//       every emitted tick even when a run has nothing to show.
//   S6. Snapshot must NOT grow for link/GUI needs (project rule) — and with
//       this design the wire never needs to either: new domains add
//       adapters, not structs. LinkFrame is frozen except via the
//       versioning task's compat matrix.
//
// WIRE LAYOUT (v2, same-host — see portability note):
//
//   segment  = SegmentHeader + Slot[kSlotCount]
//   Slot     = seq (u64 seqlock) + words[kFrameWords] (atomic u64 payload)
//   LinkFrame = frame_seq, step_index, sim_time,
//               item_count, items[kMaxItems],
//               channel_count, channels[kMaxChannels]
//   LinkItem  = x,y,z, vx,vy,vz, size, color, has_velocity (+ pad to 64 B)
//   LinkChannel = name[kChannelNameSize] (NUL-terminated), value
//
//   LinkFrame is the local POD codec type only: encode/decode targets and
//   staging buffers stay ordinary structs. Only the atomic word image ever
//   sits in shared memory — slots are never memcpy'd as a whole, and the
//   shared words are never touched through a plain LinkFrame lvalue.
//   kFrameWords == sizeof(LinkFrame) / 8; word 0 carries frame_seq
//   (offset-asserted), so the latest-scan peek needs no plain access.
//
//   Frame ~= 40 B + 64 KiB items + 2.5 KiB channels (~= 67 KiB);
//   8 slots ~= 540 KiB per segment. Bounds are compile-time constants so
//   every peer agrees without negotiation; over-bound frames (too many
//   items/channels, over-long names) are dropped loudly (S7).
//   v2 keeps the v1 byte size (atomic u64 == u64 in size) but changes the
//   synchronization semantics, hence the version bump: v1 peers refuse v2
//   segments loudly at attach instead of racing them.
//
// SEQLOCK PROTOCOL (single writer, many readers — v2 atomic slot protocol):
//   Writer: seq -> odd (relaxed) | atomic word stores (relaxed) |
//           seq -> even (release).
//   Reader: a = seq (acquire); odd -> Busy (writer active, retry later);
//           zero -> Empty (never written); else atomic word loads
//           (acquire) into a reader-local LinkFrame, b = seq (acquire);
//           a != b -> Torn (discard, retry). A Torn/Busy read never yields
//           corrupt data, and — unlike v1 — no shared access is a C++ data
//           race: every concurrently touched shared word is an atomic.
//   Header init completes before any reader attaches, so version/domain/
//   capacities are observed without tearing by construction.
//   Ordering (conservative): payload stores are relaxed and sequenced
//   before the release seq store that publishes them; payload loads are
//   acquire and sandwiched between two acquire seq loads. An accepted read
//   (a == b, even, non-zero) observed no overlapping writer critical
//   section: any odd mark or even bump between the two seq loads discards
//   the copy, so mixed-epoch words can never be accepted (64-bit seq wrap
//   is the only ABA path and is practically unreachable). Readers are
//   bounded (fixed rescan budget in read_latest) and the writer never
//   waits for readers.
//
// VERSIONING + NEGOTIATION (v1 contract):
//   There is no back-and-forth to negotiate on a SHM bus: the writer stamps
//   the header, the reader accepts (exact match) or refuses loudly at
//   attach. That attach gate IS the negotiation protocol, in order: name
//   shape -> segment exists -> size == publisher layout -> version commit
//   word set -> version/capacities/domain exact match (inspect_header).
//   check_header() rejects anything else with kInvalidArgument — loudly at
//   open, never silently mid-stream. ShmReader classifies the outcome
//   (attached / missing / unready / refused / error) so tools retry the
//   transient states and fail fast with a versioned message on refusal.
//   Commit word: init_header writes version LAST with release semantics;
//   readers treat version == 0 as mid-construction (unready), never as a
//   version verdict — a partially initialized header can never be mistaken
//   for an incompatible writer.
//
// COMPAT MATRIX (v2 current; v1 refused loudly):
//   same version + capacities + valid domain -> attach, full interop.
//   anything else -> loud refusal with found-version report. v1 segments
//   (plain-payload layout) are refused by v2 readers with a version
//   verdict, and v2 segments are refused by v1 readers the same way —
//   same size, different synchronization, never interoperable. When v3
//   lands, its matrix row is added beside these rows, not instead of them.
//
// MIGRATION POLICY (for v2 authors):
//   Bump kVersion; never reuse a version number. Version 0 is reserved as
//   the unready marker and will never be assigned to a real writer. Keep
//   every v1 rejection test passing (detection only gets stricter). New
//   fields go in new structs — v1 readers must keep refusing v2 segments
//   loudly, which the crafted-segment tests guard. Multi-version readers
//   (accept ranges) are deferred until a second version actually exists;
//   until then readers support exactly one version and say so in refusal
//   messages.
//
// DROP-OLDEST + LAG ACCOUNTING (policy decision, recorded):
//   Writer drops oldest on full (ring overwrite); readers take latest only
//   and never drain a backlog — there is no queue to catch up with, only a
//   window onto the newest frame. A slow reader's slot may be recycled —
//   that IS the policy, matching FrameHistory's bounded drop-oldest ring.
//   No backlog, no blocking. publish_count (header, monotonic) and
//   frame_seq (per frame, monotonic) let readers compute exact loss:
//   missed = cur_seq - prev_seq - 1 (reader_missed(), LagStats below).
//   Receiver-lag never alters sim state or timing.
//
// PUBLISHER CONTRACT (S7 — the blocking-sink hazard):
//   Runner fans out synchronously: a blocking on_snapshot stalls the sim
//   AND every sibling sink. Transport publishers therefore MUST be
//   non-blocking (copy-and-return, drop-oldest on full) and non-throwing;
//   failures surface as sticky core::Status. An over-bound frame is dropped
//   and counted (dropped_over_bound) with kOutOfRange — loud, never
//   silently truncated (determinism: silent truncation corrupts data
//   without a trace).
//
// PER-SINK DECIMATION (recorded gap, not v1): Runner offers one global
// emit_every. Transports needing lower rates decimate client-side using
// frame_seq gaps. Approved future shape (no engine change): a StrideSink
// ISink wrapper forwarding every Nth snapshot. Do not add per-sink stride
// to Runner without that wrapper's concrete consumer.
//
// PORTABILITY: POSIX-first (shm_open/mmap in the writer task); Windows
// notes belong there. Atomics in shared memory require lock-free u64
// (static_asserted). Same-host scope: padding/endianness across hosts is
// out of v1 — cross-host framing is the TCP task's problem.

#include "wengine/core/status.hpp"
#include "wengine/link/frame.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>

namespace wengine::link {

// Protocol version peers must match exactly (see check_header).
// v2: atomic slot payload (PHYSI-31). Socket-schema versions evolve
// independently (compact_codec.hpp) and never see this word.
inline constexpr std::uint32_t kVersion = 2;
// v1 segment version, refused loudly by v2 readers (and vice versa).
inline constexpr std::uint32_t kVersionV1 = 1;
// Fixed frame bounds: every peer agrees without negotiation (see S1/S7).
inline constexpr std::size_t kMaxItems = 1024;
inline constexpr std::size_t kMaxChannels = 64;
// NUL-terminated channel name (31 usable chars); solver/domain tag (S2).
inline constexpr std::size_t kChannelNameSize = 32;
inline constexpr std::size_t kDomainSize = 32;
// Ring depth: 8 slots at 60 Hz emit ~= 130 ms of reader jitter cover.
inline constexpr std::size_t kSlotCount = 8;

// Fixed-capacity wire structs. Trivially copyable by construction: the only
// types ever memcpy'd across the transport. Host-side model (Frame) lives
// in link/frame.hpp; the particle adapter with it.
struct LinkItem {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
  double vx = 0.0;
  double vy = 0.0;
  double vz = 0.0;
  double size = 1.0;
  std::uint32_t color = 0;
  std::uint8_t has_velocity = 1;
  std::uint8_t reserved[3] = {}; // explicit pad: stable 64 B layout
};

static_assert(std::is_standard_layout_v<LinkItem>);
static_assert(std::is_trivially_copyable_v<LinkItem>);
static_assert(sizeof(LinkItem) == 64);

struct LinkChannel {
  char name[kChannelNameSize] = {}; // NUL-terminated (checked on decode)
  double value = 0.0;
};

static_assert(std::is_standard_layout_v<LinkChannel>);
static_assert(std::is_trivially_copyable_v<LinkChannel>);
static_assert(sizeof(LinkChannel) == kChannelNameSize + sizeof(double));

struct LinkFrame {
  std::uint64_t frame_seq = 0; // monotonic publish id (0 = never written)
  std::uint64_t step_index = 0;
  double sim_time = 0.0;
  std::uint64_t item_count = 0;
  LinkItem items[kMaxItems] = {};
  std::uint64_t channel_count = 0;
  LinkChannel channels[kMaxChannels] = {};
};

static_assert(std::is_standard_layout_v<LinkFrame>);
static_assert(std::is_trivially_copyable_v<LinkFrame>);
static_assert(sizeof(LinkFrame) == 5U * sizeof(std::uint64_t) +
                                        kMaxItems * sizeof(LinkItem) +
                                        kMaxChannels * sizeof(LinkChannel));

// Payload word count of the atomic slot image: LinkFrame bits carried as
// u64 words. LinkFrame stays the local POD codec type; only this word
// image is ever shared.
inline constexpr std::size_t kFrameWords =
    sizeof(LinkFrame) / sizeof(std::uint64_t);
static_assert(sizeof(LinkFrame) % sizeof(std::uint64_t) == 0);
static_assert(offsetof(LinkFrame, frame_seq) == 0);

// One seqlock slot. The seq word is the seqlock parity; the payload words
// are the only shared frame storage and are ALWAYS accessed atomically
// (writer stores, reader loads). Reader-local LinkFrame objects stay
// ordinary POD — only the shared image is atomic. Slots are never
// memcpy'd as a whole — only word images cross the critical section.
struct Slot {
  std::atomic<std::uint64_t> seq;
  std::array<std::atomic<std::uint64_t>, kFrameWords> words;

  Slot() noexcept : seq(0) {
    for (auto &w : words) {
      w.store(0, std::memory_order_relaxed);
    }
  }

  Slot(const Slot &) = delete;
  Slot &operator=(const Slot &) = delete;
  Slot(Slot &&) = delete;
  Slot &operator=(Slot &&) = delete;
};

// Cross-process atomics need lock-free u64/u32; fail loudly where untrue.
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(sizeof(std::atomic<std::uint64_t>) == sizeof(std::uint64_t));
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
static_assert(sizeof(std::atomic<std::uint32_t>) == sizeof(std::uint32_t));
// Same byte size as the v1 slot (seq + frame bits); only the sharing
// semantics changed, which is exactly what the version bump distinguishes.
static_assert(sizeof(Slot) == sizeof(std::uint64_t) + sizeof(LinkFrame));

// Segment header, written once at creation before readers attach.
// publish_count is the only field mutated post-init (writer only).
// Not copyable (atomic members): init in place via init_header, never
// memcpy'd as a whole — only slot word images cross the wire by value.
// version is the commit word: plain readers must observe it via acquire
// (see inspect_header); version == 0 means mid-construction.
struct SegmentHeader {
  std::atomic<std::uint32_t> version{0}; // kVersion once committed
  char domain[kDomainSize] = {}; // solver/domain tag, NUL-terminated (S2)
  std::uint64_t max_items = 0; // == kMaxItems once initialized
  std::uint64_t max_channels = 0; // == kMaxChannels once initialized
  std::uint64_t slot_count = 0; // == kSlotCount once initialized
  std::atomic<std::uint64_t> publish_count{0}; // successful publishes
};

// Process-local publisher bookkeeping (S7). publish_count mirrors into the
// header for readers; over-bound drops stay local in v1. Socket publishers
// add dropped_send (frames refused by a full peer/kernel buffer — transient,
// counted, never sticky).
struct PublisherStats {
  std::uint64_t published = 0;
  std::uint64_t dropped_over_bound = 0;
  std::uint64_t dropped_send = 0;
};

// Reader outcome: only kOk yields a usable frame. Busy/Torn mean "retry
// later"; Empty means "no frame published to this slot yet".
enum class SlotRead : std::uint8_t { kOk, kEmpty, kBusy, kTorn };

// Initialize a segment header before readers attach. Loud on misuse:
// empty or over-long domain is kInvalidArgument, never truncated (S7).
[[nodiscard]] inline core::Status init_header(SegmentHeader &h,
                                              std::string_view domain) noexcept {
  if (domain.empty() || domain.size() >= kDomainSize) {
    return {core::StatusCode::kInvalidArgument};
  }
  // Keep the commit word unready (version 0) throughout initialization so a
  // concurrent attacher observes only unready or a complete valid header,
  // never a ready version with incomplete fields. Exactly one release-store
  // below publishes kVersion after all immutable fields are valid.
  h.version.store(0, std::memory_order_relaxed);
  for (std::size_t i = 0; i < domain.size(); ++i) {
    h.domain[i] = domain[i];
  }
  h.domain[domain.size()] = '\0';
  h.max_items = static_cast<std::uint64_t>(kMaxItems);
  h.max_channels = static_cast<std::uint64_t>(kMaxChannels);
  h.slot_count = static_cast<std::uint64_t>(kSlotCount);
  h.publish_count.store(0, std::memory_order_relaxed);
  // Commit word LAST with release semantics: a reader observing version ==
  // kVersion (acquire) sees the fully initialized header above. Readers
  // treat version == 0 as mid-construction, never as a verdict.
  h.version.store(kVersion, std::memory_order_release);
  return core::Status::success();
}

// Structured attach-gate verdict: what failed, and which version the
// segment actually carries — the "clear error" behind every refusal.
// First failure wins, checked in negotiation order (version, capacities,
// domain), so the reported cause is always the actionable one.
enum class HeaderIssue : std::uint8_t {
  kNone,
  kVersionMismatch,
  kCapacityMismatch,
  kDomainMismatch
};

struct HeaderReport {
  core::Status status{};
  HeaderIssue issue = HeaderIssue::kNone;
  std::uint32_t found_version = 0;
};

[[nodiscard]] inline HeaderReport inspect_header(const SegmentHeader &h) noexcept {
  HeaderReport r{};
  r.found_version = h.version.load(std::memory_order_acquire);
  if (r.found_version != kVersion) {
    r.status = {core::StatusCode::kInvalidArgument};
    r.issue = HeaderIssue::kVersionMismatch;
    return r;
  }
  if (h.max_items != static_cast<std::uint64_t>(kMaxItems) ||
      h.max_channels != static_cast<std::uint64_t>(kMaxChannels) ||
      h.slot_count != static_cast<std::uint64_t>(kSlotCount)) {
    r.status = {core::StatusCode::kInvalidArgument};
    r.issue = HeaderIssue::kCapacityMismatch;
    return r;
  }
  if (h.domain[0] == '\0') {
    r.status = {core::StatusCode::kInvalidArgument};
    r.issue = HeaderIssue::kDomainMismatch;
    return r;
  }
  for (std::size_t i = 0; i < kDomainSize; ++i) {
    if (h.domain[i] == '\0') {
      r.status = core::Status::success();
      return r;
    }
  }
  r.status = {core::StatusCode::kInvalidArgument}; // unterminated: corrupt
  r.issue = HeaderIssue::kDomainMismatch;
  return r;
}

// Exact-match version/capacity/domain gate for attaching readers.
// Corrupt or foreign segments (bad version, missing NUL, capacity drift)
// fail loudly here instead of mid-stream. For the reason behind a
// refusal, see inspect_header.
[[nodiscard]] inline core::Status check_header(const SegmentHeader &h) noexcept {
  return inspect_header(h).status;
}

// Shared POSIX segment-name rule (single leading slash, nothing else):
// used by both endpoints so tools can validate upfront (exit 2) instead
// of failing at open time.
[[nodiscard]] inline bool valid_segment_name(std::string_view name) noexcept {
  if (name.size() < 2 || name.front() != '/') {
    return false;
  }
  return name.find('/', 1) == std::string_view::npos;
}

// Human refusal line for a HeaderReport (tools print this verbatim after
// "Error: "). Shared so every consumer — sample, bridge, future GUI
// subscriber — reports the same clear error for the same verdict. Edge
// code may allocate; never called on hot paths.
[[nodiscard]] inline std::string describe_refusal(std::string_view segment,
                                                  const HeaderReport &rep) {
  std::string msg = "segment '";
  msg += segment;
  switch (rep.issue) {
  case HeaderIssue::kVersionMismatch:
    msg += "' has version " + std::to_string(rep.found_version) +
           ", this reader supports version " + std::to_string(kVersion);
    break;
  case HeaderIssue::kCapacityMismatch:
    msg += "' has incompatible capacities";
    break;
  case HeaderIssue::kDomainMismatch:
    msg += "' has an invalid domain tag";
    break;
  default:
    msg += "' is incompatible";
    break;
  }
  return msg;
}

// Frame -> wire (S1/S3/S4). Hot publisher path: noexcept,
// allocation-free, bounded copies. Every bound is validated BEFORE
// touching `out`, so over-bound input is kOutOfRange with `out` untouched
// (drop loudly, S7). Order preserved, so decode(encode(f)) == f exactly.
[[nodiscard]] inline core::Status encode_frame(const Frame &in, LinkFrame &out,
                                               std::uint64_t frame_seq) noexcept {
  if (in.items.size() > kMaxItems || in.channels.size() > kMaxChannels) {
    return {core::StatusCode::kOutOfRange};
  }
  for (const FrameChannel &c : in.channels) {
    if (c.name.size() >= kChannelNameSize) {
      return {core::StatusCode::kOutOfRange};
    }
  }
  out.frame_seq = frame_seq;
  out.step_index = in.step;
  out.sim_time = in.time;
  out.item_count = static_cast<std::uint64_t>(in.items.size());
  for (std::size_t k = 0; k < in.items.size(); ++k) {
    const FrameItem &src = in.items[k];
    LinkItem &dst = out.items[k];
    dst.x = src.x;
    dst.y = src.y;
    dst.z = src.z;
    dst.vx = src.vx;
    dst.vy = src.vy;
    dst.vz = src.vz;
    dst.size = src.size;
    dst.color = src.color;
    dst.has_velocity = src.has_velocity ? std::uint8_t{1} : std::uint8_t{0};
  }
  out.channel_count = static_cast<std::uint64_t>(in.channels.size());
  for (std::size_t k = 0; k < in.channels.size(); ++k) {
    const FrameChannel &src = in.channels[k];
    LinkChannel &dst = out.channels[k];
    for (std::size_t i = 0; i < src.name.size(); ++i) {
      dst.name[i] = src.name[i];
    }
    dst.name[src.name.size()] = '\0';
    dst.value = src.value;
  }
  return core::Status::success();
}

// Wire -> Frame (S3/S4). Cold reader side: may allocate (vectors/strings),
// so NOT noexcept — never call inside the sim loop. Corrupt counts or an
// unterminated channel name is kInvalidArgument, never an OOB read.
[[nodiscard]] inline core::Status decode_frame(const LinkFrame &frame, Frame &out) {
  if (frame.item_count > static_cast<std::uint64_t>(kMaxItems) ||
      frame.channel_count > static_cast<std::uint64_t>(kMaxChannels)) {
    return {core::StatusCode::kInvalidArgument};
  }
  const std::size_t n_items = static_cast<std::size_t>(frame.item_count);
  const std::size_t n_channels = static_cast<std::size_t>(frame.channel_count);
  for (std::size_t k = 0; k < n_channels; ++k) {
    bool terminated = false;
    for (std::size_t i = 0; i < kChannelNameSize; ++i) {
      if (frame.channels[k].name[i] == '\0') {
        terminated = true;
        break;
      }
    }
    if (!terminated) {
      return {core::StatusCode::kInvalidArgument};
    }
  }
  out.step = frame.step_index;
  out.time = frame.sim_time;
  out.items.resize(n_items);
  for (std::size_t k = 0; k < n_items; ++k) {
    const LinkItem &src = frame.items[k];
    FrameItem &dst = out.items[k];
    dst.x = src.x;
    dst.y = src.y;
    dst.z = src.z;
    dst.vx = src.vx;
    dst.vy = src.vy;
    dst.vz = src.vz;
    dst.size = src.size;
    dst.color = src.color;
    dst.has_velocity = src.has_velocity != std::uint8_t{0};
  }
  out.channels.resize(n_channels);
  for (std::size_t k = 0; k < n_channels; ++k) {
    out.channels[k].name.assign(frame.channels[k].name);
    out.channels[k].value = frame.channels[k].value;
  }
  return core::Status::success();
}

// Writer side of the seqlock: mark active (odd), caller stores the staged
// frame words into the slot, then mark ready (even, release). The release
// store publishes the relaxed payload stores sequenced before it.
inline void slot_begin_write(Slot &slot) noexcept {
  const std::uint64_t s = slot.seq.load(std::memory_order_relaxed);
  slot.seq.store(s + std::uint64_t{1}, std::memory_order_relaxed);
}

inline void slot_end_write(Slot &slot) noexcept {
  const std::uint64_t s = slot.seq.load(std::memory_order_relaxed);
  slot.seq.store(s + std::uint64_t{1}, std::memory_order_release);
}

// Copy a reader-local frame into shared atomic words. Writer-only, called
// strictly between slot_begin_write and slot_end_write; `frame` itself is
// never shared, so plain byte access to it is race-free.
inline void slot_store_frame(Slot &slot, const LinkFrame &frame) noexcept {
  const auto *src = reinterpret_cast<const unsigned char *>(&frame);
  for (std::size_t i = 0; i < kFrameWords; ++i) {
    std::uint64_t w = 0;
    std::memcpy(&w, src + i * sizeof(std::uint64_t), sizeof(std::uint64_t));
    slot.words[i].store(w, std::memory_order_relaxed);
  }
}

// Peek the frame_seq word without a full copy. Must be bracketed by the
// caller with seq loads (see ShmReader::read_latest): the peek alone
// proves nothing, the unchanged seq around it does.
[[nodiscard]] inline std::uint64_t slot_peek_seq(const Slot &slot) noexcept {
  return slot.words[0].load(std::memory_order_acquire);
}

// Reader side: torn/busy reads report without the caller using `out`
// (out is clobbered on Torn, matching the historic contract — callers
// must only consume it on kOk).
[[nodiscard]] inline SlotRead slot_try_read(const Slot &slot, LinkFrame &out) noexcept {
  const std::uint64_t before = slot.seq.load(std::memory_order_acquire);
  if ((before & std::uint64_t{1}) != std::uint64_t{0}) {
    return SlotRead::kBusy;
  }
  if (before == std::uint64_t{0}) {
    return SlotRead::kEmpty;
  }
  auto *dst = reinterpret_cast<unsigned char *>(&out);
  for (std::size_t i = 0; i < kFrameWords; ++i) {
    const std::uint64_t w = slot.words[i].load(std::memory_order_acquire);
    std::memcpy(dst + i * sizeof(std::uint64_t), &w, sizeof(std::uint64_t));
  }
  const std::uint64_t after = slot.seq.load(std::memory_order_acquire);
  if (before != after) {
    return SlotRead::kTorn;
  }
  return SlotRead::kOk;
}

// Ring position for a monotonic write index. Callers use validated headers
// (check_header), so slot_count > 0 holds; the guard only avoids UB.
[[nodiscard]] inline constexpr std::uint64_t ring_slot(std::uint64_t write_index,
                                                       std::uint64_t slot_count) noexcept {
  return slot_count == 0 ? 0 : write_index % slot_count;
}

// Exact reader lag from per-frame ids: frames strictly between prev and
// current were overwritten before this reader saw them (drop-oldest).
[[nodiscard]] inline constexpr std::uint64_t reader_missed(std::uint64_t prev_seq,
                                                           std::uint64_t cur_seq) noexcept {
  return (cur_seq > prev_seq + 1) ? (cur_seq - prev_seq - 1) : 0;
}

// Reader-side lag/overflow counters: one cursor per reader (share-nothing
// across threads; the mapping itself is multi-reader safe). Feed every
// clean frame_seq observed (e.g. beside poll_newer); the counters then
// report exactly what this reader saw vs. what the bus carried.
// First observation counts pre-attach frames as missed — they existed on
// the bus and this reader never saw them (same display as the sample app).
struct LagStats {
  std::uint64_t last_seq = 0; // last consumed frame_seq (0 = none yet)
  std::uint64_t received = 0; // clean frames consumed
  std::uint64_t missed = 0; // frames overwritten before observation
  std::uint64_t max_gap = 0; // largest single-span loss
};

// Record one clean frame_seq; returns this span's loss. noexcept,
// allocation-free: safe beside any read loop, including the GUI subscriber.
[[nodiscard]] inline std::uint64_t lag_observe(LagStats &cur,
                                               std::uint64_t seq) noexcept {
  const std::uint64_t gap = reader_missed(cur.last_seq, seq);
  cur.missed += gap;
  if (gap > cur.max_gap) {
    cur.max_gap = gap;
  }
  cur.last_seq = seq;
  ++cur.received;
  return gap;
}

// Writer-side publish accounting (header side, visible to readers).
inline void header_note_publish(SegmentHeader &h) noexcept {
  h.publish_count.fetch_add(1, std::memory_order_relaxed);
}

[[nodiscard]] inline std::uint64_t header_published(const SegmentHeader &h) noexcept {
  return h.publish_count.load(std::memory_order_acquire);
}

} // namespace wengine::link
