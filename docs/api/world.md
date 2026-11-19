# World

The live simulation state and its snapshots. `world::State` owns the
clock, an entity registry (stable slot index + generation identity), and
per-domain SoA component pools — parallel arrays with occupancy markers,
indexed by entity slot, holes skipped — currently particles and signal
sources, plus the sequential-logic pool as a singleton. Snapshots are
deep, dense copies in slot order: sinks never hold references whose
lifetime depends on mutable simulation state. `State` is standalone —
no engine dependencies — so [Core](core.md) mutates it, solvers write
it, and the emit/link edges consume snapshots adapted from it. Signal
pool semantics (kinds, serials, quantity stamping) are specified on
[Signal](signal.md); for the full map see
[Architecture](../dev/architecture.md).

## Usage pattern

Spawn an entity, attach a particle, and take an owning snapshot copy:
the snapshot keeps its values after further state mutation. The program
prints `live 1, snap x 1, state x 99, id (0,0)`:

```cpp
#include <iostream>
#include "wengine/world/state.hpp"

using namespace wengine;

int main() {
  world::State state;
  world::Entity e = state.spawn_entity();
  state.particles.attach(e.index);
  state.particles.pos_x[e.index] = 1.0;
  state.particles.vel_x[e.index] = 0.5;

  const world::Snapshot snap = world::take_snapshot(state);
  state.particles.pos_x[e.index] = 99.0; // snapshots are deep copies

  std::cout << "live " << snap.particle_count() << ", snap x " << snap.pos_x[0]
            << ", state x " << state.particles.pos_x[e.index] << ", id ("
            << snap.particle_index[0] << "," << snap.particle_generation[0] << ")\n";
}
```

## Key entities

```{doxygenstruct} wengine::world::State
:project: wengine
```

```{doxygenstruct} wengine::world::Snapshot
:project: wengine
```

```{doxygenstruct} wengine::world::Particles
:project: wengine
```

```{doxygenstruct} wengine::world::Signals
:project: wengine
```

```{doxygenstruct} wengine::world::Entity
:project: wengine
```

```{doxygenclass} wengine::world::EntityRegistry
:project: wengine
```

```{doxygenfunction} wengine::world::take_snapshot
:project: wengine
```

```{doxygenfunction} wengine::world::fill_snapshot
:project: wengine
```

Contract notes for the pool and snapshot types: `spawn_entity()` touches
the registry only — attaching components (e.g. `particles.attach(...)`)
is opt-in per pool. `despawn_entity()` detaches every pool before
releasing the slot, and the bumped generation guarantees a stale handle
never aliases a recycled slot. The snapshot contract is slot order,
always: `take_snapshot` is the owning-copy convenience for callers that
retain the result, while `fill_snapshot` refills a caller-owned buffer
reusing its capacities — the Runner's emission path stays
allocation-free after warmup. `Snapshot` carries dense particle identity
(`particle_index` + `particle_generation` per live slot), and the
`select_signals` family filters by source serial at the edge. Versioned
binary persistence lives in `world/serialize.hpp`
(`save_state`/`load_state`, current format v6 with fallback loaders for
v1–v5 files) — checkpoints, distinct from snapshot CSV output.

## Complete module listing

```{doxygennamespace} wengine::world
:project: wengine
```

The nested `wengine::world::detail` namespace holds the little-endian
byte helpers backing the binary state format — internal plumbing,
rendered here so nothing undocumented can hide.
