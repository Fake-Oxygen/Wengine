"""Receiver-side sequence accounting shared by the socket sample receivers.

Python mirror of include/wengine/link/receiver.hpp (PHYSI-40): bounded
reorder window (64 seqs), per-session run_id cursors, and separated
pre-attach / intentional-skip / confirmed-loss counters. Trailing loss is
reported only with an authoritative expected final sequence.

One SessionReceiver serves ONE (sender, stream): UDP call sites key by
source address so unrelated senders never share a counter. TCP call sites
keep their ordered verdict (any reordered/duplicate is a framing
violation) but share the session/pre-attach/trailing semantics.
"""

from __future__ import annotations

WINDOW = 64
_MASK_ALL = (1 << WINDOW) - 1

IN_ORDER = "in_order"
ADVANCED_WITH_GAP = "advanced_with_gap"
REORDERED_FILL = "reordered_fill"
DUPLICATE = "duplicate"
STALE = "stale"
IGNORED = "ignored"


class SequenceTracker:
    """Single-session reorder tracker; field names match the C++ struct."""

    def __init__(self) -> None:
        self.attached = False
        self.highest = 0
        self.seen_mask = 0
        self.unique = 0
        self.duplicates = 0
        self.reordered = 0
        self.stale = 0
        self.pre_attach = 0
        self.skipped = 0
        self.confirmed = 0
        self.pending = 0
        self.max_gap = 0

    def reset(self) -> None:
        self.__init__()

    def observe(self, seq: int) -> str:
        if seq == 0:
            return IGNORED
        if not self.attached:
            self.attached = True
            self.highest = seq
            self.seen_mask = _MASK_ALL
            self.unique = 1
            self.pre_attach = seq - 1
            return IN_ORDER
        if seq > self.highest:
            delta = seq - self.highest
            gap = delta - 1
            if gap > self.max_gap:
                self.max_gap = gap
            shift = delta if delta < WINDOW else WINDOW
            expired = 0
            for d in range(WINDOW - shift, WINDOW):
                if ((self.seen_mask >> d) & 1) == 0:
                    expired += 1
            expired = min(expired, self.pending)
            self.pending -= expired
            self.confirmed += expired
            if delta > WINDOW:
                within = WINDOW - 1
                self.pending += within
                self.confirmed += gap - within
                self.seen_mask = 0
            else:
                self.pending += gap
                self.seen_mask = (self.seen_mask << shift) & _MASK_ALL
            self.seen_mask |= 1
            self.highest = seq
            self.unique += 1
            return IN_ORDER if gap == 0 else ADVANCED_WITH_GAP
        distance = self.highest - seq
        if distance >= WINDOW:
            self.stale += 1
            return STALE
        if ((self.seen_mask >> distance) & 1) != 0:
            self.duplicates += 1
            return DUPLICATE
        self.seen_mask |= 1 << distance
        self.reordered += 1
        self.unique += 1
        if self.pending > 0:
            self.pending -= 1
        return REORDERED_FILL

    def observe_latest(self, seq: int) -> int:
        """Latest-only jump: skipped span counts as intentional skips."""
        if seq == 0:
            return 0
        if not self.attached:
            self.attached = True
            self.highest = seq
            self.seen_mask = _MASK_ALL
            self.unique = 1
            self.pre_attach = seq - 1
            return 0
        if seq <= self.highest:
            self.duplicates += 1
            return 0
        gap = seq - self.highest - 1
        if gap > self.max_gap:
            self.max_gap = gap
        self.skipped += self.pending + gap
        self.pending = 0
        self.highest = seq
        self.seen_mask = _MASK_ALL
        self.unique += 1
        return gap

    def trailing(self, expected: int, has_expected: bool) -> tuple[bool, int]:
        if not has_expected or not self.attached or expected == 0:
            return False, 0
        if expected <= self.highest:
            return True, 0
        return True, expected - self.highest


class SessionReceiver:
    """One tracker plus the run_id cursor for a single (sender, stream)."""

    def __init__(self) -> None:
        self.has_run = False
        self.run_id = 0
        self.sessions = 0
        self.track = SequenceTracker()

    def observe(
        self, run_id: int, seq: int, *, latest_only: bool = False
    ) -> tuple[bool, str]:
        if seq == 0:
            return False, IGNORED
        reset = False
        if not self.has_run or run_id != self.run_id:
            self.has_run = True
            self.run_id = run_id
            self.sessions += 1
            self.track.reset()
            reset = True
        if latest_only:
            self.track.observe_latest(seq)
            return reset, IN_ORDER
        return reset, self.track.observe(seq)


def session_for(sessions: dict, source_key: str) -> SessionReceiver:
    """Return the per-sender session, creating it on first use."""
    session = sessions.get(source_key)
    if session is None:
        session = SessionReceiver()
        sessions[source_key] = session
    return session


def summarize(sessions: dict) -> str:
    """Aggregate counters across per-sender sessions for final summaries."""
    unique = sum(s.track.unique for s in sessions.values())
    duplicates = sum(s.track.duplicates for s in sessions.values())
    reordered = sum(s.track.reordered for s in sessions.values())
    stale = sum(s.track.stale for s in sessions.values())
    pre_attach = sum(s.track.pre_attach for s in sessions.values())
    skipped = sum(s.track.skipped for s in sessions.values())
    confirmed = sum(s.track.confirmed for s in sessions.values())
    pending = sum(s.track.pending for s in sessions.values())
    return (
        f"unique={unique} duplicates={duplicates} reordered={reordered} stale={stale} "
        f"pre_attach={pre_attach} skipped={skipped} confirmed_lost={confirmed} "
        f"pending={pending} trailing=unknown (no authoritative final sequence)"
    )
