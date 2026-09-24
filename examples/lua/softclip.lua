-- softclip.lua: waveshaper — an overdriven 220 Hz sine through x/(1+|x|).
--
-- Shows stateless nonlinear shaping: the output always lands in (-1, 1) no
-- matter the drive, so Hot samples can never leak inf/nan downstream (and
-- if they somehow did, the runtime rejects non-finite results loudly).
-- Lua 5.4 math has no tanh; the rational soft clip needs only abs.
--
-- Run:
--   PhysicsSimApp sig run --sig-kind lua --sig-lua examples/lua/softclip.lua \
--     --steps 3 --dt 1ms
-- Expect: t = 0.003 s -> ~-0.717 (clipped negative lobe, |value| < 1).

local x = 3 * math.sin(2 * math.pi * 220 * t)
return x / (1 + math.abs(x))
