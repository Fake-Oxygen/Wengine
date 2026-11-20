# Contract: Engine Seam — Delay Annotation (013)

**Seam**: `include/wengine/logic/netlist.hpp` (two new functions beside
`set_pull`:361 / `clear_pull`:393) and `include/wengine/cli/args.hpp`
(one new parse helper). Engine modules stay header-only; no new TU; no
struct change (`Gate::propagation_delay_ps`, types.hpp:333, is the
pre-existing V2 timing hook).

## New functions

### `set_gate_delay(Netlist &nl, slot, value_ps) -> core::Status`

- Validates the slot is live and in range; on failure returns
  `kInvalidArgument` with the netlist untouched. On success assigns
  `gates[slot].propagation_delay_ps = value_ps`.
- Legal on **every live gate kind** — combinational and sequential
  alike; the seam performs no kind check (per-kind timing semantics
  belong to the future delay evaluator, spec FR-008).
- Idempotent, last-write-wins: a re-set replaces the value
  deterministically. Zero is a legal write equal to clearing.
- `[[nodiscard]]`, `noexcept`, Doxygen block (title, parameters,
  return codes, the stored-now/honored-later note citing
  docs/logic.md §12).

### `clear_gate_delay(Netlist &nl, slot) -> core::Status`

- Validates the slot is live; **refuses loudly when the stored value is
  zero** (`kInvalidArgument`, netlist untouched) — the `clear_pull`
  precedent: an un-annotated gate has nothing to clear. On success
  assigns zero.
- `[[nodiscard]]`, `noexcept`, Doxygen block.

### `parse_time_ps(text) -> core::Result<std::uint64_t>` (cli/args.hpp)

The exact whole-picosecond time parse — a thin forwarder over
`units/scales.hpp` (spellings and scales from `scales_for<Time>`; no
suffix knowledge lives here).

## Exactness contract (spec FR-002/FR-003 — the correctness core)

- **Bare token** (`^[0-9]+$`): exact u64 decimal parse — full field
  range `0 .. 18,446,744,073,095,516,15` ps. Malformed bare tokens
  (sign, dot, exponent) refuse loudly: "bare form is integer
  picoseconds; use a suffix for other spellings".
- **Suffixed token**: numeric part must match `^[0-9]+(\.[0-9]+)?$`
  (signs/exponents refuse loudly); the suffix must match an owned
  `scales_for<Time>` spelling exactly (case-sensitive: `s sec ms us ns
  min h ps`). The matched scale must classify into the exact decimal
  factor set {1, 60, 3600, 10⁻³, 10⁻⁶, 10⁻⁹, 10⁻¹²} (double-literal
  comparison; an unclassifiable future row refuses that suffix loudly
  rather than guessing). Value = integer arithmetic:
  `num × factor × 10^(12+exp10)` over the implied fraction divisor,
  evaluated in unsigned `__int128` with an explicit u64 overflow check.
- **Loud refusals (all exit at the verb as usage errors, state
  untouched)**: negative ("delay must be non-negative"), sub-picosecond
  remainder ("X is not a whole number of picoseconds"), field overflow
  ("value exceeds the 64-bit picosecond field"), unknown/cross-dimension
  suffix ("unknown time suffix"), malformed grammar.
- **No floating point anywhere in the parse.** The result is a pure
  function of the token — deterministic across runs and platforms by
  construction, with no magnitude caps or error budgets to maintain.

## Reference anchoring (constitution III)

Goldens come from hand-derived decimal arithmetic, never from the
implementation's readback:

| Input | Independent derivation | Stored |
|---|---|---|
| `1500` | bare ps | 1500 |
| `1.5ns` | 1.5 × 10⁹ ps = 1500 | 1500 |
| `250ps` | 250 | 250 |
| `0.001ns` | 10⁶/10³ = 1 | 1 |
| `2us` | 2 × 10⁶ | 2,000,000 |
| `5ms` | 5 × 10⁹ | 5,000,000,000 |
| `1.5h` | 1.5 × 3600 × 10¹² = 5.4 × 10¹⁵ | 5,400,000,000,000,000 |
| `18446744073709551615` | u64 max | 18446744073709551615 |
| `1.5ps` | 1.5 is not whole ps | REFUSED |
| `6000h` | 2.16 × 10¹⁹ > u64 max | REFUSED |
| `1500V` | voltage, not time | REFUSED |
| `-1`, `1e3ns`, `1.5` bare | grammar | REFUSED |

## Inertness (spec FR-006 — the module's standing zero-delay rule)

- No evaluation, stepping, truth-table, export, or draw path may read
  the field to alter behavior. The reader inventory today is exactly:
  save/check/replay (persistence), the optimizer O2 reset
  (optimize_o2_emit.hpp:195), display, and tests — the feature adds no
  reader.
- Pinned by test: the same netlist with every gate annotated produces
  byte-identical solve/step/truth/export output to the un-annotated
  netlist; every pre-existing golden passes unchanged.

## Lifecycle rules (spec FR-010/FR-011 — existing behavior, pinned)

- `add_gate` zeroes the field on slot (re)use (netlist.hpp:103);
  removal/reset zero it (netlist.hpp:764): annotations die with their
  gate; recycled slots start un-annotated.
- O0/O1 optimize: preserved in place (documented, optimize.hpp:63).
- O2 optimize: reset on reshape (documented site
  optimize_o2_emit.hpp:195) — unchanged; the *report* gains a
  conditional note (contracts/cli-verbs.md).

## Verbs map failures identically (validation parity, spec FR-013)

- Unknown/mistyped gate name → `resolve_gate` failure → verb usage
  error, exit 2, workspace untouched.
- Engine `clear_gate_delay` refusal (zero field) → verb maps to
  "no delay set on `<NAME>`", exit 2 (the unpull.cpp:35-37 pattern).
- YAML replay accepts whatever the row checker accepts (u64 ≥ 0) and
  the verbs accept the same domain — both funnel through the same
  field with the same bounds (the u64 type is the contract).
