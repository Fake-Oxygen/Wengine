# Units

Typed physical quantities for every value crossing a subsystem boundary.
A `units::Value<Dim>` wraps a plain SI-base-unit `double` with a
compile-time `Dimension` tag: combining values derives the result
dimension automatically, and mixing dimensions (adding volts to amps)
fails to compile instead of corrupting a simulation. The module is the
engine's leaf — it depends on nothing engine-side, and every other
module (solvers, signal, world consumers) uses it for SI quantities.

Unit *text* knowledge lives in exactly one place here as well:
`units/scales.hpp` maps each dimension's accepted suffixes to SI scales
(`"mV"` → `1e-3`). Parsing machinery elsewhere (`cli/args.hpp`) holds
only thin forwarders onto these tables, never a second copy of the data,
so a new unit is one literal in `units.hpp` plus one row in `scales.hpp`
and nothing else changes. For the full dependency map, see
[Architecture](../dev/architecture.md).

## Usage pattern

Literals build typed values; `/` and `*` derive the result dimension
(volts / amps = ohms, volts × amps = watts). The program prints
`R = 4000 Ohm, P = 0.036 W`:

```cpp
#include <iostream>
#include "wengine/units/scales.hpp"
#include "wengine/units/units.hpp"

using namespace wengine::units;
using namespace wengine::units::literals;

int main() {
  const Value<ElectricPotential> u = 12.0_V;
  const Value<ElectricCurrent> i = 3.0_mA;
  const Value<ElectricResistance> r = u / i;
  const Value<Power> p = u * i;
  std::cout << "R = " << r.get() << " Ohm, P = " << p.get() << " W\n";
}
```

## Key entities

Two headers, two jobs: `units/units.hpp` owns the type system
(`Dimension`, `Value<Dim>`, arithmetic, the literal operators in
`wengine::units::literals` such as `12.0_V`, `3.0_mA`, `10.0_Ohm`), and
`units/scales.hpp` owns all suffix knowledge as `scales_for<Dim>`
tables of `SuffixScale` rows. Never introduce a `using namespace` for
the literals into a header — keep the operators function-local.

```{doxygenclass} wengine::units::Value
:project: wengine
```

```{doxygenstruct} wengine::units::Dimension
:project: wengine
```

```{doxygenstruct} wengine::units::SuffixScale
:project: wengine
```

```{doxygenstruct} wengine::units::scales_for
:project: wengine
```

Notes on the arithmetic contract: `operator==`/`!=` are exact
floating-point comparisons within one dimension — use `.get()` with a
justified epsilon where tolerance-based checks are appropriate.
`operator<<` prints the raw SI base value with no unit suffix;
callers add the context. `WValue<Dim>` is the legacy alias kept for
back-compat.

## Complete module listing

```{doxygennamespace} wengine::units
:project: wengine
```

The listing includes the nested `wengine::units::literals` operator set
and every per-dimension `scales_for<...>` specialization registered for
text parsing.
