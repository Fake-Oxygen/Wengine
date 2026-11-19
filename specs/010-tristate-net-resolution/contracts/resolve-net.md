# Contract: `resolve_net` — the single resolution funnel

**Feature**: [spec.md](../spec.md) | **Date**: 2026-11-19
**Module**: `wengine_logic` (`include/wengine/logic/types.hpp`)
**Contract kind**: public C++ engine API (library interface)

This is the external contract of the one seam every net input
resolves through. Signature and call sites are unchanged from v1
(docs/logic.md §12: "replaces one function body; call sites
unchanged"); only the semantics of the body change.

## Signature (unchanged)

```cpp
[[nodiscard]] constexpr bool
resolve_net(const LogicValue *drivers, std::size_t n, LogicValue &out) noexcept;
```

## Semantics

Combines `n` driver contributions (`drivers[0..n-1]`) into one net
value. Resolution is total over value combinations, deterministic,
and order-independent: the result depends only on the multiset of
driver values, never on slot order (any permutation resolves
identically).

**Return value**:

- `true`, with `out` set, for every well-formed span (including
  `n == 0`).
- `false`, with `out` untouched, only for an ill-formed span
  (`drivers == nullptr` while `n > 0`). Value combination itself is
  never an error at this seam.

**Preconditions/postconditions**:

- `drivers` points to `n` readable values, or is ignored when
  `n == 0`.
- On `true`, `out` is exactly one of the four vocabulary values.

**Guarantees**: `constexpr`, `noexcept`, allocation-free,
exception-free, single pass O(n); the `n == 1` live path keeps the
v1 shape.

## Resolution table

Pairwise combination (fold over the span; commutative and
associative — IEEE 1164 `resolution` restricted to `{0,1,X,Z}`):

| resolve(a,b)   | b=`kHighZ` | b=`kLow` | b=`kHigh` | b=`kUnknown` |
|----------------|------------|----------|-----------|--------------|
| **a=`kHighZ`**   | `kHighZ`   | `kLow`   | `kHigh`    | `kUnknown`   |
| **a=`kLow`**     | `kLow`     | `kLow`   | `kUnknown` | `kUnknown`   |
| **a=`kHigh`**    | `kHigh`    | `kUnknown` | `kHigh`  | `kUnknown`   |
| **a=`kUnknown`** | `kUnknown` | `kUnknown` | `kUnknown` | `kUnknown`  |

In words: high-Z is transparent (identity); unknown dominates
low/high (absorbing); low vs high is contention → unknown;
agreement → the agreed level. Closure over a span:

| Driver multiset | Result |
|---|---|
| `{}` (no drivers) | `kHighZ` |
| all `kHighZ` | `kHighZ` |
| exactly one active level (+ any Zs) | that level |
| actives all one level (+ any Zs) | that level |
| actives with both levels | `kUnknown` |
| any `kUnknown` present | `kUnknown` |

## Examples

```text
{H}          → H      v1 passthrough, unchanged
{H, Z}       → H      enable-gated bus: disabled driver transparent
{Z}          → Z      single disabled driver: net undriven
{Z, Z}       → Z      bus with every driver off
{L, L, L}    → L      wired consensus
{L, H}       → X      contention: detected, never a picked winner
{L, H, X}    → X      conflict and X-dominance agree
{H, X}       → X      unknown is never outvoted by a level
```

## Caller obligations

- Callers may pass any span length, including zero.
- Callers that require a definite level MUST validate the resolved
  value themselves: gate inputs, sequential control nets, and block
  sinks keep rejecting `kHighZ`/`kUnknown` loudly (their behavior is
  unchanged by this contract).
- A resolved `kUnknown`/`kHighZ` is the caller's signal (contention
  vs undriven), not an error from this seam.

## Compatibility

- Every existing call site compiles and behaves identically: the
  production caller today passes exactly one `kLow`/`kHigh` driver,
  which resolves to itself.
- The vocabulary is append-only: `kLow=0, kHigh=1, kHighZ=2,
  kUnknown=3` are pinned at compile time and never renumbered.
- The v1 funnel's placeholder refusals (non-single spans, Z/X
  drivers) are superseded by this contract; the tests that pinned
  them are re-pinned to this table in the same change.
- Netlist-level multi-driver *authoring* (bus primitives,
  importers, tri-state gate kinds) is not part of this contract and
  remains a reserved follow-on slot (docs/logic.md §12).

## Verification anchors

- Independent reference: IEEE Std 1164-1993 wired resolution
  restricted to `{0,1,X,Z}` (see [research.md](../research.md) D1
  for the hand-derived semilattice).
- Test coverage: exhaustive multiset table (sizes 0..4),
  permutation invariance, v1 passthrough, ill-formed-span refusal —
  in `tests/test_logic.cpp` (see [quickstart.md](../quickstart.md)).
