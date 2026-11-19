/**
 * @file
 * External-app transport link ("wengine-link"): wire protocol v3 (seqlock snapshot bus).
 * @note Versioned snapshot bus for realtime local consumers (GUI, sample readers, future
 *       TCP bridge): wire layout, seqlock slot mechanics, encode/decode, versioning, and
 *       drop-oldest/lag policy. OS endpoints (SHM segment open/attach, TCP framing) reuse
 *       everything defined here — no endpoint code lives in this header. link -> world
 *       (+ core/status vocabulary); engine never includes link. Publishers are emit::ISink
 *       implementations built and owned by the caller (CLI/GUI edge), never in RunConfig.
 *       Headers stay platform-free; POSIX syscalls live in the POSIX TUs under src/link/ under
 *       wengine_link_shm — a Windows port replaces the TU, never this header.
 *       Consumers: ShmPublisher/ShmReader, socket publishers, GUI frame-source registry.
 */
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
// Frame is ONE view projection for points-plus-scalars displays (see
// docs/domain-outputs.md), not the engine's data limit: each domain owns
// typed numeric blocks first. The wire carries the generic projection:
// a frame is clock (step/time) plus generic spatial items (position,
// velocity, glyph hints) plus named scalar channels. This mirrors one
// engine -> GUI projection exactly — views consume link::Frame
// (link/frame.hpp) and never see solver internals, and each solver plugs
// in through one adapter entry, with frame_from_snapshot() as the
// particle adapter (the only one today). New domains add typed blocks
// first and reuse Frame only when the projection is faithful; mesh
// topology, orientation/constraints, and bulk tensor fields require typed
// blocks and, where sec. 5 of that doc holds, new views — zero protocol
// change, zero reader change, zero view change otherwise. Versions bump
// only when the generic structs themselves need new field kinds. The shared model
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
//   S4. Snapshot capture reuses the Runner's emission buffer
//       (fill_snapshot, allocation-free after warmup); the particle
//       adapter allocates in the same cost class it always has (one items
//       vector, a handful of short channel strings). encode itself stays noexcept,
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
// WIRE LAYOUT (v3, same-host — see portability note):
//
//   segment  = SegmentHeader + Slot[kSlotCount]
//   Slot     = seq (u64 seqlock) + words[kFrameWords] (atomic u64 payload)
//   LinkFrame = frame_seq, step_index, sim_time,
//               item_count, items[kMaxItems],
//               channel_count, channels[kMaxChannels]
//   LinkItem  = x,y,z, vx,vy,vz, size, color, has_velocity, has_identity,
//               entity_index, entity_generation (+ pad to 72 B)
//   LinkChannel = name[kChannelNameSize] (NUL-terminated), value
//
//   LinkFrame is the local POD codec type only: encode/decode targets and
//   staging buffers stay ordinary structs. Only the atomic word image ever
//   sits in shared memory — slots are never memcpy'd as a whole, and the
//   shared words are never touched through a plain LinkFrame lvalue.
//   kFrameWords == sizeof(LinkFrame) / 8; word 0 carries frame_seq
//   (offset-asserted), so the latest-scan peek needs no plain access.
//
//   Frame ~= 40 B + 72 KiB items + 2.5 KiB channels (~= 75 KiB);
//   8 slots ~= 600 KiB per segment. Bounds are compile-time constants so
//   every peer agrees without negotiation; over-bound frames (too many
//   items/channels, over-long names) are dropped loudly (S7).
//   v2 keeps the v1 byte size (atomic u64 == u64 in size) but changes the
//   synchronization semantics, hence the version bump: v1 peers refuse v2
//   segments loudly at attach instead of racing them.
//   v3 grows LinkItem 64 -> 72 B for stable particle identity
//   (entity_index + entity_generation + has_identity): v1/v2 peers refuse
//   v3 segments loudly at attach (version gate + size gate).
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
// COMPAT MATRIX (v3 current; v1/v2 refused loudly):
//   same version + capacities + valid domain -> attach, full interop.
//   anything else -> loud refusal with found-version report. v1 segments
//   (plain-payload layout) are refused by v2/v3 readers with a version
//   verdict, v2 segments (64 B items, no identity) are refused by v3
//   readers the same way, and v3 segments are refused by v1/v2 readers —
//   different sizes and synchronization, never interoperable. When v4
//   lands, its matrix row is added beside these rows, not instead of them.
//
// MIGRATION POLICY (for v3 authors):
//   Bump kVersion; never reuse a version number. Version 0 is reserved as
//   the unready marker and will never be assigned to a real writer. Keep
//   every v1/v2 rejection test passing (detection only gets stricter). New
//   fields go in new structs — old readers must keep refusing new segments
//   loudly, which the crafted-segment tests guard. Multi-version readers
//   (accept ranges) are deferred until a second version actually exists;
//   until then readers support exactly one version and say so in refusal
//   messages. v3 identity fields: items without identity (legacy frames,
//   has_identity 0) decode to Frames with has_identity false — unavailable,
//   never fabricated from dense order.
//
// DROP-OLDEST + LAG ACCOUNTING (policy decision, recorded):
//   Writer drops oldest on full (ring overwrite). Latest-only readers take
//   the newest frame and never drain a backlog — no queue to catch up
//   with, only a window onto the newest frame. Ordered readers
//   (ShmReader::poll_next/read_batch) may instead drain the retained
//   window in ascending order: bursts within the ring arrive losslessly,
//   larger bursts report the exact overwritten prefix and resume from the
//   oldest retained frame. Either way a slow reader's slot may be
//   recycled — that IS the policy, matching FrameHistory's bounded
//   drop-oldest ring. No backlog beyond the ring, no blocking.
//   publish_count (header, monotonic) and frame_seq (per frame, monotonic)
//   let readers compute exact loss: missed = cur_seq - prev_seq - 1
//   (reader_missed(), LagStats below). Latest-only skips are intentional
//   coalescing; ordered-drain skips are ring overruns — same counters,
//   different interpretation (see delivery.md). Receiver-lag never alters
//   sim state or timing. Bounded jitter cover only: kSlotCount retained
//   frames (~130 ms at 60 Hz emit); a persistently slower reader still
//   overruns, loudly.
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
// v3: stable particle identity per item (PHYSI particle-identity task).
// v2: atomic slot payload (PHYSI-31). Socket-schema versions evolve
// independently (compact_codec.hpp) and never see this word.
/** @brief Protocol version peers must match exactly at attach (v3: stable particle identity per item). */
inline constexpr std::uint32_t kVersion = 3;
// v1 segment version, refused loudly by v2/v3 readers (and vice versa).
/** @brief v1 segment version (plain-payload layout): refused loudly by v2/v3 readers, and vice versa. */
inline constexpr std::uint32_t kVersionV1 = 1;
// v2 segment version (64 B items, no identity), refused loudly by v3.
/** @brief v2 segment version (64 B items, no identity): refused loudly by v3 readers, and vice versa. */
inline constexpr std::uint32_t kVersionV2 = 2;
// Fixed frame bounds: every peer agrees without negotiation (see S1/S7).
/** @brief Fixed frame bound: max generic spatial items per frame (every peer agrees without negotiation). */
inline constexpr std::size_t kMaxItems = 1024;
/** @brief Fixed frame bound: max named scalar channels per frame (every peer agrees without negotiation). */
inline constexpr std::size_t kMaxChannels = 64;
// NUL-terminated channel name (31 usable chars); solver/domain tag (S2).
/** @brief NUL-terminated channel name capacity (31 usable chars); solver/domain tag rule S2. */
inline constexpr std::size_t kChannelNameSize = 32;
/** @brief NUL-terminated solver/domain tag capacity (non-empty; empty or over-long domains are kInvalidArgument, never truncated). */
inline constexpr std::size_t kDomainSize = 32;
// Ring depth: 8 slots at 60 Hz emit ~= 130 ms of reader jitter cover.
/** @brief Ring depth: 8 slots at 60 Hz emit cover ~130 ms of reader jitter (bounded jitter cover only, never a completeness promise). */
inline constexpr std::size_t kSlotCount = 8;

// Fixed-capacity wire structs. Trivially copyable by construction: the only
// types ever memcpy'd across the transport. Host-side model (Frame) lives
// in link/frame.hpp; the particle adapter with it.
/**
 * @brief Fixed-capacity wire item: the local POD codec type (only types ever memcpy'd across the transport).
 * @note Trivially copyable by construction; only the atomic word image ever sits in shared memory — slots are never memcpy'd as a whole.
 */
struct LinkItem {
/** @brief X position (engine length units). */
  double x = 0.0;
/** @brief Y position (engine length units). */
  double y = 0.0;
/** @brief Z position (engine length units; 0 for 2D domains). */
  double z = 0.0;
/** @brief X velocity (engine velocity units). */
  double vx = 0.0;
/** @brief Y velocity (engine velocity units). */
  double vy = 0.0;
/** @brief Z velocity (engine velocity units). */
  double vz = 0.0;
/** @brief Glyph scale hint (rigid bodies render bigger). */
  double size = 1.0;
/** @brief Glyph color tag (0 = palette by index; future: heat/color data). */
  std::uint32_t color = 0;
/** @brief Velocity-valid flag (0/1 only, checked on decode). */
  std::uint8_t has_velocity = 1;
/** @brief Identity-valid flag (0/1 only, checked on decode; 0 = legacy frame, unavailable — never fabricated from dense order). */
  std::uint8_t has_identity = 0; // 0/1 only (checked on decode)
/** @brief Explicit pad: stable 72 B layout (always zeroed on encode). */
  std::uint8_t reserved[2] = {}; // explicit pad: stable 72 B layout
/** @brief Stable particle entity slot index (valid iff has_identity != 0). */
  std::uint32_t entity_index = 0; // valid iff has_identity != 0
/** @brief Stable particle entity generation (valid iff has_identity != 0). */
  std::uint32_t entity_generation = 0; // valid iff has_identity != 0
};

static_assert(std::is_standard_layout_v<LinkItem>);
static_assert(std::is_trivially_copyable_v<LinkItem>);
static_assert(sizeof(LinkItem) == 72);

/**
 * @brief Fixed-capacity wire channel: NUL-terminated name plus scalar value.
 * @note Trivially copyable by construction.
 */
struct LinkChannel {
/** @brief Channel name, NUL-terminated (checked on decode; unterminated is corrupt). */
  char name[kChannelNameSize] = {}; // NUL-terminated (checked on decode)
/** @brief Scalar value (SI units of the stamped quantity). */
  double value = 0.0;
};

static_assert(std::is_standard_layout_v<LinkChannel>);
static_assert(std::is_trivially_copyable_v<LinkChannel>);
static_assert(sizeof(LinkChannel) == kChannelNameSize + sizeof(double));

/**
 * @brief Local POD codec type: clock plus fixed-capacity items and channels (frame_seq 0 = never written).
 * @note Only this struct's atomic word image is ever shared; reader-local LinkFrame objects stay ordinary POD.
 */
struct LinkFrame {
/** @brief Monotonic publish id (0 = never written; LagStats loss source). */
  std::uint64_t frame_seq = 0; // monotonic publish id (0 = never written)
/** @brief Simulation step index. */
  std::uint64_t step_index = 0;
/** @brief Simulation time in seconds (SI). */
  double sim_time = 0.0;
/** @brief Populated items in items[] (bounded by kMaxItems). */
  std::uint64_t item_count = 0;
/** @brief Fixed-capacity item storage (only item_count entries valid). */
  LinkItem items[kMaxItems] = {};
/** @brief Populated channels in channels[] (bounded by kMaxChannels). */
  std::uint64_t channel_count = 0;
/** @brief Fixed-capacity channel storage (only channel_count entries valid). */
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
/** @brief Payload word count of the atomic slot image (LinkFrame bits carried as u64 words; LinkFrame stays the local POD codec type, only this word image is ever shared). */
inline constexpr std::size_t kFrameWords =
    sizeof(LinkFrame) / sizeof(std::uint64_t);
static_assert(sizeof(LinkFrame) % sizeof(std::uint64_t) == 0);
static_assert(offsetof(LinkFrame, frame_seq) == 0);

// One seqlock slot. The seq word is the seqlock parity; the payload words
// are the only shared frame storage and are ALWAYS accessed atomically
// (writer stores, reader loads). Reader-local LinkFrame objects stay
// ordinary POD — only the shared image is atomic. Slots are never
// memcpy'd as a whole — only word images cross the critical section.
/**
 * @brief One seqlock slot: the seq word is the seqlock parity; payload words are the only shared frame storage and are ALWAYS accessed atomically (writer stores, reader loads).
 * @note Slots are never memcpy'd as a whole — only word images cross the critical section. Not copyable/movable.
 */
struct Slot {
/** @brief Seqlock parity word (odd = writer active, even = ready, 0 = never written). */
  std::atomic<std::uint64_t> seq;
/** @brief Shared frame payload (atomic word image; reader-local LinkFrame objects stay ordinary POD). */
  std::array<std::atomic<std::uint64_t>, kFrameWords> words;

/** @brief Zero-initialize seq and all payload words. */
  Slot() noexcept : seq(0) {
    for (auto &w : words) {
      w.store(0, std::memory_order_relaxed);
    }
  }

/** @brief Non-copyable (owns atomic shared state). */
  Slot(const Slot &) = delete;
/** @brief Non-copyable (owns atomic shared state). */
  Slot &operator=(const Slot &) = delete;
/** @brief Non-movable (owns atomic shared state). */
  Slot(Slot &&) = delete;
/** @brief Non-movable (owns atomic shared state). */
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
/**
 * @brief Segment header, written once at creation before readers attach (version is the commit word).
 * @note publish_count is the only field mutated post-init (writer only). Not copyable (atomic members):
 *       init in place via init_header, never memcpy'd as a whole — only slot word images cross by value.
 *       Plain readers must observe version via acquire; version == 0 means mid-construction, never a verdict.
 */
struct SegmentHeader {
/** @brief kVersion once committed (commit word LAST with release semantics; 0 = mid-construction marker, never assigned to a real writer). */
  std::atomic<std::uint32_t> version{0}; // kVersion once committed
/** @brief Solver/domain tag, NUL-terminated (provenance S2; drives reader adapter choice). */
  char domain[kDomainSize] = {}; // solver/domain tag, NUL-terminated (S2)
/** @brief == kMaxItems once initialized (capacity gate). */
  std::uint64_t max_items = 0; // == kMaxItems once initialized
/** @brief == kMaxChannels once initialized (capacity gate). */
  std::uint64_t max_channels = 0; // == kMaxChannels once initialized
/** @brief == kSlotCount once initialized (capacity gate). */
  std::uint64_t slot_count = 0; // == kSlotCount once initialized
/** @brief Successful publishes (writer-side accounting, visible to readers). */
  std::atomic<std::uint64_t> publish_count{0}; // successful publishes
};

// Process-local publisher bookkeeping (S7). publish_count mirrors into the
// header for readers; over-bound drops stay local in v1. Socket publishers
// add dropped_send (frames refused by a full peer/kernel buffer — transient,
// counted, never sticky).
//
// PHYSI-39 per-cause vocabulary: every on_snapshot call counts exactly once
// in `attempted`. At any quiescent point (no TCP pending frame) the
// accounting is exact:
//   attempted == published + dropped_over_bound + dropped_send
//              + dropped_pending_blocked + dropped_queue_full
//              + disabled_after_error (+ 1 while a TCP frame is pending).
// Field notes:
// - published: frames accepted locally (SHM bus / kernel). Local
//   acceptance only — remote receipt is unknown unless a receiver reports
//   its own LagStats.
// - dropped_send: socket backpressure (TCP zero-byte EAGAIN, UDP datagram
//   refused) — transient, counted, never sticky.
// - dropped_pending_blocked: TCP only — a new frame dropped because the
//   depth-1 pending tail was still blocked (stream framing forbids
//   discarding a partial prefix). Previously folded into dropped_send.
 // - dropped_queue_full: bounded-queue refusal counted by the PHYSI-45
 //   owning queue (link/output_queue.hpp); transports with no queue stage
 //   leave this 0.
// - disabled_after_error: snapshots ignored after the publisher went
//   sticky (including the frame that triggered a sticky local failure
//   such as an adapter throw or a dead peer).
// - bytes_accepted: wire bytes accepted locally (TCP length-prefix +
//   record, UDP datagram, SHM slot image) for effective bytes/s.
// - Intentional stride/subset skips live in the routing wrappers
//   (emit/routing.hpp), never here; pending/abandoned-at-close is
//   publisher state (has_pending_frame/pending_bytes), reported by the
//   edge at finish/close rather than as a cumulative counter.
/**
 * @brief Process-local publisher bookkeeping (PHYSI-39 per-cause vocabulary; publish_count mirrors into the header for readers).
 * @note Local acceptance only (SHM bus / kernel) — remote receipt is unknown unless a receiver
 *       reports its own LagStats. Intentional stride/subset skips live in the routing wrappers
 *       (emit/routing.hpp), never here; pending/abandoned-at-close is publisher edge state.
 */
struct PublisherStats {
/** @brief Every on_snapshot call counts exactly once (the source of truth every downstream count is cut against). */
  std::uint64_t attempted = 0;
/** @brief Frames accepted locally (mirrors SHM header publish_count). */
  std::uint64_t published = 0;
/** @brief Over-bound refusals (shared encode gate; sticky kOutOfRange, never silently truncated). */
  std::uint64_t dropped_over_bound = 0;
/** @brief Socket backpressure (TCP zero-byte EAGAIN, UDP datagram refusal) — transient, counted, never sticky. */
  std::uint64_t dropped_send = 0;
/** @brief TCP only: new frame dropped while the depth-1 pending tail is still blocked (stream framing forbids discarding a partial prefix). */
  std::uint64_t dropped_pending_blocked = 0;
/** @brief Bounded-queue refusals (link/output_queue.hpp stage; 0 with no queue stage). */
  std::uint64_t dropped_queue_full = 0;
/** @brief Snapshots ignored once sticky (including the frame that triggered a sticky local failure). */
  std::uint64_t disabled_after_error = 0;
/** @brief Wire bytes accepted locally (SHM slot image / TCP prefix+record / UDP datagram) for effective bytes/s. */
  std::uint64_t bytes_accepted = 0;
};

// Reader outcome: only kOk yields a usable frame. Busy/Torn mean "retry
// later"; Empty means "no frame published to this slot yet".
/**
 * @brief Reader outcome: only kOk yields a usable frame. kEmpty means no frame published to this slot yet;
 *        kBusy means the writer is active (retry later); kTorn means discard the copy (retry later).
 * @note A Torn/Busy read never yields corrupt data, and no shared access is a C++ data race.
 */
enum class SlotRead : std::uint8_t { kOk, kEmpty, kBusy, kTorn };

// Initialize a segment header before readers attach. Loud on misuse:
// empty or over-long domain is kInvalidArgument, never truncated (S7).
/**
 * @brief Initialize a segment header in place before readers attach (commit word published LAST with release).
 * @param[out] h header to initialize (version kept unready at 0 throughout so concurrent attachers observe only unready or complete valid headers).
 * @param[in] domain solver/domain tag (non-empty, shorter than kDomainSize).
 * @return kOk on success; kInvalidArgument for empty or over-long domain (loud misuse, never truncated).
 */
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
/**
 * @brief Structured attach-gate verdict: what failed, checked in negotiation order (version, capacities, domain) so the reported cause is always the actionable one.
 * @note kNone means no issue; kVersionMismatch/kCapacityMismatch/kDomainMismatch name the first failing gate.
 */
enum class HeaderIssue : std::uint8_t {
  kNone,
  kVersionMismatch,
  kCapacityMismatch,
  kDomainMismatch
};

/**
 * @brief Attach-gate report: what failed, and which version the segment actually carries (the clear error behind every refusal).
 * @note First failure wins, checked in negotiation order (version, capacities, domain).
 */
struct HeaderReport {
/** @brief Gate verdict (kOk when attached; kInvalidArgument on refusal). */
  core::Status status{};
/** @brief Failing gate (kNone when attached). */
  HeaderIssue issue = HeaderIssue::kNone;
/** @brief Version word actually observed (0 = mid-construction marker, never a real writer). */
  std::uint32_t found_version = 0;
};

/**
 * @brief Inspect a header and classify the attach verdict (the reason behind every refusal).
 * @param[in] h header to inspect (observed via acquire).
 * @return Report with status, first-failure issue, and found version.
 */
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
/**
 * @brief Exact-match version/capacity/domain gate for attaching readers.
 * @param[in] h header to gate.
 * @return kOk on exact match; kInvalidArgument for corrupt or foreign segments (loud at open, never silent mid-stream). For the reason, see inspect_header.
 */
[[nodiscard]] inline core::Status check_header(const SegmentHeader &h) noexcept {
  return inspect_header(h).status;
}

// Shared POSIX segment-name rule (single leading slash, nothing else):
// used by both endpoints so tools can validate upfront (exit 2) instead
// of failing at open time.
/**
 * @brief Shared POSIX segment-name rule (single leading slash, nothing else).
 * @param[in] name candidate segment name.
 * @return True for "/name" shape; used by endpoints to fail upfront (usage error) instead of at open time.
 */
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
/**
 * @brief Human refusal line for a HeaderReport (tools print it verbatim after "Error: ").
 * @param[in] segment segment name refused.
 * @param[in] rep gate report to render.
 * @return Refusal message naming the found version / capacities / domain verdict. Edge code may allocate; never called on hot paths.
 */
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
/**
 * @brief Frame to wire (S1/S3/S4; hot publisher path: noexcept, allocation-free, bounded memcpy).
 * @param[in] in source frame (order preserved).
 * @param[out] out wire frame (untouched on failure).
 * @param[in] frame_seq monotonic publish id stamped into the wire frame.
 * @return kOk on success; kOutOfRange for over-bound input (items/channels/name lengths) with out untouched — drop loudly (S7), never silently truncate.
 * @note decode(encode(f)) == f exactly. Publish path performs no syscalls or I/O beyond this encode plus the seqlock copy.
 */
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
    dst.has_identity = src.has_identity ? std::uint8_t{1} : std::uint8_t{0};
    dst.reserved[0] = 0;
    dst.reserved[1] = 0;
    dst.entity_index = src.entity_index;
    dst.entity_generation = src.entity_generation;
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
// so NOT noexcept — never call inside the sim loop. Corrupt counts, an
// unterminated channel name, or a non-0/1 identity flag is
// kInvalidArgument, never an OOB read. Items without identity
// (has_identity 0, e.g. legacy publishers) yield Frames with
// has_identity false — unavailable, never fabricated.
/**
 * @brief Wire to Frame (S3/S4; cold reader side: may allocate, so NOT noexcept — never call inside the sim loop).
 * @param[in] frame wire frame to decode.
 * @param[out] out decoded frame (untouched on failure).
 * @return kOk on success; kInvalidArgument for corrupt counts, unterminated channel names, or non-0/1 identity flags (never an OOB read).
 * @note Items without identity (has_identity 0) yield Frames with has_identity false — unavailable, never fabricated.
 */
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
  for (std::size_t k = 0; k < n_items; ++k) {
    if (frame.items[k].has_identity != std::uint8_t{0} && frame.items[k].has_identity != std::uint8_t{1}) {
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
    dst.has_identity = src.has_identity != std::uint8_t{0};
    dst.entity_index = src.entity_index;
    dst.entity_generation = src.entity_generation;
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
/**
 * @brief Writer side of the seqlock: mark active (odd). The release store in slot_end_write publishes the relaxed payload stores sequenced before it.
 * @param[in,out] slot slot to mark.
 */
inline void slot_begin_write(Slot &slot) noexcept {
  const std::uint64_t s = slot.seq.load(std::memory_order_relaxed);
  slot.seq.store(s + std::uint64_t{1}, std::memory_order_relaxed);
}

/**
 * @brief Writer side of the seqlock: mark ready (even, release).
 * @param[in,out] slot slot to publish.
 */
inline void slot_end_write(Slot &slot) noexcept {
  const std::uint64_t s = slot.seq.load(std::memory_order_relaxed);
  slot.seq.store(s + std::uint64_t{1}, std::memory_order_release);
}

// Copy a reader-local frame into shared atomic words. Writer-only, called
// strictly between slot_begin_write and slot_end_write; `frame` itself is
// never shared, so plain byte access to it is race-free.
/**
 * @brief Copy a reader-local frame into shared atomic words (writer-only, strictly between slot_begin_write and slot_end_write).
 * @param[in,out] slot destination slot.
 * @param[in] frame reader-local frame (plain byte access is race-free: the frame itself is never shared).
 */
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
/**
 * @brief Peek the frame_seq word without a full copy (must be bracketed by seq loads: the peek alone proves nothing, the unchanged seq around it does).
 * @param[in] slot slot to peek.
 * @return frame_seq word image (acquire).
 */
[[nodiscard]] inline std::uint64_t slot_peek_seq(const Slot &slot) noexcept {
  return slot.words[0].load(std::memory_order_acquire);
}

// Reader side: torn/busy reads report without the caller using `out`
// (out is clobbered on Torn, matching the historic contract — callers
// must only consume it on kOk).
/**
 * @brief Seqlock read: torn/busy reads report without the caller using out (out is clobbered on Torn — callers must only consume it on kOk).
 * @param[in] slot slot to read.
 * @param[out] out reader-local copy (valid only on kOk).
 * @return kOk/kEmpty/kBusy/kTorn per SlotRead.
 */
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
/**
 * @brief Ring position for a monotonic write index (callers use validated headers, so slot_count > 0 holds; the guard only avoids UB).
 * @param[in] write_index monotonic write index.
 * @param[in] slot_count ring depth.
 * @return write_index % slot_count (0 when slot_count is 0).
 */
[[nodiscard]] inline constexpr std::uint64_t ring_slot(std::uint64_t write_index,
                                                       std::uint64_t slot_count) noexcept {
  return slot_count == 0 ? 0 : write_index % slot_count;
}

// Exact reader lag from per-frame ids: frames strictly between prev and
// current were overwritten before this reader saw them (drop-oldest).
/**
 * @brief Exact reader lag from per-frame ids: frames strictly between prev and current were overwritten before this reader saw them (drop-oldest).
 * @param[in] prev_seq last consumed frame_seq.
 * @param[in] cur_seq newly observed frame_seq.
 * @return cur_seq - prev_seq - 1 when positive, else 0.
 */
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
/**
 * @brief Reader-side lag/overflow counters: one cursor per reader (share-nothing across threads; the mapping itself is multi-reader safe).
 * @note Feed every clean frame_seq observed (e.g. beside poll_newer); counters report exactly what this
 *       reader saw vs. what the bus carried. First observation counts pre-attach frames as missed.
 */
struct LagStats {
/** @brief Last consumed frame_seq (0 = none yet). */
  std::uint64_t last_seq = 0; // last consumed frame_seq (0 = none yet)
/** @brief Clean frames consumed. */
  std::uint64_t received = 0; // clean frames consumed
/** @brief Frames overwritten before observation (per-frame frame_seq gaps; latest-only skips are intentional coalescing, ordered-drain skips are ring overruns — same counters, different interpretation). */
  std::uint64_t missed = 0; // frames overwritten before observation
/** @brief Largest single-span loss. */
  std::uint64_t max_gap = 0; // largest single-span loss
};

// Record one clean frame_seq; returns this span's loss. noexcept,
// allocation-free: safe beside any read loop, including the GUI subscriber.
/**
 * @brief Record one clean frame_seq (noexcept, allocation-free: safe beside any read loop, including the GUI subscriber).
 * @param[in,out] cur reader cursor to advance.
 * @param[in] seq observed frame_seq.
 * @return This span's loss (reader_missed(old, seq)).
 */
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
/**
 * @brief Writer-side publish accounting (header side, visible to readers).
 * @param[in,out] h header whose publish_count increments (writer only).
 */
inline void header_note_publish(SegmentHeader &h) noexcept {
  h.publish_count.fetch_add(1, std::memory_order_relaxed);
}

/**
 * @brief Total frames published into the segment (0 while publisher missing/unready).
 * @param[in] h header to read (acquire).
 * @return publish_count value.
 */
[[nodiscard]] inline std::uint64_t header_published(const SegmentHeader &h) noexcept {
  return h.publish_count.load(std::memory_order_acquire);
}

} // namespace wengine::link
