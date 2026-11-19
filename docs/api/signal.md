# Signal

Stamped signal sources: waveform kinds, parameters, and the quantity
table that keeps every sample dimensionally labeled. The hot path is
plain SI doubles tagged with a one-byte `QuantityId`; dimensional safety
lives at the edges, where the `kQuantities` table maps each tag to its
channel suffix (`"U"`), human name, accepted spellings, and
units-owned parse scales, and `stamp()` builds a typed
`units::Value<Dim>` variant. Evaluation is closed-form over source
time — periodic kinds use exact integer cycle counting, and noise is a
seeded splitmix64 stream advanced once per step per live slot — so
value(t) is exactly reproducible for fixed inputs. The design history
and decision log live in the [signals](../signals.md) guide.

The module's types are leaves: `signal/` depends only on
[Units](units.md) and the [Core](core.md) status vocabulary, while
world, solvers, emit, CLI, and GUI consume them — never the reverse.
The source *pool* itself lives in [World](world.md)
(`world::Signals`); see [Architecture](../dev/architecture.md) for the
full map.

## Usage pattern

Evaluate a source purely and stamp the sample dimensionally, then
attach a live source to a pool and read its serial. The program prints
`ok 1, eval(t=0.125) = 1 U` and `serial 0, live 1`:

```cpp
#include <iostream>
#include "wengine/signal/types.hpp"
#include "wengine/world/state.hpp"

using namespace wengine;

int main() {
  signal::SignalParams p; // sine, 1 Hz, 1 V peak, voltage-stamped
  p.freq_hz = 2.0;
  const signal::StampedValue s = signal::stamp(p.quantity, signal::eval_signal(p, 0.125, 0.0));

  world::State state;
  const world::Entity e = state.spawn_entity();
  const core::Status st = world::configure_source(e, state.signals, p, 0.0);
  std::cout << "ok " << st.ok() << ", eval(t=0.125) = "
            << std::get<units::Value<units::ElectricPotential>>(s).get()
            << " " << signal::quantity_suffix(p.quantity) << "\n";
  std::cout << "serial " << state.signals.serial[e.index] << ", live " << state.signals.live_count << "\n";
}
```

## Key entities

```{doxygenenum} wengine::signal::SignalKind
:project: wengine
```

```{doxygenenum} wengine::signal::QuantityId
:project: wengine
```

```{doxygenstruct} wengine::signal::SignalParams
:project: wengine
```

```{doxygenstruct} wengine::signal::QuantityDesc
:project: wengine
```

```{doxygenvariable} wengine::signal::kQuantities
:project: wengine
```

```{doxygenfunction} wengine::signal::eval_signal
:project: wengine
```

```{doxygenfunction} wengine::signal::stamp
:project: wengine
```

```{doxygenstruct} wengine::signal::SignalRunConfig
:project: wengine
```

Serials are the identity contract: every source owns a stable `serial`
assigned at configure time — monotonic per `world::State` and never
reused, even across despawn/respawn (slots recycle, serials don't) —
so subscriptions, CSV `sig_id`s, Frame channels
(`sig.<serial>.U`), and the control plane address sources
unambiguously; `world::find_serial` resolves one to its live slot.
Quantity stamping is single-table by construction: adding a quantity is
one `kQuantities` row plus one `DimensionFor` line, with the
`static_assert`s failing until table, codes, and dimension bindings
agree. `kSum` sources name their operands by serial and are validated
at configure time (live non-sum operands, matching quantity — sums
compose only flattened); the solver evaluates generators first and
sums second, so sums always read fresh values, and a later-despawned
operand fails the step loudly. Custom generators extend the kind set
without scripts or allocations: `kExpression` compiles fixed RPN
programs (`signal/expr.hpp`), `kTable` replays CSV points from the
pool's append-only store (`signal/table.hpp`), and `kLua` sources carry
script bytes evaluated by the app-owned Lua extra solver — never inside
the engine core. `SignalRunConfig` is the batch section owned by this
module and aggregated by `core::RunConfig`.

## Complete module listing

```{doxygennamespace} wengine::signal
:project: wengine
```

The listing includes the nested `wengine::signal::control` namespace
(the control-plane line vocabulary) and the internal expression/table
helper namespaces.
