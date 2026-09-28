#pragma once

// Receiver-side sequence accounting (PHYSI-40): reorder-tolerant loss
// tracking with explicit session and attach semantics.
//
// WHY THIS EXISTS: link::lag_observe() assumes ordered delivery — it moves
// last_seq to every observed seq and counts ++received, so a reordered or
// duplicate UDP datagram rewinds the cursor and corrupts missed/received.
// The Python samples instead discard every late packet, which hides real
// reorders and still cannot infer a lost final tail from gaps alone. SHM
// additionally folds pre-attach history into missed, and latest-only jumps
// share one counter with genuine loss.
//
// WHAT THIS PROVIDES (one header, no transport, no syscalls):
//   * SequenceTracker: bounded reorder window (64 seqs, one u64 bitmask,
//     no allocation) for a single run/session. Gaps stay pending until
//     they fill (reordered) or age out of the window (confirmed loss).
//     Duplicates, stale arrivals outside the window, pre-attach history
//     and intentional latest-only skips are counted separately.
//   * SessionReceiver: wraps one tracker with a run_id cursor. A run_id
//     change resets the tracker (new publisher run under one address).
//     One SessionReceiver serves ONE (sender, stream); unrelated UDP
//     senders must not share it — key by source address at the call site.
//   * Trailing loss is reported ONLY with an authoritative expected final
//     sequence (trailing_report); otherwise it is unknown, never inferred.
//
// LAYERING: pure helpers over frame_seq/run_id integers. SHM latest-only
// readers keep using LagStats/reader_missed for monotonic jumps or call
// observe_latest() here to separate intentional skips; UDP/sample receivers
// use observe() per datagram. Hot path is noexcept/allocation-free.
// TCP stays ordered: any reordered/duplicate there is a framing violation,
// reported loudly by the caller (see the Python TCP receiver).

#include <cstddef>
#include <cstdint>

namespace wengine::link {

// Bounded reorder window: how many sequences behind the highest seen seq
// are remembered exactly (one bit each). Arrivals older than this are
// stale: already confirmed lost if they never arrived, counted but unable
// to fill the gap.
inline constexpr std::size_t kReceiveWindow = 64;

// Per-datagram verdict for SequenceTracker::observe (UDP path).
enum class SeqOutcome : std::uint8_t {
  kInOrder, // seq == highest + 1 (or first attach): new unique frame
  kAdvancedWithGap, // seq > highest + 1: new unique, gap stays pending
  kReorderedFill, // seq < highest, filled a pending gap: unique + reordered
  kDuplicate, // seq already seen inside the window
  kStale, // seq older than the window: too late to fill, counted only
  kIgnored, // seq == 0 (never a real frame_seq): no state change
};

// Single-session reorder tracker. Counters are monotonic within the
// session and reset only by an explicit session change (SessionReceiver)
// or reset().
struct SequenceTracker {
  bool attached = false; // any seq observed yet in this session
  std::uint64_t highest = 0; // max seq seen (0 = none yet)
  std::uint64_t seen_mask = 0; // bit d = (highest - d) seen, d in 0..63
  std::uint64_t unique = 0; // distinct frames accepted (incl. reordered fills)
  std::uint64_t duplicates = 0; // re-observations inside the window
  std::uint64_t reordered = 0; // late arrivals that filled a pending gap
  std::uint64_t stale = 0; // arrivals older than the window
  std::uint64_t pre_attach = 0; // seqs before the first observation (history)
  std::uint64_t skipped = 0; // intentional latest-only jumps (observe_latest)
  std::uint64_t confirmed = 0; // pending gaps aged out of the window (loss)
  std::uint64_t pending = 0; // gaps still inside the window (unknown yet)
  std::uint64_t max_gap = 0; // largest single advance gap (observe/observe_latest)
};

inline void receiver_reset(SequenceTracker &t) noexcept {
  t = SequenceTracker{};
}

[[nodiscard]] inline std::uint64_t receiver_pending(const SequenceTracker &t) noexcept {
  return t.pending;
}

// UDP reorder-tolerant path: record one datagram seq. Returns the verdict;
// counters update as documented on SequenceTracker. seq == 0 is ignored
// (0 means "no frame yet", never a real frame_seq).
[[nodiscard]] inline SeqOutcome receiver_observe(SequenceTracker &t,
                                                 std::uint64_t seq) noexcept {
  if (seq == 0) {
    return SeqOutcome::kIgnored;
  }
  if (!t.attached) {
    t.attached = true;
    t.highest = seq;
    // Bits for seqs below the first observation are pre-attach history or
    // nonexistent (seq 0 wrap): mark them seen/ignored so they can never
    // age out into confirmed loss.
    t.seen_mask = ~static_cast<std::uint64_t>(0);
    t.unique = 1;
    t.pre_attach = seq - 1;
    return SeqOutcome::kInOrder;
  }
  if (seq > t.highest) {
    const std::uint64_t delta = seq - t.highest;
    const std::uint64_t gap = delta - 1;
    if (gap > t.max_gap) {
      t.max_gap = gap;
    }
    // Expire pending gaps falling out of the window: the top `shift` bits
    // of the old mask leave the window; zero bits there were real pending
    // gaps (pre-attach/invalid bits are 1 by construction above).
    const std::uint64_t shift = delta >= kReceiveWindow ? kReceiveWindow : delta;
    std::uint64_t expired = 0;
    for (std::uint64_t d = kReceiveWindow - shift; d < kReceiveWindow; ++d) {
      // d < 64 always here (shift >= 1), so the shift is in range.
      if (((t.seen_mask >> d) & static_cast<std::uint64_t>(1)) == 0) {
        ++expired;
      }
    }
    // expired <= pending by construction; guard anyway for robustness.
    if (expired > t.pending) {
      expired = t.pending;
    }
    t.pending -= expired;
    t.confirmed += expired;
    t.seen_mask =
        (shift >= kReceiveWindow) ? static_cast<std::uint64_t>(0) : (t.seen_mask << shift);
    if (delta > kReceiveWindow) {
      // New gap is larger than the window: the oldest part is already
      // outside it, so confirm immediately; the newest 63 stay pending.
      const std::uint64_t within = kReceiveWindow - 1;
      const std::uint64_t beyond = gap - within;
      t.pending += within;
      t.confirmed += beyond;
    } else {
      t.pending += gap;
    }
    t.seen_mask |= static_cast<std::uint64_t>(1);
    t.highest = seq;
    ++t.unique;
    return gap == 0 ? SeqOutcome::kInOrder : SeqOutcome::kAdvancedWithGap;
  }
  const std::uint64_t distance = t.highest - seq;
  if (distance >= kReceiveWindow) {
    ++t.stale;
    return SeqOutcome::kStale;
  }
  // distance < 64: shift is in range.
  const std::uint64_t bit =
      (t.seen_mask >> distance) & static_cast<std::uint64_t>(1);
  if (bit != 0) {
    ++t.duplicates;
    return SeqOutcome::kDuplicate;
  }
  t.seen_mask |= (static_cast<std::uint64_t>(1) << distance);
  ++t.reordered;
  ++t.unique;
  // Filled gap was pending by construction (gaps below the first seq are
  // masked 1, so this bit was a real 0); guard anyway.
  if (t.pending > 0) {
    --t.pending;
  }
  return SeqOutcome::kReorderedFill;
}

// SHM/latest-only path: jump to the newest seq, counting the whole skipped
// span (plus any previously pending gaps) as intentional skips, never as
// confirmed loss. seq == 0 is ignored. seq <= highest counts as duplicate
// (poll_newer guarantees strictly newer; anything else is a re-read).
[[nodiscard]] inline std::uint64_t receiver_observe_latest(SequenceTracker &t,
                                                           std::uint64_t seq) noexcept {
  if (seq == 0) {
    return 0;
  }
  if (!t.attached) {
    t.attached = true;
    t.highest = seq;
    t.seen_mask = ~static_cast<std::uint64_t>(0);
    t.unique = 1;
    t.pre_attach = seq - 1;
    return 0;
  }
  if (seq <= t.highest) {
    ++t.duplicates;
    return 0;
  }
  const std::uint64_t gap = seq - t.highest - 1;
  if (gap > t.max_gap) {
    t.max_gap = gap;
  }
  t.skipped += t.pending + gap;
  t.pending = 0;
  t.highest = seq;
  t.seen_mask = ~static_cast<std::uint64_t>(0);
  ++t.unique;
  return gap;
}

// Trailing-loss report: frames after highest that never arrived. Only
// meaningful with an authoritative expected final sequence (e.g. a run
// header/footer or a closed TCP stream length); otherwise unknown.
struct TrailingReport {
  bool known = false; // false: no authoritative expectation, do not infer
  std::uint64_t missing = 0; // expected - highest when expected > highest
};

[[nodiscard]] inline TrailingReport
receiver_trailing(const SequenceTracker &t, std::uint64_t expected_final_seq,
                  bool has_expected) noexcept {
  TrailingReport r;
  if (!has_expected || !t.attached || expected_final_seq == 0) {
    return r;
  }
  r.known = true;
  r.missing = (expected_final_seq > t.highest) ? (expected_final_seq - t.highest) : 0;
  return r;
}

// Session wrapper: one tracker plus the run_id cursor. A run_id change
// (publisher restart under one address) resets the tracker so the new run's
// first seq counts its own pre-attach history, never the old run's loss.
// One SessionReceiver per (sender, stream): unrelated UDP senders must use
// separate instances (e.g. keyed by source address at the call site).
struct SessionReceiver {
  bool has_run = false;
  std::uint64_t run_id = 0;
  std::uint64_t sessions = 0; // distinct run_ids observed
  SequenceTracker track{};
};

inline void session_reset(SessionReceiver &s) noexcept {
  s = SessionReceiver{};
}

struct SessionOutcome {
  bool session_reset = false; // true when run_id changed (cursor reset)
  SeqOutcome seq_outcome = SeqOutcome::kIgnored;
};

// UDP path with session handling. latest_only selects the SHM-style jump
// accounting for the new seq (TCP-ordered callers keep their own strict
// verdict instead and should not set it).
[[nodiscard]] inline SessionOutcome session_observe(SessionReceiver &s,
                                                    std::uint64_t run_id,
                                                    std::uint64_t seq,
                                                    bool latest_only = false) noexcept {
  SessionOutcome out;
  if (seq == 0) {
    return out;
  }
  if (!s.has_run || run_id != s.run_id) {
    s.has_run = true;
    s.run_id = run_id;
    ++s.sessions;
    receiver_reset(s.track);
    out.session_reset = true;
  }
  if (latest_only) {
    (void)receiver_observe_latest(s.track, seq);
    out.seq_outcome = SeqOutcome::kInOrder;
    return out;
  }
  out.seq_outcome = receiver_observe(s.track, seq);
  return out;
}

} // namespace wengine::link
