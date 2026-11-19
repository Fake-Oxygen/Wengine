# Solvers

Two solver flavors live here. Closed-form solvers are pure,
header-only calculators — static `constexpr`/`noexcept` functions over
typed [Units](units.md) values, no state, no I/O, results as SI
quantities (ohm's law, S0 pulse counters, I²C pull-up math, LDO and LED
models). Stepped solvers subclass `core::ISolver`, mutate
[World](world.md) state per fixed step, and register in the compile-time
`SolverRegistry` (`projectile`, `signal`, `logic`); factories build
default-configured instances which callers configure before handing
them to a `core::Runner`. Batch drivers (`run_headless`,
`ConfiguredRun`, the paced runner) own validation and initialization so
every frontend steps the same population in the same order. Solvers
depend on units and math, never on CLI types — see
[Architecture](../dev/architecture.md).

## Usage pattern

Closed-form call plus registry lookup: compute a resistor state from
typed values, then resolve a stepped solver by name. The program prints
`R = 4 Ohm` and the projectile solver's description:

```cpp
#include <iostream>
#include "wengine/solvers/ohm_law.hpp"
#include "wengine/solvers/solver_registry.hpp"

using namespace wengine;
using namespace wengine::units::literals;

int main() {
  const solvers::ResistorState st = solvers::OhmLawSolver::solve_from_ui(12.0_V, 3.0_A);
  std::cout << "R = " << st.resistance.get() << " Ohm\n";
  const solvers::SolverDef *def = solvers::SolverRegistry::find("projectile");
  std::cout << "solver '" << def->name << "': " << def->description << "\n";
}
```

## Key entities

```{doxygenclass} wengine::solvers::OhmLawSolver
:project: wengine
```

```{doxygenstruct} wengine::solvers::ResistorState
:project: wengine
```

```{doxygenclass} wengine::solvers::ProjectileSolver
:project: wengine
```

```{doxygenclass} wengine::solvers::SignalSolver
:project: wengine
```

```{doxygenstruct} wengine::solvers::SolverDef
:project: wengine
```

```{doxygenstruct} wengine::solvers::Registry
:project: wengine
```

```{doxygenfunction} wengine::solvers::run_headless
:project: wengine
```

```{doxygenclass} wengine::solvers::ConfiguredRun
:project: wengine
```

Contract notes: a stepped solver carries `static constexpr kName` /
`kDescription` plus a `static create()` factory, and its `step()` must
validate `dt`, skip detached pool slots, and report failures through
`core::Status` (never exceptions). The registry validates at compile
time — non-empty unique names, non-null factories — and `find()` is a
`constexpr` linear search over the small catalog. `run_headless` is the
one-shot batch driver over `ConfiguredRun` (the shared
validation/initialization path behind the CLI and the GUI controller);
`ConfiguredRun::build` rejects cross-scoped options (e.g. signal
options with a non-signal solver) and returns a run that can be driven
in chunks. `solvers::ControlBus` applies the signal control-plane
commands to a live run; wall-clock paced batching lives in
`solvers::paced`.

Part-specific datasheet calculators live in `solvers/ic/`
(`ap63205`, `ina219`, `tmc22xx`): each header quotes its formulas from
the chip's datasheet — the reference block names title, revision, and
section/equation numbers, and the tests anchor against trusted
datasheet values. The parts share one top-level `ic` CLI dispatch, so
hundreds of parts never become hundreds of commands. The full IC
catalog renders in the [complete module listing](#complete-module-listing)
below.

## Complete module listing

```{doxygennamespace} wengine::solvers
:project: wengine
```

The listing includes the nested `wengine::solvers::ic` datasheet
catalog, the `wengine::solvers::paced` run driver, and the internal
helper namespaces (`solvers::detail`, `solvers::control__detail`).
