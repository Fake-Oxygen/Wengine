# Quickstart: SR/D Latch Pair

**Feature**: `specs/004-sr-d-latches/spec.md` | **Contracts**: `contracts/session-verbs.md` | **Date**: 2026-11-01

Runnable validation scenarios. Each block runs in the headless session
(REPL, script file, or pipe) against a fresh workspace unless noted.
Expected outputs reference contract line forms (`DLATCHn.Q: a->b`,
`SRLATCHn.Q: a->b`, `peek` Q/NQ readback); exact human-readable prefixes follow
existing session conventions.

## 1. D-latch bus hold (US1 — transparent then frozen)

```text
add DLATCH
poke DLATCH1.EN=1
poke DLATCH1.D=1        # transparent: expect DLATCH1.Q: 0->1 at once (no step)
poke DLATCH1.D=0        # transparent: expect DLATCH1.Q: 1->0 at once
poke DLATCH1.D=1        # expect Q=1 again
poke DLATCH1.EN=0       # freeze at Q=1
poke DLATCH1.D=0        # frozen: expect no Q line (Q stays 1)
poke DLATCH1.D=1        # frozen: expect no Q line (Q stays 1)
step 3                  # frozen across advances: expect Q still 1
peek                    # expect DLATCH1.Q=1 NQ=0
poke DLATCH1.EN=1       # re-enable: expect Q follows current D at once
peek                    # expect Q=D
```

Pass: Q tracks D with zero advances while enabled; ignores D (drives and
steps alike) while disabled; resumes tracking on re-enable.

## 2. SR handshake + illegal refusal (US2 — set/reset/hold/illegal)

```text
add SRLATCH
peek                    # expect SRLATCH1.Q=0 NQ=1 (power-on)
poke SRLATCH1.S=1            # set: expect SRLATCH1.Q: 0->1 at once (no step)
poke SRLATCH1.S=release
peek                    # expect SRLATCH1.Q=1 (hold across release)
poke SRLATCH1.R=1            # reset: expect SRLATCH1.Q: 1->0 at once
poke SRLATCH1.R=release
peek                    # expect SRLATCH1.Q=0 (hold)
poke SRLATCH1.S=1
poke SRLATCH1.R=1            # illegal: usage error naming S=1,R=1, Q unchanged
peek                    # expect SRLATCH1.Q still 1 (state untouched)
step 2                  # (after releasing one side) hold across advances
peek                    # expect Q unchanged
```

Pass: three legal rows set/reset/hold with no advance; the fourth row is
a loud usage error (exit 2) with state untouched; replaying the script
from a fresh workspace yields identical readback.

## 3. Mixed edge + level on one workspace (US3 — shared verbs)

```text
add DFF
add SRLATCH
add DLATCH
poke DFF1.D=1
poke SRLATCH1.S=1            # expect SRLATCH1.Q=1 at once
poke DLATCH1.EN=1
poke DLATCH1.D=1        # expect DLATCH1.Q=1 at once
step 1
peek                    # expect DFF1.Q=1 (captured), SRLATCH1.Q=1 (held),
                        #   DLATCH1.Q=1 (transparent) — one cycle count
poke DLATCH1.EN=0
poke DFF1.D=0
poke DLATCH1.D=0        # frozen: DLATCH1.Q stays 1
step 1
peek                    # expect DFF1.Q=0 (re-captured), SRLATCH1.Q=1, DLATCH1.Q=1
```

Pass: each kind follows its own rule under shared verbs; level Q is never
gated by the advance; run twice from the same start → byte-identical
outputs.

## 4. Docs + errors (US4 + edge cases)

```text
add --help              # lists SRLATCH kind, S/R pins, level sentences
poke --help             # names latch pins, transparent/frozen rule, S=R=1 refusal
add SRLATCH
poke SRLATCH1.S=2            # usage error (exit 2), no state change
poke SRLATCH1.X=1            # usage error naming the bad pin
poke DFF1.S=1           # (with a DFF present) usage error naming the bad pin
wire BUF1.Y SRLATCH1.S       # legal feed wires like any other input
save /tmp/srd.yaml
reset
load /tmp/srd.yaml
peek                    # SR/DLATCH blocks, wires, ignore marks round-trip
```

Pass: help names the transparent/frozen rule and the SR table including
the illegal row; invalid drives rejected without mutation; save/load
preserves the latches; an SR-free legacy file loads and steps exactly as
before (spot-check against pre-feature output, modulo the documented
DLATCH refinement).
