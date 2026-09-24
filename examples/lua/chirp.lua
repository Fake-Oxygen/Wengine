-- chirp.lua: linear frequency sweep from f0 to f1 over T seconds.
--
-- Phase is the integral of frequency, so a sweep needs the quadratic term
-- f0*t + (f1-f0)*t^2/(2*T) — something the closed expression language can
-- also do, but the named locals keep it readable as sweeps get fancier
-- (log sweeps, vibrato, FM). Pure function of t; deterministic.
--
-- Run:
--   PhysicsSimApp sig run --sig-kind lua --sig-lua examples/lua/chirp.lua \
--     --steps 2 --dt 0.25s
-- Expect: t = 0.25 s -> ~0.9239, t = 0.5 s -> -1 (quarter/three-quarter
-- points of the swept phase: 0.3125 and 0.75 cycles).

local f0, f1, T = 1, 5, 2
return math.sin(2 * math.pi * (f0 * t + (f1 - f0) * t * t / (2 * T)))
