# Physics

The shared physical-model seam. `physics/components.hpp` is the
announced home for "physics component declarations (resistor,
particle, ...)" — the models every domain shares, kept apart from the
solvers that operate on them. Today the module owns no declarations of
its own: the models it names live beside their consumers, which is why
this page renders per-header file directives instead of a namespace
listing — the Doxygen extraction of `include/wengine/` contains no
`wengine::physics` namespace. The resistor model lives in
[Solvers](solvers.md) (`solvers/ohm_law.hpp`), and particle state in
[World](world.md) (`world/particles.hpp`). For the full map, see
[Architecture](../dev/architecture.md).

## Usage pattern

Including the seam header pulls in the models it currently carries —
here the resistor model, applied as a datasheet-style closed-form
calculation. The program prints `U = 20 V, P = 40 W`:

```cpp
#include <iostream>
#include "wengine/physics/components.hpp"

using namespace wengine;
using namespace wengine::units::literals;

int main() {
  const solvers::ResistorState st = solvers::OhmLawSolver::solve_from_ir(2.0_A, 10.0_Ohm);
  std::cout << "U = " << st.voltage.get() << " V, P = " << st.power.get() << " W\n";
}
```

## Key entities

The module has no namespace-scoped entities yet; the directive below
renders the header's Doxygen extraction. The models reachable through
it are documented where they are owned —
`solvers::OhmLawSolver`/`solvers::ResistorState` on
[Solvers](solvers.md) and `world::Particles` on [World](world.md):

```{doxygenfile} components.hpp
:project: wengine
```

## Complete module listing

One header, no namespace (see the intro for why). The completeness
listing is therefore the per-header file directive:

```{doxygenfile} components.hpp
:project: wengine
```
