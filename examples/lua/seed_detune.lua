-- seed_detune.lua: one script, many voices — `seed` is the spawn serial.
--
-- Batch runs spawn identical sources, but each source sees its own `seed`
-- (0, 1, 2, ...), so a single script fans out into a detuned stack. This
-- is the scripting counterpart of noise's seed+k derivation.
--
-- Run:
--   PhysicsSimApp sig run --sig-kind lua --sig-lua examples/lua/seed_detune.lua \
--     --sig-count 2 --steps 1 --dt 1ms
-- Expect: two sources with DIFFERENT values (s0 phase 0, s1 phase 1 rad).

return math.sin(2 * math.pi * 440 * t + seed)
