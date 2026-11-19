# Logic

`logic` is the gate-level logic domain: a fixed-capacity netlist pool
(slots recycle; per-kind display counters like `AND1`/`NOT5` are
monotonic and never reused), single-driver wiring with loud
occupancy/cycle/tie refusals, a derived block interface (dangling pins
minus ignored ones, tie classes collapsed), and deterministic topology
compilation feeding zero-delay evaluation — scalar `solve`,
bit-parallel `eval_block_word` (64 truth rows per word), and the
sequential stepper for DFF/DLATCH/JK/T/SRLATCH. Fallible operations
return `core::Status`/`core::Result`; everything is header-only and
allocation-free on eval paths, with one TU exception: the YAML
workspace store (`src/logic/yaml_store.cpp`). The engine's `logic`
solver pools its state in `world::State` and steps it. Design contract
and decision log: [logic domain](../logic.md); dependency map:
[Architecture](../dev/architecture.md).

## Usage pattern

Header-only: compile with `g++ -std=c++20 -I include`, run, expect
`inputs=2 outputs=1 NOT(AND(1,0))=1` and exit status 0. Pins spell
`NAME.Y` (output) or `NAME.i` (input, 1-based in text); sequential
kinds also accept named pins (`DFF1.D`, `DFF1.CLK`, `Q`, `NQ`).

```cpp
#include "wengine/logic/block_interface.hpp"
#include "wengine/logic/eval.hpp"
#include "wengine/logic/netlist.hpp"
#include "wengine/logic/pins.hpp"
#include "wengine/logic/types.hpp"

#include <cstdio>

int main() {
  wengine::logic::Netlist nl{}; // slot pool; identity is per-kind counters
  // Build AND1 -> NOT1: block inputs AND1.1, AND1.2; block output NOT1.Y.
  const auto and1 = wengine::logic::add_gate(nl, wengine::logic::GateKind::kAnd);
  const auto not1 = wengine::logic::add_gate(nl, wengine::logic::GateKind::kNot);
  if (!and1.ok() || !not1.ok() ||
      !wengine::logic::wire_text(nl, "AND1.Y", "NOT1.1").ok()) {
    return 1;
  }

  // Derived block boundary (never stored): sizes must match exactly.
  const wengine::logic::LogicValue in[]{wengine::logic::LogicValue::kHigh,
                                        wengine::logic::LogicValue::kLow};
  wengine::logic::LogicValue out[1]{};
  if (!wengine::logic::solve(nl, in, out).ok()) {
    return 1;
  }
  std::printf("inputs=%zu outputs=%zu NOT(AND(1,0))=%d\n",
              wengine::logic::block_input_count(nl),
              wengine::logic::block_output_count(nl),
              out[0] == wengine::logic::LogicValue::kHigh ? 1 : 0);
  return (wengine::logic::block_input_count(nl) == 2 &&
          wengine::logic::block_output_count(nl) == 1 &&
          out[0] == wengine::logic::LogicValue::kHigh) ? 0 : 1;
}
```

## Key entities

Identity and ordering rules worth knowing up front: gate kinds are
append-only bytes (persisted in YAML v1 — never renumbered), the
ignored-pin rule ties ignored dangling inputs to `kLow` during eval
(wired inputs ignore the mark), and `kHighZ`/`kUnknown` exist in the
value vocabulary but fail loudly in v1 eval until tri-state buses
land. Combinational cycles are rejected at wire time (sequential Q
outputs break loops, so flop feedback validates).

```{doxygenenum} wengine::logic::GateKind
:project: wengine
```

```{doxygenenum} wengine::logic::LogicValue
:project: wengine
```

```{doxygenstruct} wengine::logic::Netlist
:project: wengine
```

```{doxygenstruct} wengine::logic::PinRef
:project: wengine
```

```{doxygenfunction} wengine::logic::wire_text
:project: wengine
```

```{doxygenfunction} wengine::logic::solve
:project: wengine
```

```{doxygenfunction} wengine::logic::block_input_count
:project: wengine
```

## Complete module listing

Everything public in the module, including the topology Module
(`logic::topo`: Kahn compile, reachability, edge filters), the
optimization passes, the layered schematic layout, the sequential
state/stepper vocabulary, the clock-domain derivation, the internals
(`logic::detail`), and the versioned YAML workspace store
(`logic::yaml` — declared here, implemented in the one engine TU that
knows yaml-cpp exists).

```{doxygennamespace} wengine::logic
:project: wengine
```
