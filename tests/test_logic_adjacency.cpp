// test_logic_adjacency: cached-adjacency parity corpus vs the pre-change
// scan oracle (tests/logic_scan_reference.hpp). Links wengine_core alone
// to prove the engine-header seam.
//
// Layout: shared deterministic corpus builders first (used by every
// story phase), then one test group per task battery. Each group pins
// current behavior oracle-vs-engine; implementation tasks must keep
// every group green.
#include "wengine/logic/adjacency.hpp"
#include "wengine/logic/block_interface.hpp"
#include "wengine/logic/netlist.hpp"
#include "wengine/logic/optimize.hpp"
#include "wengine/logic/optimize_barrier.hpp"
#include "wengine/logic/optimize_o2_dedup.hpp"
#include "wengine/logic/seq_state.hpp"
#include "wengine/logic/topo.hpp"
#include "wengine/world/serialize.hpp"
#include "wengine/world/state.hpp"

#include "logic_scan_reference.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <sstream>
#include <tuple>
#include <vector>

using namespace wengine;
using wengine::logic::GateKind;
using wengine::logic::Netlist;
using wengine::logic::PinRef;
using wengine::logic::Slot;

namespace {

Slot add_ok(Netlist &nl, GateKind kind, std::uint8_t inputs) {
  auto r = logic::add_gate(nl, kind, inputs);
  assert(r.ok());
  return r.value;
}

void wire_ok(Netlist &nl, Slot from, Slot to, std::uint8_t input) {
  assert(logic::wire(nl, from, to, input).ok());
}

// Layered combinational growth: stage 0 holds dangling-input NANDs
// (block inputs); each later stage wires one input per gate from the
// previous stage in slot order.
Netlist make_layered(std::size_t stages, std::size_t width) {
  Netlist nl;
  std::vector<Slot> prev;
  for (std::size_t s = 0; s < stages; ++s) {
    std::vector<Slot> cur;
    for (std::size_t w = 0; w < width; ++w) {
      const Slot g = add_ok(nl, GateKind::kNand, 2);
      if (!prev.empty()) {
        wire_ok(nl, prev[(w * 2) % prev.size()], g, 0);
        wire_ok(nl, prev[(w * 2 + 1) % prev.size()], g, 1);
      }
      cur.push_back(g);
    }
    prev = cur;
  }
  return nl;
}

// Shared net: a marked input with primary + two extras, one NQ.
Netlist make_shared() {
  Netlist nl;
  const Slot a = add_ok(nl, GateKind::kAnd, 2);
  const Slot b = add_ok(nl, GateKind::kAnd, 2);
  const Slot f = add_ok(nl, GateKind::kDff, 2);
  const Slot c = add_ok(nl, GateKind::kOr, 3);
  assert(logic::mark_shared(nl, c, 0).ok());
  wire_ok(nl, a, c, 0); // primary
  wire_ok(nl, b, c, 0); // extra 0 (Q)
  const PinRef nq{.slot = f, .is_output = true, .input = 0, .negated = true};
  const PinRef dst{.slot = c, .is_output = false, .input = 0};
  assert(logic::wire_pins(nl, nq, dst).ok()); // extra 1 (NQ)
  return nl;
}

// Tie classes: two classes (3 + 2 members) plus a singleton.
Netlist make_tied() {
  Netlist nl;
  const Slot a = add_ok(nl, GateKind::kAnd, 2);
  const Slot b = add_ok(nl, GateKind::kOr, 2);
  const Slot c = add_ok(nl, GateKind::kNand, 3);
  const Slot solo = add_ok(nl, GateKind::kNot, 1);
  (void)solo;
  const PinRef a0{.slot = a, .is_output = false, .input = 0};
  const PinRef a1{.slot = a, .is_output = false, .input = 1};
  const PinRef b0{.slot = b, .is_output = false, .input = 0};
  const PinRef c0{.slot = c, .is_output = false, .input = 0};
  const PinRef c1{.slot = c, .is_output = false, .input = 1};
  assert(logic::tie_pins(nl, a0, b0).ok());
  assert(logic::tie_pins(nl, b0, c0).ok());
  assert(logic::tie_pins(nl, c1, a1).ok());
  return nl;
}

// Sequential: DFF with legal Q->D feedback plus a driven NOT chain.
Netlist make_seq() {
  Netlist nl;
  const Slot dff = add_ok(nl, GateKind::kDff, 2);
  const Slot n1 = add_ok(nl, GateKind::kNot, 1);
  const Slot n2 = add_ok(nl, GateKind::kNot, 1);
  wire_ok(nl, n2, dff, 0); // D driven
  wire_ok(nl, dff, n1, 0); // Q feeds chain
  wire_ok(nl, n1, n2, 0);
  return nl;
}

void test_oracle_reaches_agree() {
  Netlist nl = make_layered(4, 4);
  const std::size_t n = logic::slot_count(nl);
  std::vector<std::uint8_t> seen(n), seen2(n);
  std::vector<Slot> stack(n), stack2(n);
  for (const auto filter : {logic::EdgeFilter::kAll, logic::EdgeFilter::kCombOnly}) {
    for (Slot f = 0; f < static_cast<Slot>(n); ++f) {
      for (Slot t = 0; t < static_cast<Slot>(n); ++t) {
        const bool a = logic::scan_ref::ref_reaches(nl, f, t, filter, std::span<std::uint8_t>(seen.data(), n),
                                                    std::span<Slot>(stack.data(), n));
        const bool b = logic::topo::reaches(nl, f, t, filter, std::span<std::uint8_t>(seen2.data(), n),
                                            std::span<Slot>(stack2.data(), n));
        assert(a == b);
      }
    }
  }
}

void test_oracle_tie_agree() {
  const Netlist nl = make_tied();
  const std::size_t n = logic::slot_count(nl);
  std::vector<std::size_t> out(n * logic::kMaxInputs), out2(n * logic::kMaxInputs);
  for (Slot s = 0; s < static_cast<Slot>(n); ++s) {
    for (std::uint8_t i = 0; i < nl.slots[s].input_count; ++i) {
      const std::size_t leader = logic::tie_leader_flat(nl, logic::tie_flat(s, i));
      const std::size_t a = logic::scan_ref::ref_tie_class_members(nl, leader, out.data(), out.size());
      const std::size_t b = logic::tie_class_members(nl, leader, out2.data(), out2.size());
      assert(a == b);
      for (std::size_t k = 0; k < a && k < out.size(); ++k) {
        assert(out[k] == out2[k]);
      }
      assert(logic::scan_ref::ref_tie_group_size(nl, s, i) == logic::tie_group_size(nl, s, i));
    }
  }
  assert(logic::scan_ref::ref_tie_group_count(nl) == logic::tie_group_count(nl));
  assert(logic::scan_ref::ref_tie_pair_count(nl) == logic::tie_pair_count(nl));
}

void test_oracle_drives_shared_agree() {
  const Netlist nl = make_shared();
  for (Slot s = 0; s < static_cast<Slot>(logic::slot_count(nl)); ++s) {
    assert(logic::scan_ref::ref_drives_shared(nl, s) == logic::detail::drives_shared(nl, s));
  }
}

using AdjEdge = std::tuple<Slot, Slot, std::uint8_t, std::uint8_t>; // (driver, sink, input, extra)

bool adj_matches_tables(const Netlist &nl) {
  std::vector<AdjEdge> chained;
  for (Slot d = 0; d < static_cast<Slot>(nl.succ_head.size()); ++d) {
    for (Slot e = nl.succ_head[d]; e != logic::kNoDriver; e = nl.succ_next[e]) {
      chained.emplace_back(d, nl.succ_sink[e], nl.succ_input[e], nl.succ_extra[e]);
    }
  }
  std::vector<AdjEdge> tabled;
  for (Slot d = 0; d < static_cast<Slot>(logic::slot_count(nl)); ++d) {
    if (!nl.slots[d].live) {
      continue; // dead drivers hold no edges (sweep releases on remove)
    }
    for (Slot s = 0; s < static_cast<Slot>(logic::slot_count(nl)); ++s) {
      const auto &g = nl.slots[s];
      if (!g.live) {
        continue;
      }
      for (std::uint8_t i = 0; i < g.input_count; ++i) {
        if (g.drivers[i] == d) {
          tabled.emplace_back(d, s, i, std::uint8_t{0});
        }
        for (std::uint8_t j = 0; j < logic::shared_extra_flat(nl, logic::tie_flat(s, i)); ++j) {
          if (logic::shared_extra_driver(nl, logic::tie_flat(s, i), j) == d) {
            tabled.emplace_back(d, s, i, std::uint8_t{1});
          }
        }
      }
    }
  }
  std::sort(chained.begin(), chained.end());
  std::sort(tabled.begin(), tabled.end());
  return chained == tabled;
}

void test_adjacency_content_invariant() {
  Netlist nl;
  auto check = [&] {
    assert(adj_matches_tables(nl));
  };
  check(); // fresh pool: no edges, no chains
  const Slot a = add_ok(nl, GateKind::kNand, 2);
  const Slot b = add_ok(nl, GateKind::kNand, 2);
  const Slot c = add_ok(nl, GateKind::kOr, 2);
  const Slot d = add_ok(nl, GateKind::kOr, 2);
  const Slot f = add_ok(nl, GateKind::kDff, 2);
  check();
  wire_ok(nl, a, c, 0);
  check();
  wire_ok(nl, b, c, 1);
  check();
  wire_ok(nl, c, d, 0);
  check();
  assert(logic::mark_shared(nl, d, 1).ok());
  check();
  wire_ok(nl, a, d, 1);
  check();
  wire_ok(nl, b, d, 1); // shared extra
  check();
  const PinRef nq{.slot = f, .is_output = true, .input = 0, .negated = true};
  const PinRef dst{.slot = d, .is_output = false, .input = 1};
  assert(logic::wire_pins(nl, nq, dst).ok()); // NQ extra
  check();
  assert(logic::unwire(nl, c, 0).ok());
  check();
  assert(logic::unwire_driver(nl, a, d, 1).ok()); // primary of shared input
  check();
  assert(logic::unwire_driver(nl, b, d, 1).ok()); // extra of shared input
  check();
  assert(logic::unwire(nl, d, 1).ok()); // promotes the NQ extra to primary
  check();
  assert(logic::remove_gate(nl, b).ok());
  check();
  assert(logic::remove_gate(nl, d).ok()); // sink sweep over shared/promoted edges
  check();
  // Optimizer-adjacent surgery on a small net keeps chains exact too.
  Netlist opt = make_layered(3, 4);
  assert(adj_matches_tables(opt));
  logic::detail::rewire_users(opt, 0, 1);
  assert(adj_matches_tables(opt));
  assert(logic::remove_gate(opt, 2).ok());
  assert(adj_matches_tables(opt));
  logic::reset_netlist(nl);
  check();
}

void test_oracle_add_slots_agree() {
  Netlist ref_nl;
  Netlist live_nl;
  for (int k = 0; k < 40; ++k) {
    const GateKind kind = (k % 3 == 0) ? GateKind::kOr : GateKind::kNand;
    auto r1 = logic::scan_ref::ref_add_gate(ref_nl, kind, 2);
    auto r2 = logic::add_gate(live_nl, kind, 2);
    assert(r1.ok() == r2.ok());
    if (r1.ok()) {
      assert(r1.value == r2.value);
    }
  }
  // Same wire script on both, then full state equality.
  for (Slot s = 4; s < 40; s += 2) {
    assert(logic::wire(ref_nl, s - 4, s, 0).ok());
    assert(logic::wire(live_nl, s - 4, s, 0).ok());
  }
  assert(ref_nl == live_nl);
}

// --- US2: wire-time cycle probe parity (T011) ---------------------------------

void test_probe_pair_parity() {
  const Netlist nets[4] = {make_layered(4, 4), make_shared(), make_tied(), make_seq()};
  for (const Netlist &nl : nets) {
    const std::size_t n = logic::slot_count(nl);
    std::vector<std::uint8_t> seen(n), seen2(n);
    std::vector<Slot> stack(n), stack2(n);
    for (const auto f : {logic::EdgeFilter::kAll, logic::EdgeFilter::kCombOnly}) {
      for (Slot a = 0; a < static_cast<Slot>(n); ++a) {
        for (Slot b = 0; b < static_cast<Slot>(n); ++b) {
          const bool r = logic::scan_ref::ref_reaches(nl, a, b, f, std::span<std::uint8_t>(seen.data(), n),
                                                      std::span<Slot>(stack.data(), n));
          const bool l = logic::topo::reaches(nl, a, b, f, std::span<std::uint8_t>(seen2.data(), n),
                                              std::span<Slot>(stack2.data(), n));
          assert(r == l);
        }
      }
    }
  }
}

void test_wire_attempt_verdicts() {
  // Legal wires accept.
  {
    Netlist nl;
    const Slot x = add_ok(nl, GateKind::kNand, 2);
    const Slot y = add_ok(nl, GateKind::kNand, 2);
    assert(logic::wire(nl, x, y, 0).ok());
    assert(logic::wire(nl, x, y, 1).ok());
  }
  // Cycle-closing wire refuses; the net is untouched.
  {
    Netlist nl;
    const Slot x = add_ok(nl, GateKind::kNand, 2);
    const Slot y = add_ok(nl, GateKind::kNand, 2);
    wire_ok(nl, x, y, 0);
    const Netlist before = nl;
    assert(!logic::wire(nl, y, x, 0).ok());
    assert(nl == before);
  }
  // Cycle closing through a shared extra refuses; the net is untouched.
  {
    Netlist nl;
    const Slot x = add_ok(nl, GateKind::kNand, 2);
    const Slot y = add_ok(nl, GateKind::kOr, 2);
    assert(logic::mark_shared(nl, y, 0).ok());
    wire_ok(nl, x, y, 0);
    const Netlist before = nl;
    assert(!logic::wire(nl, y, x, 0).ok());
    assert(nl == before);
  }
  // Flop feedback accepts (Q breaks the loop).
  {
    Netlist nl;
    const Slot f = add_ok(nl, GateKind::kDff, 2);
    const Slot n = add_ok(nl, GateKind::kNot, 1);
    wire_ok(nl, f, n, 0);
    assert(logic::wire(nl, n, f, 0).ok());
  }
  // Shared append onto a marked occupied input accepts up to the cap.
  {
    Netlist nl;
    const Slot a = add_ok(nl, GateKind::kAnd, 2);
    const Slot b = add_ok(nl, GateKind::kAnd, 2);
    const Slot c = add_ok(nl, GateKind::kOr, 4);
    const Slot d = add_ok(nl, GateKind::kAnd, 2);
    const Slot e = add_ok(nl, GateKind::kAnd, 2);
    assert(logic::mark_shared(nl, c, 0).ok());
    wire_ok(nl, a, c, 0); // primary
    const std::size_t base = logic::adjacency::adj_edge_count(nl);
    wire_ok(nl, b, c, 0); // extras 0..2 (cap 4 total)
    wire_ok(nl, d, c, 0);
    wire_ok(nl, e, c, 0);
    assert(logic::adjacency::adj_edge_count(nl) == base + 3);
    const Slot f = add_ok(nl, GateKind::kAnd, 2);
    assert(!logic::wire(nl, f, c, 0).ok()); // shared net full: loud
    assert(logic::adjacency::adj_edge_count(nl) == base + 3);
  }
}

// --- US5: authoring verbs parity (T014) ---------------------------------------

void test_slot_assignment_replay() {
  Netlist ref_nl;
  Netlist live_nl;
  std::vector<Slot> ref_slots;
  std::vector<Slot> live_slots;
  auto ref_add = [&](GateKind k) {
    auto r = logic::scan_ref::ref_add_gate(ref_nl, k, 2);
    assert(r.ok());
    ref_slots.push_back(r.value);
  };
  auto live_add = [&](GateKind k) {
    auto r = logic::add_gate(live_nl, k, 2);
    assert(r.ok());
    live_slots.push_back(r.value);
  };
  for (int k = 0; k < 30; ++k) {
    ref_add((k % 2 == 0) ? GateKind::kNand : GateKind::kOr);
    live_add((k % 2 == 0) ? GateKind::kNand : GateKind::kOr);
  }
  assert(ref_slots == live_slots);
  // Remove a spread (head, middle run, tail), then re-add into recycling.
  for (const Slot s : {ref_slots[0], ref_slots[10], ref_slots[11], ref_slots[29]}) {
    std::vector<std::size_t> surv(logic::slot_count(ref_nl) * logic::kMaxInputs);
    std::vector<std::size_t> surv2(logic::slot_count(live_nl) * logic::kMaxInputs);
    assert(logic::scan_ref::ref_remove_gate(ref_nl, s, std::span<std::size_t>(surv.data(), surv.size())).ok());
    assert(logic::remove_gate(live_nl, s, std::span<std::size_t>(surv2.data(), surv2.size())).ok());
    assert(ref_nl == live_nl);
  }
  ref_slots.clear();
  live_slots.clear();
  for (int k = 0; k < 6; ++k) {
    ref_add(GateKind::kAnd);
    live_add(GateKind::kAnd);
  }
  // Recycled slots are the lowest freed, in order.
  assert(ref_slots == live_slots);
  assert((ref_slots == std::vector<Slot>{0, 10, 11, 29, 30, 31}));
  assert(ref_nl == live_nl);
}

void test_remove_release_parity() {
  // Layered net: remove drivers, sinks, and middle gates.
  {
    Netlist a = make_layered(3, 4);
    Netlist b = a;
    for (const Slot s : {Slot{0}, Slot{5}, Slot{11}}) {
      std::vector<std::size_t> sa(logic::slot_count(a) * logic::kMaxInputs);
      std::vector<std::size_t> sb(logic::slot_count(b) * logic::kMaxInputs);
      assert(logic::scan_ref::ref_remove_gate(a, s, std::span<std::size_t>(sa.data(), sa.size())).ok());
      assert(logic::remove_gate(b, s, std::span<std::size_t>(sb.data(), sb.size())).ok());
      assert(a == b);
      assert(adj_matches_tables(b));
    }
  }
  // Shared net: remove a primary driver, an extra driver, and the sink.
  {
    Netlist a = make_shared();
    Netlist b = a;
    for (const Slot s : {Slot{0}, Slot{1}, Slot{3}}) {
      std::vector<std::size_t> sa(logic::slot_count(a) * logic::kMaxInputs);
      std::vector<std::size_t> sb(logic::slot_count(b) * logic::kMaxInputs);
      assert(logic::scan_ref::ref_remove_gate(a, s, std::span<std::size_t>(sa.data(), sa.size())).ok());
      assert(logic::remove_gate(b, s, std::span<std::size_t>(sb.data(), sb.size())).ok());
      assert(a == b);
      assert(adj_matches_tables(b));
    }
  }
}

// --- US3: tie enumeration parity (T018) --------------------------------------

// Build two identical dangling-pin nets for ref-vs-live tie replay.
void make_twin_tie_nets(Netlist &ref_nl, Netlist &live_nl, std::vector<PinRef> &pins) {
  for (int k = 0; k < 6; ++k) {
    auto r1 = logic::scan_ref::ref_add_gate(ref_nl, GateKind::kAnd, 2);
    auto r2 = logic::add_gate(live_nl, GateKind::kAnd, 2);
    assert(r1.ok() && r2.ok() && r1.value == r2.value);
  }
  for (Slot s = 0; s < 6; ++s) {
    for (std::uint8_t i = 0; i < 2; ++i) {
      pins.push_back(PinRef{.slot = s, .is_output = false, .input = i});
    }
  }
}

// Tie-chain content invariant (T019): every non-singleton class
// enumerates exactly its star members, ascending, head == minimum.
bool tie_chains_match_tables(const Netlist &nl) {
  const std::size_t nflat = logic::slot_count(nl) * logic::kMaxInputs;
  for (std::size_t leader = 0; leader < nflat; ++leader) {
    if (nl.tie_head[leader] == logic::kNoDriver) {
      continue;
    }
    // Collect the reference member set by scanning.
    std::vector<std::size_t> ref;
    for (Slot s = 0; s < static_cast<Slot>(logic::slot_count(nl)); ++s) {
      const auto &g = nl.slots[s];
      if (!g.live) {
        continue;
      }
      for (std::uint8_t i = 0; i < g.input_count; ++i) {
        const std::size_t f = logic::tie_flat(s, i);
        if (logic::tie_leader_flat(nl, f) == leader) {
          ref.push_back(f);
        }
      }
    }
    if (ref.size() < 2) {
      return false; // chain exists only for real classes
    }
    std::vector<std::size_t> chained;
    for (Slot e = nl.tie_head[leader]; e != logic::kNoDriver; e = nl.tie_next[e]) {
      chained.push_back(static_cast<std::size_t>(e));
      if (chained.size() > ref.size() + 1) {
        return false; // runaway chain
      }
    }
    if (chained != ref) {
      return false;
    }
    if (chained[0] != leader) {
      return false; // head == class minimum
    }
  }
  return true;
}

void test_tie_op_parity() {
  Netlist ref_nl;
  Netlist live_nl;
  std::vector<PinRef> pins;
  make_twin_tie_nets(ref_nl, live_nl, pins);
  auto step = [&](PinRef a, PinRef b) {
    const bool ro = logic::scan_ref::ref_tie_pins(ref_nl, a, b).ok();
    const bool lo = logic::tie_pins(live_nl, a, b).ok();
    assert(ro == lo);
    assert(ref_nl == live_nl);
    assert(tie_chains_match_tables(live_nl));
  };
  step(pins[0], pins[2]); // class {0,2}
  step(pins[4], pins[6]); // class {4,6}
  step(pins[2], pins[4]); // merge -> {0,2,4,6}, leader 0
  step(pins[8], pins[10]); // class {8,10}
  step(pins[0], pins[2]); // idempotent re-tie
  step(pins[8], pins[8]); // self-tie refused
  // Member enumeration agrees after every step is covered by the
  // per-step state equality above (tie_rep is the compared truth).
}

void test_untie_parity() {
  Netlist ref_nl;
  Netlist live_nl;
  std::vector<PinRef> pins;
  make_twin_tie_nets(ref_nl, live_nl, pins);
  auto tie = [&](PinRef a, PinRef b) {
    assert(logic::scan_ref::ref_tie_pins(ref_nl, a, b).ok());
    assert(logic::tie_pins(live_nl, a, b).ok());
  };
  tie(pins[0], pins[2]);
  tie(pins[2], pins[4]);
  tie(pins[4], pins[6]); // class {0,2,4,6}
  assert(ref_nl == live_nl);
  std::vector<std::size_t> surv(logic::slot_count(ref_nl) * logic::kMaxInputs);
  std::vector<std::size_t> surv2(logic::slot_count(live_nl) * logic::kMaxInputs);
  auto untie = [&](PinRef p) {
    const bool ro = logic::scan_ref::ref_untie_pin(ref_nl, p, std::span<std::size_t>(surv.data(), surv.size())).ok();
    const bool lo = logic::untie_pin(live_nl, p, std::span<std::size_t>(surv2.data(), surv2.size())).ok();
    assert(ro == lo);
    assert(ref_nl == live_nl);
    assert(tie_chains_match_tables(live_nl));
  };
  untie(pins[2]); // non-leader removal
  untie(pins[0]); // leader removal re-elects lowest survivor
  untie(pins[6]); // two-member dissolve path
  untie(pins[1]); // not tied: refused on both
}

void test_tie_readout_parity() {
  Netlist ref_nl = make_tied();
  Netlist live_nl = make_tied();
  const std::size_t n = logic::slot_count(ref_nl);
  std::vector<std::size_t> out(n * logic::kMaxInputs), out2(n * logic::kMaxInputs);
  for (Slot s = 0; s < static_cast<Slot>(n); ++s) {
    for (std::uint8_t i = 0; i < live_nl.slots[s].input_count; ++i) {
      const std::size_t leader = logic::tie_leader_flat(ref_nl, logic::tie_flat(s, i));
      assert(leader == logic::tie_leader_flat(live_nl, logic::tie_flat(s, i)));
      const std::size_t a = logic::scan_ref::ref_tie_class_members(ref_nl, leader, out.data(), out.size());
      const std::size_t b = logic::tie_class_members(live_nl, leader, out2.data(), out2.size());
      assert(a == b);
      for (std::size_t k = 0; k < a && k < out.size(); ++k) {
        assert(out[k] == out2[k]);
      }
      assert(logic::scan_ref::ref_tie_group_size(ref_nl, s, i) == logic::tie_group_size(live_nl, s, i));
    }
  }
}

// --- US6: optimizer parity (T022) -------------------------------------------

Netlist make_opt_comb() {
  // Double negation plus a duplicated NAND feeding one AND: bypass and
  // DCE fire, but every dangling input stays live, so the block
  // interface is stable under optimization (truth comparable row-wise).
  Netlist nl;
  const Slot x0 = add_ok(nl, GateKind::kNand, 2);
  const Slot x1 = add_ok(nl, GateKind::kNand, 2);
  (void)x0;
  (void)x1; // dangling block inputs
  const Slot n1 = add_ok(nl, GateKind::kNot, 1);
  const Slot n2 = add_ok(nl, GateKind::kNot, 1);
  const Slot g1 = add_ok(nl, GateKind::kNand, 2);
  const Slot d = add_ok(nl, GateKind::kAnd, 2);
  wire_ok(nl, x0, n1, 0);
  wire_ok(nl, n1, n2, 0);
  wire_ok(nl, n2, d, 0);
  wire_ok(nl, x0, g1, 0);
  wire_ok(nl, x1, g1, 1);
  wire_ok(nl, g1, d, 1);
  return nl;
}

Netlist make_opt_seq() {
  Netlist nl;
  const Slot f = add_ok(nl, GateKind::kDff, 2);
  const Slot n = add_ok(nl, GateKind::kNot, 1);
  const Slot m = add_ok(nl, GateKind::kNand, 2); // feeds the flop (barrier)
  wire_ok(nl, m, f, 0);
  wire_ok(nl, f, n, 0);
  wire_ok(nl, n, m, 0);
  return nl;
}

void run_opt_levels(const Netlist &src, const std::vector<logic::OptLevel> &levels) {
  for (const auto lv : levels) {
    logic::OptimizeOptions opts;
    opts.level = lv;
    auto r1 = logic::optimize(src, opts);
    auto r2 = logic::optimize(src, opts);
    assert(r1.ok() == r2.ok());
    if (!r1.ok()) {
      continue; // O2 refuses shared/buffered nets loudly: refusal parity
    }
    assert(r1.value == r2.value); // deterministic fixpoint
  }
}

void test_optimize_determinism() {
  run_opt_levels(make_opt_comb(), {logic::OptLevel::kO0, logic::OptLevel::kO1, logic::OptLevel::kO2});
  run_opt_levels(make_opt_seq(), {logic::OptLevel::kO0, logic::OptLevel::kO1});
  run_opt_levels(make_shared(), {logic::OptLevel::kO0, logic::OptLevel::kO1});
  run_opt_levels(make_tied(), {logic::OptLevel::kO0, logic::OptLevel::kO1});
}

std::vector<logic::LogicValue> solve_all_rows(Netlist nl) {
  using namespace wengine::logic;
  const std::size_t n_in = block_input_count(nl);
  const std::size_t n_out = block_output_count(nl);
  assert(n_in > 0 && n_in <= 6 && n_out > 0);
  std::vector<LogicValue> in_vals(n_in), out_vals(n_out), truth;
  for (std::size_t r = 0; r < (std::size_t(1) << n_in); ++r) {
    for (std::size_t j = 0; j < n_in; ++j) {
      in_vals[j] = (((r >> j) & 1ULL) != 0ULL) ? LogicValue::kHigh : LogicValue::kLow;
    }
    assert(solve(nl, std::span<const LogicValue>(in_vals.data(), n_in),
                 std::span<LogicValue>(out_vals.data(), n_out), nullptr)
               .ok());
    truth.insert(truth.end(), out_vals.begin(), out_vals.end());
  }
  return truth;
}

void test_optimize_truth_preserved() {
  // O0 on the interface-stable net: same inputs, same outputs, same
  // truth on every row. (O1/O2 truth preservation rides on the
  // standing check_equiv suite; determinism below pins their paths.)
  Netlist src = make_opt_comb();
  const std::vector<logic::LogicValue> before = solve_all_rows(src);
  const std::size_t n_in = logic::block_input_count(src);
  const std::size_t n_out = logic::block_output_count(src);
  logic::OptimizeOptions opts;
  opts.level = logic::OptLevel::kO0;
  auto r = logic::optimize(src, opts);
  assert(r.ok());
  assert(logic::block_input_count(r.value) == n_in);
  assert(logic::block_output_count(r.value) == n_out);
  assert(solve_all_rows(r.value) == before);
}

void test_optimize_invariants() {
  Netlist src = make_opt_comb();
  const std::size_t n_in = logic::block_input_count(src);
  const std::size_t n_out = logic::block_output_count(src);
  for (const auto lv : {logic::OptLevel::kO0, logic::OptLevel::kO1}) {
    logic::OptimizeOptions opts;
    opts.level = lv;
    auto r = logic::optimize(src, opts);
    assert(r.ok());
    Netlist opt = r.value;
    assert(opt.live_count <= src.live_count); // O0/O1 never grow
    assert(logic::topo::query(opt, logic::EdgeFilter::kCombOnly).ok());
    assert(logic::block_input_count(opt) == n_in); // interface stable
    assert(logic::block_output_count(opt) == n_out);
    // Counters stay unique per kind.
    for (Slot s = 0; s < static_cast<Slot>(logic::slot_count(opt)); ++s) {
      if (!opt.slots[s].live) {
        continue;
      }
      for (Slot t = static_cast<Slot>(s + 1); t < static_cast<Slot>(logic::slot_count(opt)); ++t) {
        if (!opt.slots[t].live || opt.slots[t].kind != opt.slots[s].kind) {
          continue;
        }
        assert(opt.slots[t].counter != opt.slots[s].counter);
      }
    }
  }
}

void test_dedup_sweep_state() {
  // NAND-heavy net: repeated universal gates with shared drivers.
  // Twin run: reference all-pairs vs live implementation, per sweep.
  Netlist ref_nl;
  Netlist live_nl;
  std::vector<Slot> srcs;
  std::vector<Slot> srcs2;
  for (int k = 0; k < 4; ++k) {
    srcs.push_back(add_ok(ref_nl, GateKind::kNand, 2));
    srcs2.push_back(add_ok(live_nl, GateKind::kNand, 2));
  }
  for (int k = 0; k < 10; ++k) {
    const Slot g = add_ok(ref_nl, GateKind::kNand, 2);
    wire_ok(ref_nl, srcs[static_cast<std::size_t>(k) % srcs.size()], g, 0);
    wire_ok(ref_nl, srcs[(static_cast<std::size_t>(k) + 1) % srcs.size()], g, 1);
    const Slot h = add_ok(live_nl, GateKind::kNand, 2);
    wire_ok(live_nl, srcs2[static_cast<std::size_t>(k) % srcs2.size()], h, 0);
    wire_ok(live_nl, srcs2[(static_cast<std::size_t>(k) + 1) % srcs2.size()], h, 1);
  }
  assert(ref_nl == live_nl);
  const std::size_t n = logic::slot_count(ref_nl);
  auto feeds = std::make_unique<bool[]>(n);
  auto feeds2 = std::make_unique<bool[]>(n);
  for (std::size_t i = 0; i < n; ++i) {
    feeds[i] = false;
    feeds2[i] = false;
  }
  std::vector<std::size_t> surv(n * logic::kMaxInputs), surv2(n * logic::kMaxInputs);
  std::vector<std::uint64_t> buck2(n, 0);
  std::vector<Slot> bh2(n, logic::kNoDriver), bn2(n, logic::kNoDriver);
  int sweeps = 0;
  bool changed = true;
  bool changed2 = true;
  while ((changed || changed2) && sweeps < 10) {
    changed = logic::scan_ref::ref_dedup_universal_pass_impl(
        ref_nl, GateKind::kNand, logic::detail::MergeTied::kStrict, feeds.get(),
        std::span<std::size_t>(surv.data(), surv.size()));
    changed2 = logic::detail::dedup_universal_pass_impl(
        live_nl, GateKind::kNand, logic::detail::MergeTied::kStrict, feeds2.get(),
        std::span<std::size_t>(surv2.data(), surv2.size()), std::span<std::uint64_t>(buck2.data(), buck2.size()), std::span<Slot>(bh2.data(), bh2.size()), std::span<Slot>(bn2.data(), bn2.size()));
    assert(changed == changed2); // identical merge decisions per sweep
    assert(ref_nl == live_nl);   // identical netlist per sweep
    ++sweeps;
  }
  assert(logic::topo::query(live_nl, logic::EdgeFilter::kCombOnly).ok());
}

void test_optimizer_barriers() {
  Netlist src = make_opt_seq();
  // Flop input drivers are frozen: record them.
  std::uint8_t fanin = 0;
  Slot fdrv[8] = {};
  for (Slot s = 0; s < static_cast<Slot>(logic::slot_count(src)); ++s) {
    if (src.slots[s].live && logic::is_sequential_kind(src.slots[s].kind)) {
      fanin = src.slots[s].input_count;
      for (std::uint8_t i = 0; i < fanin; ++i) {
        fdrv[i] = src.slots[s].drivers[i];
      }
    }
  }
  for (const auto lv : {logic::OptLevel::kO0, logic::OptLevel::kO1}) {
    logic::OptimizeOptions opts;
    opts.level = lv;
    auto r = logic::optimize(src, opts);
    assert(r.ok());
    const Netlist &opt = r.value;
    for (Slot s = 0; s < static_cast<Slot>(logic::slot_count(opt)); ++s) {
      if (!opt.slots[s].live || !logic::is_sequential_kind(opt.slots[s].kind)) {
        continue;
      }
      assert(opt.slots[s].input_count == fanin);
      for (std::uint8_t i = 0; i < fanin; ++i) {
        assert(opt.slots[s].drivers[i] == fdrv[i]); // barrier: pins frozen
      }
    }
  }
}

void test_loud_refusals() {
  Netlist nl = make_layered(3, 3);
  const Netlist before = nl;
  // Undersized Kahn scratch refuses before touching the netlist: the
  // single scratch-taking core (detail::kahn_core) is the internal seam
  // the refusal tests pin — compile/query/comb_order own their scratch.
  {
    std::vector<Slot> tiny(1, 0);
    std::vector<std::uint8_t> tiny_b(1, 0);
    std::size_t n_out = 0;
    assert(!logic::topo::detail::kahn_core(nl, logic::EdgeFilter::kAll, tiny.data(), tiny.data(), n_out,
                                           std::span<std::uint8_t>(tiny_b.data(), tiny_b.size()),
                                           std::span<Slot>(tiny.data(), tiny.size()), false));
    assert(!logic::topo::detail::kahn_core(nl, logic::EdgeFilter::kCombOnly, tiny.data(), nullptr, n_out,
                                           std::span<std::uint8_t>(tiny_b.data(), tiny_b.size()),
                                           std::span<Slot>(tiny.data(), tiny.size()), true));
    assert(nl == before);
  }
  // Undersized reachability scratch answers false, never partial.
  {
    std::vector<std::uint8_t> seen(1, 0);
    std::vector<Slot> stack(1, logic::kNoDriver);
    assert(!logic::topo::reaches(nl, 0, 1, logic::EdgeFilter::kAll, std::span<std::uint8_t>(seen.data(), 1),
                                 std::span<Slot>(stack.data(), 1)));
    assert(nl == before);
  }
  // Undersized tie scratch refuses loudly.
  {
    Netlist tied = make_tied();
    const Netlist tied_before = tied;
    std::vector<std::size_t> small(2, 0);
    const logic::PinRef p{.slot = 0, .is_output = false, .input = 0};
    assert(!logic::untie_pin(tied, p, std::span<std::size_t>(small.data(), small.size())).ok());
    assert(tied == tied_before);
    std::vector<std::size_t> small2(1, 0);
    assert(!logic::remove_gate(tied, 3, std::span<std::size_t>(small2.data(), small2.size())).ok());
    assert(tied == tied_before);
  }
}

// --- US1: topology compile parity (T008) ------------------------------------

// Full Kahn drain driven by a caller-supplied drain function, recording
// the push sequence. Seed/indeg setup mirrors kahn_core exactly; only
// the per-pop successor discovery varies (reference scan vs chains).
template <typename DrainFn>
std::vector<Slot> drain_sequence(const Netlist &nl, logic::EdgeFilter f, DrainFn drain) {
  const std::size_t n = logic::slot_count(nl);
  std::vector<std::uint8_t> indeg(n, 0);
  std::vector<Slot> queue(n, logic::kNoDriver);
  const bool comb = (f == logic::EdgeFilter::kCombOnly);
  for (Slot s = 0; s < static_cast<Slot>(n); ++s) {
    const auto &g = nl.slots[s];
    if (!g.live || (comb && logic::is_sequential_kind(g.kind))) {
      continue;
    }
    const auto r = logic::topo::detail::gate_indeg(nl, s, comb);
    assert(r.ok());
    indeg[s] = r.value;
  }
  std::size_t tail = 0;
  for (Slot s = 0; s < static_cast<Slot>(n); ++s) {
    const auto &g = nl.slots[s];
    if (!g.live || (comb && logic::is_sequential_kind(g.kind))) {
      continue;
    }
    if (indeg[s] == 0) {
      queue[tail++] = s;
    }
  }
  std::vector<Slot> pushed;
  std::size_t head = 0;
  while (head < tail) {
    const Slot cur = queue[head++];
    const std::size_t before = tail;
    drain(nl, f, cur, indeg.data(), queue.data(), tail);
    for (std::size_t k = before; k < tail; ++k) {
      pushed.push_back(queue[k]);
    }
  }
  return pushed;
}

void test_drain_step_parity() {
  const Netlist nets[4] = {make_layered(4, 4), make_shared(), make_tied(), make_seq()};
  for (const Netlist &nl : nets) {
    for (const auto f : {logic::EdgeFilter::kAll, logic::EdgeFilter::kCombOnly}) {
      auto ref_drain = [](const Netlist &nn, logic::EdgeFilter ff, Slot cur, std::uint8_t *indeg, Slot *queue,
                           std::size_t &tail) {
        logic::scan_ref::ref_drain_successors(nn, ff, cur, indeg, queue, tail);
      };
      auto live_drain = [](const Netlist &nn, logic::EdgeFilter ff, Slot cur, std::uint8_t *indeg, Slot *queue,
                            std::size_t &tail) {
        logic::topo::detail::drain_successors(nn, ff, cur, indeg, queue, tail);
      };
      assert(drain_sequence(nl, f, ref_drain) == drain_sequence(nl, f, live_drain));
    }
  }
}

void test_compile_determinism() {
  const Netlist nets[4] = {make_layered(4, 4), make_shared(), make_tied(), make_seq()};
  for (const Netlist &src : nets) {
    for (const auto f : {logic::EdgeFilter::kAll, logic::EdgeFilter::kCombOnly}) {
      Netlist a = src;
      Netlist b = src;
      // Flop feedback fails kAll by design — determinism means both runs
      // agree (verdicts and, on success, orders/levels).
      const bool oka = logic::topo::compile(a, f).ok();
      const bool okb = logic::topo::compile(b, f).ok();
      assert(oka == okb);
      if (oka) {
        assert(a.topo_order == b.topo_order);
        assert(a.topo_level == b.topo_level);
      }
      assert(a == b);
    }
  }
}

void test_compile_verdicts() {
  // Pure combinational cycle refuses the closing wire.
  {
    Netlist nl;
    const Slot x = add_ok(nl, GateKind::kNand, 2);
    const Slot y = add_ok(nl, GateKind::kNand, 2);
    wire_ok(nl, x, y, 0);
    std::vector<std::uint8_t> seen(logic::slot_count(nl));
    std::vector<Slot> stack(logic::slot_count(nl));
    assert(!logic::wire(nl, y, x, 0, std::span<std::uint8_t>(seen.data(), seen.size()),
                        std::span<Slot>(stack.data(), stack.size()))
                .ok());
  }
  // Cycle closing through a shared-net extra refuses too.
  {
    Netlist nl;
    const Slot x = add_ok(nl, GateKind::kNand, 2);
    const Slot y = add_ok(nl, GateKind::kOr, 2);
    assert(logic::mark_shared(nl, y, 0).ok());
    wire_ok(nl, x, y, 0);
    std::vector<std::uint8_t> seen(logic::slot_count(nl));
    std::vector<Slot> stack(logic::slot_count(nl));
    assert(!logic::wire(nl, y, x, 0, std::span<std::uint8_t>(seen.data(), seen.size()),
                        std::span<Slot>(stack.data(), stack.size()))
                .ok());
  }
  // Flop feedback validates under the combinational filter.
  {
    Netlist nl = make_seq();
    assert(logic::topo::query(nl, logic::EdgeFilter::kCombOnly).ok());
  }
  // Hand-built dangling driver refuses loudly under kCombOnly.
  {
    Netlist nl;
    const Slot x = add_ok(nl, GateKind::kNand, 2);
    const Slot y = add_ok(nl, GateKind::kNand, 2);
    wire_ok(nl, x, y, 0);
    nl.slots[x].live = false; // defect injection: driver dies without release
    assert(!logic::topo::query(nl, logic::EdgeFilter::kCombOnly).ok());
  }
}

void test_kahn_golden() {
  // Diamond: a -> {b, c} -> d. Hand-derived Kahn order and levels.
  Netlist nl;
  const Slot a = add_ok(nl, GateKind::kNand, 2);
  const Slot b = add_ok(nl, GateKind::kNand, 2);
  const Slot c = add_ok(nl, GateKind::kNand, 2);
  const Slot d = add_ok(nl, GateKind::kNand, 2);
  wire_ok(nl, a, b, 0);
  wire_ok(nl, a, c, 0);
  wire_ok(nl, b, d, 0);
  wire_ok(nl, c, d, 1);
  assert(logic::topo::compile(nl, logic::EdgeFilter::kAll).ok());
  assert(nl.topo_order.size() == 4);
  assert(nl.topo_order[0] == a);
  assert(nl.topo_order[1] == b);
  assert(nl.topo_order[2] == c);
  assert(nl.topo_order[3] == d);
  assert(nl.topo_level[a] == 0);
  assert(nl.topo_level[b] == 1);
  assert(nl.topo_level[c] == 1);
  assert(nl.topo_level[d] == 2);
}

// --- US4: cache lifecycle (T028) ------------------------------------------------

void check_queries_agree(const Netlist &nl) {
  const std::size_t n = logic::slot_count(nl);
  // Kahn drain parity, both filters.
  for (const auto f : {logic::EdgeFilter::kAll, logic::EdgeFilter::kCombOnly}) {
    auto ref_drain = [](const Netlist &nn, logic::EdgeFilter ff, Slot cur, std::uint8_t *indeg, Slot *queue,
                        std::size_t &tail) {
      logic::scan_ref::ref_drain_successors(nn, ff, cur, indeg, queue, tail);
    };
    auto live_drain = [](const Netlist &nn, logic::EdgeFilter ff, Slot cur, std::uint8_t *indeg, Slot *queue,
                         std::size_t &tail) {
      logic::topo::detail::drain_successors(nn, ff, cur, indeg, queue, tail);
    };
    assert(drain_sequence(nl, f, ref_drain) == drain_sequence(nl, f, live_drain));
  }
  // Reachability parity (small nets: all pairs).
  if (n <= 64) {
    std::vector<std::uint8_t> seen(n), seen2(n);
    std::vector<Slot> stack(n), stack2(n);
    for (const auto f : {logic::EdgeFilter::kAll, logic::EdgeFilter::kCombOnly}) {
      for (Slot a = 0; a < static_cast<Slot>(n); ++a) {
        for (Slot b = 0; b < static_cast<Slot>(n); ++b) {
          const bool r = logic::scan_ref::ref_reaches(nl, a, b, f, std::span<std::uint8_t>(seen.data(), n),
                                                      std::span<Slot>(stack.data(), n));
          const bool l = logic::topo::reaches(nl, a, b, f, std::span<std::uint8_t>(seen2.data(), n),
                                              std::span<Slot>(stack2.data(), n));
          assert(r == l);
        }
      }
    }
  }
  // Tie readout parity + shared-feeder parity.
  std::vector<std::size_t> out(n * logic::kMaxInputs), out2(n * logic::kMaxInputs);
  for (Slot s = 0; s < static_cast<Slot>(n); ++s) {
    for (std::uint8_t i = 0; i < nl.slots[s].input_count; ++i) {
      const std::size_t leader = logic::tie_leader_flat(nl, logic::tie_flat(s, i));
      assert(logic::scan_ref::ref_tie_class_members(nl, leader, out.data(), out.size()) ==
             logic::tie_class_members(nl, leader, out2.data(), out2.size()));
    }
    assert(logic::scan_ref::ref_drives_shared(nl, s) == logic::detail::drives_shared(nl, s));
  }
  // Validation verdicts agree.
  for (const auto f : {logic::EdgeFilter::kAll, logic::EdgeFilter::kCombOnly}) {
    Netlist a = nl;
    Netlist b = nl;
    assert(logic::topo::query(a, f).ok() == logic::topo::query(b, f).ok());
  }
  // Content invariants hold.
  assert(adj_matches_tables(nl));
  assert(tie_chains_match_tables(nl));
}

void test_lifecycle_interleaved() {
  Netlist nl;
  auto check = [&] { check_queries_agree(nl); };
  check();
  const Slot a = add_ok(nl, GateKind::kNand, 2);
  const Slot b = add_ok(nl, GateKind::kNand, 2);
  const Slot c = add_ok(nl, GateKind::kOr, 2);
  check();
  wire_ok(nl, a, c, 0);
  check();
  wire_ok(nl, b, c, 1);
  check();
  assert(logic::mark_shared(nl, c, 0).ok()); // marking the occupied input is legal
  check();
  assert(logic::unshare_input(nl, c, 0).ok()); // single attachment: allowed
  check();
  const Slot f = add_ok(nl, GateKind::kDff, 2);
  (void)f;
  const Slot g = add_ok(nl, GateKind::kAnd, 2);
  const Slot h = add_ok(nl, GateKind::kAnd, 2);
  const PinRef gp0{.slot = g, .is_output = false, .input = 0};
  const PinRef hp0{.slot = h, .is_output = false, .input = 0};
  const PinRef gp1{.slot = g, .is_output = false, .input = 1};
  assert(logic::tie_pins(nl, gp0, hp0).ok());
  check();
  assert(logic::tie_pins(nl, hp0, gp1).ok());
  check();
  std::vector<std::size_t> surv(logic::slot_count(nl) * logic::kMaxInputs);
  assert(logic::untie_pin(nl, gp0, std::span<std::size_t>(surv.data(), surv.size())).ok());
  check();
  assert(logic::unwire(nl, c, 0).ok());
  check();
  assert(logic::remove_gate(nl, b).ok());
  check();
  logic::reset_netlist(nl);
  assert(nl.live_count == 0);
  assert(logic::adjacency::adj_edge_count(nl) == 0);
  check();
  // Rebuild after reset: allocation restarts at the lowest slot.
  const Slot r0 = add_ok(nl, GateKind::kNand, 2);
  assert(r0 == 0);
  check();
}

void test_checkpoint_roundtrip() {
  // Round-trips ties (persisted); shared marks/extras are outside the
  // checkpoint format's scope (pre-existing, untouched here).
  wengine::world::State src;
  src.netlist = make_layered(3, 3);
  const logic::PinRef a{.slot = 0, .is_output = false, .input = 0};
  const logic::PinRef b{.slot = 1, .is_output = false, .input = 0};
  assert(logic::tie_pins(src.netlist, a, b).ok());
  logic::init_seq_state(src.seq, src.netlist);
  src.seq_inputs.assign(logic::block_input_count(src.netlist), logic::LogicValue::kLow);
  src.seq_members.clear();
  src.seq_outputs.assign(logic::block_output_count(src.netlist), logic::LogicValue::kLow);
  std::ostringstream bytes;
  assert(wengine::world::save_state(src, bytes).ok());
  wengine::world::State dst;
  std::istringstream in(bytes.str());
  assert(wengine::world::load_state(dst, in).ok());
  assert(dst == src);
  assert(dst.netlist == src.netlist);
  assert(adj_matches_tables(dst.netlist));
  assert(tie_chains_match_tables(dst.netlist));
  check_queries_agree(dst.netlist);
}

} // namespace

int main() {
  test_oracle_reaches_agree();
  test_oracle_tie_agree();
  test_oracle_drives_shared_agree();
  test_oracle_add_slots_agree();
  test_adjacency_content_invariant();
  test_drain_step_parity();
  test_compile_determinism();
  test_compile_verdicts();
  test_kahn_golden();
  test_optimize_determinism();
  test_optimize_truth_preserved();
  test_optimize_invariants();
  test_dedup_sweep_state();
  test_optimizer_barriers();
  test_lifecycle_interleaved();
  test_checkpoint_roundtrip();
  test_loud_refusals();
  test_probe_pair_parity();
  test_wire_attempt_verdicts();
  test_slot_assignment_replay();
  test_remove_release_parity();
  test_tie_op_parity();
  test_untie_parity();
  test_tie_readout_parity();
  std::cout << "test_logic_adjacency: OK\n";
  return 0;
}
