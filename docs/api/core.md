# Core

The fixed-step simulation backbone. `core::Runner` owns a
`world::State`, advances the clock (`sim_time`, `step_index`) in fixed
`dt` chunks, invokes every registered `core::ISolver` in registration
order, and at a decimated rate refills one reusable snapshot and hands
it to the attached sinks. `core::RunConfig` is the seam frontends
(CLI today, GUI) fill to describe a run; `core::Status` is the error
vocabulary — hot paths throw nothing, allocate nothing on the steady
path, and report failures as status codes.

The module sits between the frontends and the solver implementations:
`core/solver.hpp` depends on [World](world.md), solvers depend on
[Units](units.md), and the engine never includes frontend types. For the
full map, see [Architecture](../dev/architecture.md).

## Usage pattern

A minimal stepped solver: implement `core::ISolver` (validate `dt`,
mutate the state pools, return `Status`), register it with a `Runner`,
and drive exact step counts. The program prints
`status 0, time 1.5, x 3`:

```cpp
#include <iostream>
#include "wengine/core/runner.hpp"

using namespace wengine;

class Drift final : public core::ISolver {
public:
  std::string_view name() const noexcept override { return "drift"; }
  core::Status step(world::State &s, double dt) noexcept override {
    if (!(dt > 0.0)) return {core::StatusCode::kInvalidArgument};
    for (std::uint32_t i = 0; i < s.particles.capacity(); ++i) {
      if (s.particles.has(i)) s.particles.pos_x[i] += s.particles.vel_x[i] * dt;
    }
    return core::Status::success();
  }
};

int main() {
  Drift drift;
  core::Runner runner(core::RunnerConfig{.dt = 0.5});
  runner.add_solver(drift);
  world::Entity e = runner.state().spawn_entity();
  runner.state().particles.attach(e.index);
  runner.state().particles.vel_x[e.index] = 2.0;
  const core::Status st = runner.run(3);
  std::cout << "status " << static_cast<int>(st.code) << ", time " << runner.state().sim_time
            << ", x " << runner.state().particles.pos_x[e.index] << "\n";
}
```

## Key entities

```{doxygenclass} wengine::core::Runner
:project: wengine
```

```{doxygenclass} wengine::core::ISolver
:project: wengine
```

```{doxygenstruct} wengine::core::RunnerConfig
:project: wengine
```

```{doxygenstruct} wengine::core::RunConfig
:project: wengine
```

```{doxygenstruct} wengine::core::Status
:project: wengine
```

```{doxygenenum} wengine::core::StatusCode
:project: wengine
```

```{doxygenstruct} wengine::core::Result
:project: wengine
```

The error vocabulary, unified across the engine: `Status` is the void
fallible result; `Result<T>` is the value-or-error type carrying the
same `StatusCode` plus a zero-copy `string_view` message. `std::optional`
is reserved for absence, `bool` for predicates, and process exit codes
(`to_exit_code`) exist only at the CLI boundary. Two failure modes
differ deliberately: a solver-step failure aborts the call and is
latched — every later `run()`/`advance()` returns the stored `Status`
without invoking solvers, and there is no rollback, while the
clock-range pre-checks (`kOutOfRange`) are pure, deterministic, and not
latched. `RunConfig` stays generic: solver-scoped sections (such as
`signal::SignalRunConfig`) ride as nested structs, never flat
per-solver fields.

## Complete module listing

```{doxygennamespace} wengine::core
:project: wengine
```
