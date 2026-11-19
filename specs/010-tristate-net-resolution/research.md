# Research: Tri-state Net Resolution (kHighZ/kUnknown Semantics)

**Feature**: [spec.md](spec.md) | **Date**: 2026-11-19

The specification carried no `[NEEDS CLARIFICATION]` markers; every
open point had a reasonable default recorded in its Assumptions
section. Phase 0 therefore resolves the *implementation-facing*
decisions behind those defaults, each with rationale and rejected
alternatives, plus the independently derived reference table the
tests will be anchored to.

## D1: Resolution semantics — IEEE 1164 four-value wired resolution

**Decision**: `resolve_net` becomes a fold over the four-value
wired-resolution semilattice restricted to `{kLow, kHigh, kHighZ,
kUnknown}`: high-Z is the identity (transparent), unknown is
absorbing (dominates low/high), low-vs-high conflict resolves to
unknown, consensus resolves to the agreed level, and no active
drivers resolves to high-Z.

**Independent reference** (IEEE Std 1164-1993 `resolution` for
`std_logic`, restricted to the `'0'/'1'/'X'/'Z'` subset; identical to
the Verilog 4-value wire resolution for these values): pairwise
combination of two driver contributions —

| resolve(a,b) | b=kHighZ | b=kLow | b=kHigh | b=kUnknown |
|---|---|---|---|---|
| **a=kHighZ**   | kHighZ | kLow  | kHigh | kUnknown |
| **a=kLow**     | kLow   | kLow  | kUnknown | kUnknown |
| **a=kHigh**    | kHigh  | kUnknown | kHigh | kUnknown |
| **a=kUnknown** | kUnknown | kUnknown | kUnknown | kUnknown |

Hand derivation: the table is the meet-semilattice with kHighZ as
top (identity), kUnknown as bottom (absorbing), and
`kLow ∧ kHigh = kUnknown` (opposite levels are a conflict, not a
value). The operation is commutative and associative, so folding a
span in any order yields the same multiset-determined result — which
is exactly FR-009's order-independence requirement.

Values outside this vocabulary subset (IEEE 1164 `'U'` uninitialized,
`'H'/'L'/'W'` weak strengths; Verilog `tri0/tri1`, drive strengths)
have no WEngine counterpart and are excluded by design: `kUnknown` is
conflict/unknown, not uninitialized, and WEngine has no strength
axis.

**Rationale**: canonical, widely reviewed semantics that predate
this feature; matches FR-002..FR-008 exactly; associativity gives
determinism for free.

**Alternatives considered**:
- *Verilog strength model* (`wand`, `wor`, `tri0/tri1`, pull
  strengths): requires a strength axis the vocabulary does not have;
  rejected — out of scope per the spec's Assumptions.
- *Technology-specific wired-AND/wired-OR* (0- or 1-dominant bus):
  silently invents a level on conflict; rejected — violates FR-005.
- *CMOS-style pull-up idle default* (all-Z reads as a level):
  invents a driven value on an undriven net; rejected — violates
  FR-007's Z/X distinction.
- *Keep v1 refusal*: the feature exists to remove exactly that
  placeholder; rejected.

## D2: Signature and return-value contract stay; `false` reserved for ill-formed spans

**Decision**: keep
`[[nodiscard]] constexpr bool resolve_net(const LogicValue *drivers, std::size_t n, LogicValue &out) noexcept`.
New contract: returns `true` and sets `out` for every well-formed
span — including `n == 0` (→ `kHighZ`). Returns `false` only for an
ill-formed span (`drivers == nullptr` with `n > 0`), leaving `out`
untouched.

**Rationale**: docs/logic.md §12 pins the design as "replaces one
function body; call sites unchanged". The sole production caller
(block_interface.hpp:401, span of one gate-output value) keeps
compiling, keeps its `if (!resolve_net(...))` check, and keeps its
identical result. `constexpr`/`noexcept`/`[[nodiscard]]` and the
allocation-free, exception-free hot-path shape are preserved
(constitution IV/V).

**Alternatives considered**:
- *Return `LogicValue` directly*: cleaner for a total function, but
  breaks every call site and the documented §12 promise; rejected.
- *Return `std::optional<LogicValue>`*: API churn for no caller need;
  rejected.
- *Out-param enum `ResolveResult` (ok/contention/undriven)*: a new
  public type duplicating information the resolved value already
  encodes (X vs Z vs level); rejected — FR-007 requires Z and X to
  be *distinguishable*, which the value itself provides.

## D3: `n == 0` resolves to `kHighZ`, not an error

**Decision**: an empty driver span is the undriven net
(`out = kHighZ`, return `true`).

**Rationale**: FR-007 and the spec's edge-case list: "no drivers at
all" is a legitimate future state (a bus whose every driver is
detached) and the honest answer is undriven, not an integrity error.
No live caller passes `n == 0` today (block_interface passes exactly
1), so the change is invisible to existing behavior.

**Alternatives considered**: keep v1's `n != 1 → false`. Rejected — it
makes the empty-bus case inexpressible, defeating the feature's
purpose, and it conflates "no drivers" (a value question) with
"ill-formed span" (an integrity question).

## D4: Consumers keep rejecting X/Z loudly; no four-valued gate evaluation

**Decision**: `eval_gate`, `solve`/`fill_resolved` (block_interface),
sequential control nets, and the Q pool are untouched: they keep
failing loudly on X/Z exactly as today. Contention is *detected* —
the funnel answers `kUnknown`, readback paths carry it, and any
level-requiring consumer fails loudly with state/timing untouched
(FR-010, US1 scenario 2).

**Rationale**: the funnel produces; consumers validate — the
established house pattern (eval.hpp already documents "Only
kLow/kHigh flow; kHighZ/kUnknown inputs fail loudly"). The spec's
Assumptions record this as intentional.

**Alternatives considered**:
- *Four-valued (Kleene) gate evaluation* (X∧0=0, X∧1=X, ...): a
  large blast-radius change — eval_gate, the bit-parallel word
  evaluator, optimizer equivalence checks, and every golden — with
  no in-scope caller able to produce X/Z gate outputs (gates emit
  only 0/1 because their inputs are validated 0/1). The natural
  follow-on once multi-driver netlist constructs exist; rejected for
  this feature.
- *Silent X propagation through levels*: violates the loudness
  conventions; rejected.

## D5: Test strategy — re-pin plus exhaustive multiset table

**Decision**: in tests/test_logic.cpp (the funnel test region,
lines ~260-270):
1. Re-pin the five placeholder assertions (multi-driver refused; Z
   refused; X refused; n=0 refused) to the new table: `{Z}→Z`,
   `{X}→X`, `{Z,Z}→Z`, `{H,H}→H`, `{L}→L`, `{H}→H`.
2. Add exhaustive multiset coverage: every multiset of driver values
   with size 0..4 (1 empty + 4 singles + 10 pairs + 20 triples + 35
   quadruples = 70 spans) asserted against an expected-value table
   folded from the D1 pairwise matrix, with expected results written
   out independently in the test source (not computed by the
   implementation's own fold).
3. Add permutation-invariance spot checks (all distinct orderings of
   representative spans, e.g. `{L,H,Z}` and `{L,H,X,Z}`).
4. Add the ill-formed contract: `resolve_net(nullptr, 1, out)` is
   `false`.
5. Leave every existing consumer-rejection test (eval, solve, seq
   control nets, Q-pool corruption) unchanged — they pin consumer
   behavior, which this feature preserves.

**Rationale**: FR-011/SC-001..SC-003; the suite is deterministic
plain-`assert` binaries, so exhaustive small-multiset coverage is
stronger and cheaper than property-based sampling. Golden values come
from the D1 independent reference (constitution III), never from the
implementation under test.

**Alternatives considered**: randomized property tests (rejected —
non-deterministic inputs in a determinism-pinned suite; exhaustive
≤4 subsumes it for this domain); re-pinning via a
implementation-computed oracle (rejected — circular).

## D6: Documentation surfaces updated in the same change

**Decision**: four surfaces, no others:
1. `include/wengine/logic/types.hpp` — Doxygen on the `LogicValue`
   members (Z/X semantics now live, not "V2"), on `resolve_net`
   (total resolution contract, n==0, `false` meaning), keeping the
   docs/logic.md §12 cross-references accurate.
2. `docs/logic.md` §12 — the "Value system" bullet moves to shipped
   (matching the "Sequential / clocks (shipped)" precedent), and the
   "Drivers" bullet notes the funnel body now resolves four-value
   multi-driver spans while *netlist-level* multi-driver wiring
   (bus primitives, importers, tri-state gate kinds) remains the
   reserved follow-on slot.
3. `docs/api/logic.md` — the funnel-contract paragraph (currently
   "fail loudly in v1 eval until tri-state buses") updated: the
   funnel resolves Z/X; consumers still reject them.
4. `README.md` module table, `logic` row — add the four-value
   resolution funnel to the module description.

**Rationale**: constitution Development Workflow (Doxygen ships with
public-API changes; README is a living specification; the generated
site fails loudly on API coverage gaps).

**Alternatives considered**: types.hpp-only (rejected — stale
§12/api/README contracts would contradict the shipped code);
retiring the §12 slot entirely (rejected — the wiring-level slot
stays open by design; only the funnel semantics slot closes).

## D7: Compile-time vocabulary pinning

**Decision**: add
`static_assert(static_cast<int>(LogicValue::kHighZ) == 2 && static_cast<int>(LogicValue::kUnknown) == 3)`
beside the enum so the append-only numbering is enforced at compile time.

**Rationale**: the spec's edge case "the value vocabulary itself
never changes"; zero runtime cost; consistent with the project's
existing compile-time pinning style (registry-size
`static_assert`s).

**Alternatives considered**: comment-only (weaker — a renumber would
compile silently and corrupt persisted vocabulary semantics).

## Caller inventory (blast-radius audit)

Every `resolve_net` call site in the tree:

| Caller | Span today | After the change |
|---|---|---|
| `block_interface.hpp:401` (tie-class sink) | exactly 1, a gate-output value (always kLow/kHigh) | identical result, unchanged code |
| `tests/test_logic.cpp:260-270` (funnel tests) | 0/1/2 drivers incl. Z/X | re-pinned to the new table (D5) |

No other production code calls the funnel: `eval_gate` takes
already-resolved input values, `solve`/`fill_resolved` validate
inputs at their own seams, and the sequential stepper validates Q
pool contents itself (seq_step.hpp:298 comment already anticipates a
future tri-state Q failing loudly there). The blast radius is
therefore exactly: one function body, its doc comments, one test
region, and the four documentation surfaces.

## Resolution: all decisions closed

No `[NEEDS CLARIFICATION]` remains; every decision above is grounded
in the spec's assumptions and the constitution. Phase 1 may proceed.
