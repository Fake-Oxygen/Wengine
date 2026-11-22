// test_logic_input_resolution: InputResolution seam property — one span
// assembly, three narrow adapters agreeing on 0/1 nets.
//
// Covers docs/logic.md §4/§12 triplication fix: assemble_input_span owns
// primary-first compact order once; pull_map_idle owns the 012 idle map
// once; apply_span_polarity owns 4-state-preserving inversion once;
// span_has_contention owns the SolveDiag count once. Adapters keep their
// own failure vocabularies by design: scalar loud, word bit-parallel
// (multi-attachment + live TBUF refused at the entries), transient
// X/Z-transparent. The property pins agreement only on clean 0/1 nets;
// floating/contended nets are excluded (scalar fails, word refuses at
// entry, transient renders X/Z).
#include "wengine/logic/block_interface.hpp"
#include "wengine/logic/eval.hpp"
#include "wengine/logic/input_resolution.hpp"
#include "wengine/logic/netlist.hpp"
#include "wengine/logic/timing.hpp"
#include "wengine/logic/topo.hpp"
#include "wengine/logic/types.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <vector>

namespace {

using namespace wengine;
using wengine::logic::GateKind;
using wengine::logic::LogicValue;
using wengine::logic::Netlist;
using wengine::logic::Slot;

constexpr LogicValue L = LogicValue::kLow;
constexpr LogicValue H = LogicValue::kHigh;
constexpr LogicValue X = LogicValue::kUnknown;
constexpr LogicValue Z = LogicValue::kHighZ;

// Seam units: assembly order, dead flag, pull-map, polarity, contention.
void test_seam_units() {
  Netlist nl{};
  const auto a = logic::add_gate(nl, GateKind::kAnd, 2);
  const auto b = logic::add_gate(nl, GateKind::kBuf, 1);
  const auto o = logic::add_gate(nl, GateKind::kOr, 2);
  assert(a.ok() && b.ok() && o.ok());
  assert(logic::wire(nl, a.value, o.value, 0).ok());
  assert(logic::mark_shared(nl, o.value, 0).ok());
  assert(logic::wire(nl, b.value, o.value, 0).ok());

  // Primary-first compact order with stored polarities.
  logic::input_res::InputSpan span{};
  logic::input_res::assemble_input_span(nl, o.value, 0, span);
  assert(span.is_shared && span.n == 2 && !span.has_dead);
  assert(span.drv[0] == a.value && span.drv[1] == b.value);
  assert(span.inv[0] == 0 && span.inv[1] == 0);
  assert(span.flat == logic::tie_flat(o.value, 0));

  // Polarity: agrees with both apply_polarity and timing_nq_of on 0/1,
  // passes X/Z through (the inverted-Z fold fix).
  assert(logic::input_res::apply_span_polarity(L, 0) == L);
  assert(logic::input_res::apply_span_polarity(H, 0) == H);
  assert(logic::input_res::apply_span_polarity(L, 1) == H);
  assert(logic::input_res::apply_span_polarity(H, 1) == L);
  assert(logic::input_res::apply_span_polarity(L, 1) == logic::apply_polarity(L, 1));
  assert(logic::input_res::apply_span_polarity(H, 1) == logic::apply_polarity(H, 1));
  assert(logic::input_res::apply_span_polarity(X, 1) == X);
  assert(logic::input_res::apply_span_polarity(Z, 1) == Z);
  assert(logic::input_res::apply_span_polarity(Z, 0) == Z);
  assert(logic::timing_nq_of(H) == logic::input_res::apply_span_polarity(H, 1));
  assert(logic::timing_nq_of(L) == logic::input_res::apply_span_polarity(L, 1));

  // Pull-map: floating maps to idle, driven spans inert.
  assert(logic::input_res::pull_map_idle(nl, 0, Z) == Z); // unpulled: inert
  assert(logic::input_res::pull_map_idle(nl, 0, H) == H);
  assert(logic::input_res::pull_map_idle(nl, 0, X) == X);

  // Contention: low+high present, independent of X logic.
  const LogicValue agree[2] = {H, H};
  const LogicValue fight[2] = {L, H};
  assert(!logic::input_res::span_has_contention(agree, 2));
  assert(logic::input_res::span_has_contention(fight, 2));

  // Ignored predicate.
  assert(!logic::input_res::input_is_ignored(nl, o.value, 0));
  assert(logic::ignore_pin(nl, logic::PinRef{.slot = o.value, .is_output = false, .input = 1}).ok());
  assert(logic::input_res::input_is_ignored(nl, o.value, 1));
}

// Scalar vs transient agree on clean 0/1 nets (wired, shared agreement,
// dangling, ignored, pulled idle).
void test_scalar_transient_agreement() {
  Netlist nl{};
  const auto d1 = logic::add_gate(nl, GateKind::kBuf, 1);
  const auto d2 = logic::add_gate(nl, GateKind::kBuf, 1);
  const auto o = logic::add_gate(nl, GateKind::kOr, 2);
  assert(d1.ok() && d2.ok() && o.ok());
  assert(logic::wire(nl, d1.value, o.value, 0).ok());
  assert(logic::mark_shared(nl, o.value, 0).ok());
  assert(logic::wire(nl, d2.value, o.value, 0).ok());

  const std::size_t n = logic::slot_count(nl);
  std::vector<LogicValue> gate_out(n, L), cur(n, L);
  std::vector<LogicValue> resolved(n * logic::kMaxInputs, L), blk(n * logic::kMaxInputs, L);

  for (int r = 0; r < 4; ++r) {
    const LogicValue v1 = ((r >> 0) & 1) != 0 ? H : L;
    const LogicValue v2 = ((r >> 1) & 1) != 0 ? H : L;
    gate_out[d1.value] = v1;
    gate_out[d2.value] = v2;
    cur[d1.value] = v1;
    cur[d2.value] = v2;
    LogicValue s = L, t = L;
    const bool ok_s = logic::resolve_input(nl, o.value, 0, gate_out.data(), resolved.data(), s, nullptr);
    logic::timing_resolve_input(nl, o.value, 0, cur.data(), blk.data(), t);
    if (v1 == v2) {
      assert(ok_s && s == v1); // agreement resolves to the level
      assert(t == v1);         // transient agrees on 0/1
    } else {
      assert(!ok_s);  // contention fails loudly (scalar vocabulary)
      assert(t == X); // transient renders X (transient vocabulary)
    }
  }

  // Single-driver wired agrees, both polarities at the helper level.
  Netlist nw{};
  const auto b = logic::add_gate(nw, GateKind::kBuf, 1);
  const auto s2 = logic::add_gate(nw, GateKind::kAnd, 2);
  assert(b.ok() && s2.ok());
  assert(logic::wire(nw, b.value, s2.value, 0).ok());
  const std::size_t n2 = logic::slot_count(nw);
  std::vector<LogicValue> go2(n2, L), cu2(n2, L);
  std::vector<LogicValue> rs2(n2 * logic::kMaxInputs, L), bl2(n2 * logic::kMaxInputs, L);
  for (int r = 0; r < 2; ++r) {
    go2[b.value] = (r != 0) ? H : L;
    cu2[b.value] = go2[b.value];
    LogicValue s = L, t = L;
    assert(logic::resolve_input(nw, s2.value, 0, go2.data(), rs2.data(), s, nullptr));
    logic::timing_resolve_input(nw, s2.value, 0, cu2.data(), bl2.data(), t);
    assert(s == go2[b.value] && t == go2[b.value]);
  }

  // Pulled idle agrees: shared-empty pulled maps to the idle level.
  Netlist np{};
  const auto q = logic::add_gate(np, GateKind::kOr, 2);
  assert(q.ok());
  assert(logic::mark_shared(np, q.value, 0).ok());
  assert(logic::set_pull(np, q.value, 0, 1).ok()); // pull-up
  const std::size_t n3 = logic::slot_count(np);
  std::vector<LogicValue> go3(n3, L), cu3(n3, L);
  std::vector<LogicValue> rs3(n3 * logic::kMaxInputs, L), bl3(n3 * logic::kMaxInputs, L);
  LogicValue s = L, t = L;
  assert(logic::resolve_input(np, q.value, 0, go3.data(), rs3.data(), s, nullptr) && s == H);
  logic::timing_resolve_input(np, q.value, 0, cu3.data(), bl3.data(), t);
  assert(t == H);
}

// Scalar vs word agree on clean 0/1 nets: full-truth solve vs word eval.
void test_scalar_word_agreement() {
  // AND(OR(a,b), c): 3 block inputs, 1 output. No shared marks, no pulls,
  // no TBUF — the 0/1 property domain.
  Netlist nl{};
  const auto or1 = logic::add_gate(nl, GateKind::kOr, 2);
  const auto and1 = logic::add_gate(nl, GateKind::kAnd, 2);
  assert(or1.ok() && and1.ok());
  assert(logic::wire(nl, or1.value, and1.value, 0).ok());
  assert(logic::block_input_count(nl) == 3);
  assert(logic::block_output_count(nl) == 1);

  // Scalar: enumerate all 8 rows.
  std::array<LogicValue, 8> scalar{};
  for (std::uint32_t r = 0; r < 8; ++r) {
    std::vector<LogicValue> in(3, L);
    for (std::size_t j = 0; j < 3; ++j) {
      in[j] = (((r >> j) & 1u) != 0u) ? H : L;
    }
    std::vector<LogicValue> out(1, L);
    assert(logic::solve(nl, std::span<const LogicValue>(in.data(), 3), std::span<LogicValue>(out.data(), 1)).ok());
    scalar[r] = out[0];
  }
  // Hand-derived: (a|b)&c.
  for (std::uint32_t r = 0; r < 8; ++r) {
    const bool a = ((r >> 0) & 1u) != 0u;
    const bool b = ((r >> 1) & 1u) != 0u;
    const bool c = ((r >> 2) & 1u) != 0u;
    assert(scalar[r] == (((a || b) && c) ? H : L));
  }

  // Word: single 64-row slice covers all 8 rows; lane b holds row b.
  // The word entry owns its input map and Kahn scratch (CompiledOrder
  // seam) — gate words and block outputs stay caller-owned.
  const std::size_t n = logic::slot_count(nl);
  std::vector<std::uint64_t> scratch(n, 0), words(1, 0);
  assert(logic::eval_block_word(nl, 0, std::span<std::uint64_t>(scratch.data(), n),
                                std::span<std::uint64_t>(words.data(), 1))
             .ok());
  for (std::uint32_t r = 0; r < 8; ++r) {
    const bool bit = (((words[0] >> r) & 1ULL) != 0ULL);
    assert(((scalar[r] == H) ? true : false) == bit);
  }

  // Pull-only dangling still streams: up reads all-ones on the word path
  // and High on the scalar path (same seam idle rule, both domains).
  Netlist pp{};
  const auto u = logic::add_gate(pp, GateKind::kAnd, 2);
  assert(u.ok());
  assert(logic::set_pull(pp, u.value, 0, 1).ok());
  assert(logic::set_pull(pp, u.value, 1, 2).ok());
  assert(logic::block_input_count(pp) == 0); // terminated: no columns
  std::vector<LogicValue> empty_in{};
  std::vector<LogicValue> pout(1, L);
  assert(logic::solve(pp, std::span<const LogicValue>(empty_in.data(), 0), std::span<LogicValue>(pout.data(), 1)).ok());
  assert(pout[0] == L); // H&L -> L: pulls agree per-pin, gate still ANDs
}

// Intentional vocabulary divergence, pinned (not agreement): the word
// path cannot carry four values, so entries refuse loudly where the
// scalar path produces Z-then-fails and the transient path renders X/Z.
void test_word_refusal_parity() {
  Netlist nl{};
  const auto t = logic::add_gate(nl, GateKind::kTbuf, 2);
  const auto o = logic::add_gate(nl, GateKind::kOr, 2);
  assert(t.ok() && o.ok());
  assert(logic::wire(nl, t.value, o.value, 0).ok());
  assert(logic::has_buffer_gates(nl));

  // Word safety net refuses even if an entry guard were missed.
  const std::size_t n = logic::slot_count(nl);
  std::vector<std::uint64_t> scratch(n, 0);
  const std::size_t n_out = logic::block_output_count(nl);
  std::vector<std::uint64_t> words(n_out, 0);
  assert(!logic::eval_block_word(nl, 0, std::span<std::uint64_t>(scratch.data(), n),
                                 std::span<std::uint64_t>(words.data(), n_out))
              .ok());

  // Shared multi-attachment blocks the word path at the entries.
  Netlist ns{};
  const auto x1 = logic::add_gate(ns, GateKind::kBuf, 1);
  const auto x2 = logic::add_gate(ns, GateKind::kBuf, 1);
  const auto xo = logic::add_gate(ns, GateKind::kOr, 2);
  assert(x1.ok() && x2.ok() && xo.ok());
  assert(logic::wire(ns, x1.value, xo.value, 0).ok());
  assert(logic::mark_shared(ns, xo.value, 0).ok());
  assert(logic::wire(ns, x2.value, xo.value, 0).ok());
  assert(logic::shared_word_blocked(ns));
}

} // namespace

int main() {
  test_seam_units();
  std::cout << "test_logic_input_resolution: seam OK\n";
  test_scalar_transient_agreement();
  std::cout << "test_logic_input_resolution: scalar/transient OK\n";
  test_scalar_word_agreement();
  std::cout << "test_logic_input_resolution: scalar/word OK\n";
  test_word_refusal_parity();
  std::cout << "test_logic_input_resolution: OK\n";
  return 0;
}
