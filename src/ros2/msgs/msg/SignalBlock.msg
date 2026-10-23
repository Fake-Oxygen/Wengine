# Grouped signal block: one message per emitted tick (ROS2-3).
#
# Mirrors world::Snapshot signal lanes in dense slot order (see
# ros2/mapping.hpp block_from_snapshot): step/sim_time copy the Snapshot
# clocks (sim_time is the engine clock, never wall time), serial carries
# stable source serials (monotonic per State, never reused across
# despawn/respawn), value carries SI doubles, quantity carries
# signal::QuantityId codes (0 voltage/U, 1 current/I — see
# signal/types.hpp kQuantities; unknown codes never publish).
#
# Grouped companion to the RViz-friendly JointState on
# /<ns>/<signal_stem> (same payload, standard type): this message is the
# exact record (serials + quantity codes alongside values), published on
# /<ns>/<signal_stem>/block in grouped mode. Per-serial mode keeps
# single-entry JointStates per live serial and publishes no block.
uint64 step
float64 sim_time
uint64[] serial
float64[] value
uint8[] quantity
