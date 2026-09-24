-- sine.lua: the canonical Lua signal — a 1 Hz sine at amplitude 2.
--
-- A Lua source is a pure function of (t, seed, step, dt) that must return
-- a number each step; `t` is source seconds since start (same time base as
-- every other generator). Only math+string+a small pure base exist here —
-- no io/os/package/debug (see docs/signals.md sec. 7).
--
-- Run:
--   PhysicsSimApp sig run --sig-kind lua --sig-lua examples/lua/sine.lua \
--     --steps 250 --dt 1ms
-- Expect: s0 ends at t = 0.25 s carrying value 2 (quarter period), exactly
-- matching `sig run --sig-kind sine --sig-freq 1Hz --sig-amp 2V`.

return 2 * math.sin(2 * math.pi * t)
