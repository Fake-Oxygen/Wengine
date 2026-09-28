// Receiver loss accounting (PHYSI-40): reorder window, duplicates, session
// resets, pre-attach/skipped/confirmed separation, trailing-loss gating.
// Hand-derived expectations throughout (never copied from the header).

#include "wengine/link/receiver.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>

using namespace wengine;

namespace {

void test_reorder_and_duplicate() {
  // Acceptance: 1,3,2,3,4 -> four unique, one reordered, one duplicate,
  // no permanent interior loss once the window closes.
  link::SequenceTracker t;
  assert(link::receiver_observe(t, 1) == link::SeqOutcome::kInOrder);
  assert(link::receiver_observe(t, 3) == link::SeqOutcome::kAdvancedWithGap);
  assert(t.pending == 1 && t.confirmed == 0);
  assert(link::receiver_observe(t, 2) == link::SeqOutcome::kReorderedFill);
  assert(t.pending == 0 && t.confirmed == 0);
  assert(link::receiver_observe(t, 3) == link::SeqOutcome::kDuplicate);
  assert(link::receiver_observe(t, 4) == link::SeqOutcome::kInOrder);
  assert(t.unique == 4);
  assert(t.reordered == 1);
  assert(t.duplicates == 1);
  assert(t.pending == 0 && t.confirmed == 0 && t.pre_attach == 0);
  assert(t.highest == 4);
}

void test_seq_zero_ignored() {
  link::SequenceTracker t;
  assert(link::receiver_observe(t, 0) == link::SeqOutcome::kIgnored);
  assert(!t.attached && t.unique == 0);
  assert(link::receiver_observe(t, 2) == link::SeqOutcome::kInOrder);
  assert(t.pre_attach == 1 && t.unique == 1);
  assert(link::receiver_observe(t, 0) == link::SeqOutcome::kIgnored);
  assert(t.unique == 1 && t.highest == 2);
}

void test_confirmed_after_window_closes() {
  link::SequenceTracker t;
  assert(link::receiver_observe(t, 1) == link::SeqOutcome::kInOrder);
  // Jump far past the 64-slot window: newest 63 gaps stay pending, the
  // oldest 35 confirm immediately (gap 98 = 63 pending + 35 confirmed).
  assert(link::receiver_observe(t, 100) == link::SeqOutcome::kAdvancedWithGap);
  assert(t.pending == 63 && t.confirmed == 35);
  assert(t.unique == 2);
  // Seq 2 is now older than the window: stale, cannot fill the gap.
  assert(link::receiver_observe(t, 2) == link::SeqOutcome::kStale);
  assert(t.stale == 1 && t.unique == 2);
  assert(t.pending == 63 && t.confirmed == 35);
  // A gap still inside the window fills normally.
  assert(link::receiver_observe(t, 99) == link::SeqOutcome::kReorderedFill);
  assert(t.reordered == 1 && t.unique == 3);
  assert(t.pending == 62 && t.confirmed == 35);
}

void test_session_reset_on_run_change() {
  link::SessionReceiver s;
  link::SessionOutcome o1 = link::session_observe(s, 11, 5);
  assert(!o1.session_reset || s.sessions == 1); // first run attaches
  assert(s.track.highest == 5 && s.track.unique == 1);
  assert(s.track.pre_attach == 4);
  assert(link::session_observe(s, 11, 6).session_reset == false);
  assert(s.track.unique == 2 && s.sessions == 1);
  // New publisher run: cursor resets, new run's own pre-attach counted.
  const link::SessionOutcome o2 = link::session_observe(s, 22, 2);
  assert(o2.session_reset);
  assert(s.sessions == 2);
  assert(s.track.highest == 2 && s.track.unique == 1);
  assert(s.track.pre_attach == 1);
  assert(s.track.confirmed == 0 && s.track.pending == 0);
  // Old run's seqs must not leak into the new session: seq 6 is far above
  // the new highest, so it advances with a gap, not a duplicate.
  assert(link::session_observe(s, 22, 6).session_reset == false);
  assert(s.track.unique == 2 && s.track.pending == 3);
}

void test_unrelated_senders_have_separate_cursors() {
  // Two UDP senders keyed by source address: interleaved seqs must not
  // share one counter.
  link::SessionReceiver a;
  link::SessionReceiver b;
  assert(link::session_observe(a, 7, 1).seq_outcome == link::SeqOutcome::kInOrder);
  assert(link::session_observe(b, 7, 1).seq_outcome == link::SeqOutcome::kInOrder);
  assert(link::session_observe(a, 7, 2).seq_outcome == link::SeqOutcome::kInOrder);
  assert(link::session_observe(b, 7, 2).seq_outcome == link::SeqOutcome::kInOrder);
  assert(a.track.unique == 2 && b.track.unique == 2);
  assert(a.track.duplicates == 0 && b.track.duplicates == 0);
  // Same run_id from both senders still tracks independently.
  assert(link::session_observe(a, 7, 3).seq_outcome == link::SeqOutcome::kInOrder);
  assert(b.track.highest == 2 && b.track.unique == 2);
}

void test_pre_attach_latest_skips_confirmed_separated() {
  // Pre-attach history separates from loss: first seq 1416.
  link::SequenceTracker late;
  assert(link::receiver_observe(late, 1416) == link::SeqOutcome::kInOrder);
  assert(late.pre_attach == 1415 && late.confirmed == 0 && late.skipped == 0);
  assert(late.pending == 0 && late.unique == 1);
  // Intentional latest-only jump: gap counts as skipped, never confirmed.
  link::SequenceTracker v;
  assert(link::receiver_observe_latest(v, 10) == 0);
  assert(v.pre_attach == 9 && v.unique == 1);
  assert(link::receiver_observe_latest(v, 14) == 3);
  assert(v.skipped == 3 && v.confirmed == 0 && v.pending == 0);
  assert(v.unique == 2 && v.highest == 14);
  // Pending UDP gaps absorbed by a latest jump become skips as well.
  link::SequenceTracker m;
  assert(link::receiver_observe(m, 1) == link::SeqOutcome::kInOrder);
  assert(link::receiver_observe(m, 3) == link::SeqOutcome::kAdvancedWithGap);
  assert(m.pending == 1);
  assert(link::receiver_observe_latest(m, 6) == 2);
  assert(m.skipped == 1 + 2 && m.pending == 0 && m.confirmed == 0);
}

void test_trailing_only_with_authoritative_final() {
  link::SequenceTracker t;
  // Unknown final: never inferred.
  link::TrailingReport u0 = link::receiver_trailing(t, 0, false);
  assert(!u0.known && u0.missing == 0);
  assert(link::receiver_observe(t, 1) == link::SeqOutcome::kInOrder);
  assert(link::receiver_observe(t, 2) == link::SeqOutcome::kInOrder);
  const link::TrailingReport u1 = link::receiver_trailing(t, 0, false);
  assert(!u1.known);
  const link::TrailingReport u2 = link::receiver_trailing(t, 99, false);
  assert(!u2.known); // value present but not authoritative: still unknown
  // Authoritative final: exact tail loss.
  const link::TrailingReport k1 = link::receiver_trailing(t, 5, true);
  assert(k1.known && k1.missing == 3);
  const link::TrailingReport k2 = link::receiver_trailing(t, 2, true);
  assert(k2.known && k2.missing == 0);
  const link::TrailingReport k3 = link::receiver_trailing(t, 1, true);
  assert(k3.known && k3.missing == 0); // expected behind highest: no tail
}

} // namespace

int main() {
  test_reorder_and_duplicate();
  test_seq_zero_ignored();
  test_confirmed_after_window_closes();
  test_session_reset_on_run_change();
  test_unrelated_senders_have_separate_cursors();
  test_pre_attach_latest_skips_confirmed_separated();
  test_trailing_only_with_authoritative_final();
  std::cout << "test_link_receiver: OK\n";
  return 0;
}
