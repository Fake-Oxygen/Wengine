-- burst.lua: gated 440 Hz tone — 0.1 s bursts every 0.5 s with sin^2 edges.
--
-- Shows branching (`if`) and `math.floor` for periodic gating. The sin^2
-- window keeps burst edges click-free; the window argument stays in
-- [0, pi], so the fractional `^2` never sees a negative base. Each burst
-- restarts from the same phase (pure function of t), so replay is exact.
--
-- Run:
--   PhysicsSimApp sig run --sig-kind lua --sig-lua examples/lua/burst.lua \
--     --steps 600 --dt 1ms
-- Expect: silence outside each 0.1 s window, a smooth 440 Hz burst inside;
-- at t = 0.6 s the source sits between bursts (value 0).

local f, period, width = 440, 0.5, 0.1
local ph = t - math.floor(t / period) * period
if ph < width then
  return math.sin(2 * math.pi * f * t) * (math.sin(math.pi * ph / width) ^ 2)
end
return 0
