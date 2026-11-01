# Quickstart: JK/T Flop Kinds

**Feature**: `specs/003-jk-t-flops/spec.md` | **Contracts**: `contracts/session-verbs.md` | **Date**: 2026-11-01

Runnable validation scenarios. Each block runs in the headless session
(REPL, script file, or pipe) against a fresh workspace unless noted.
Expected outputs reference contract line forms (`JKn.Q: a->b`,
`Tn.Q: a->b`, `peek` Q/NQ readback); exact human-readable prefixes follow
existing session conventions.

## 1. T divider (US1 — toggle/hold + divide-by-2 chain)

```text
add T
add T
poke T1.T=1
poke T2.T=1
step 4                  # T1.Q toggles 4x, T2.Q toggled by chaining (see 2)
peek                    # expect T1.Q back at 0 after even toggles
poke T1.T=0
step 3                  # T1.Q unchanged across all 3 (hold row)
peek                    # expect T1.Q still 0, NQ=1
```

Pass: single flop alternates Q every cycle while T=1; freezes while T=0.

## 2. Ripple divide-by-4 (US1 — chained stages)

```text
add T
add T
wire T1.Q T2.T
poke T1.T=1
step 4
peek                    # expect T1.Q=0 (4 toggles), T2.Q=0 (2 toggles: 0->1->0)
```

Pass: second stage advances at half the rate of the first; replaying the
same script from a fresh workspace yields identical readback.

## 3. JK truth table (US2 — all four rows on one flop)

```text
add JK
poke JK1.J=0
poke JK1.K=0
step 3                  # hold: expect JK1.Q unchanged (no-change lines)
poke JK1.J=1
poke JK1.K=0
step 1                  # set: expect JK1.Q: 0->1
poke JK1.J=0
poke JK1.K=1
step 1                  # reset: expect JK1.Q: 1->0
poke JK1.J=1
poke JK1.K=1
step 1                  # toggle: expect JK1.Q: 0->1
step 1                  # toggle: expect JK1.Q: 1->0
peek                    # expect JK1.Q=0 NQ=1
```

Pass: each J/K combination produces the documented row (hold / set /
reset / toggle / toggle).

## 4. 2-bit JK counter (US2 — wrap-around sequence)

```text
add JK
add JK
# (wire toggle/carry per the session's existing wire spelling)
poke JK1.J=1
poke JK1.K=1
step 4
peek                    # expect count sequence 00 -> 01 -> 10 -> 11 -> 00
```

Pass: four advances walk the full 2-bit cycle and wrap to zero.

## 5. Mixed DFF + JK + T on one clock (US3 — shared stepper)

```text
add DFF
add JK
add T
poke DFF1.D=1
poke JK1.J=1
poke JK1.K=1
poke T1.T=1
step 2
peek                    # expect DFF1.Q=1 (captured), JK1.Q=0 (toggled twice),
                        #   T1.Q=0 (toggled twice) — one shared cycle count
```

Pass: each kind follows its own table under one advance; run twice from
the same start → byte-identical outputs.

## 6. Docs + errors (US4 + edge cases)

```text
help logic              # lists JK/T kinds, pin tables, excitation sentences
add JK
poke JK1.J=2            # usage error (exit 2), no state change
poke DFF1.J=1           # (with a DFF present) usage error naming the bad pin
save /tmp/jkt.yaml
reset
load /tmp/jkt.yaml
peek                    # JK/T blocks, wires, ignore marks round-trip
```

Pass: help names both tables; invalid drives rejected without mutation;
save/load preserves the new kinds; a legacy file without JK/T loads and
steps exactly as before (spot-check against pre-feature output).
