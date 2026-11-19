# Emit

`emit` owns the output side of the engine: `emit::ISink` is the one
interface every consumer implements — console, CSV logs, and the
transport publishers in [Link](link.md). Sinks are best-effort,
non-throwing observers that the `core::Runner` calls with immutable
`world::Snapshot` copies; a sink never sees live mutable state, and a
sink failure never alters simulation state or timing. Routing lives at
the edge, never in the Runner, and the module obeys the dependency
direction `emit -> world` (plus the `core::Status` vocabulary) — see
[Architecture](../dev/architecture.md) for the full map.

## Usage pattern

Header-only: compile with `g++ -std=c++20 -I include`, run, expect exit
status 0. A subscription selects signal serials; serial 3 does not
exist in the snapshot, so its rows count as drops while every tick
still forwards.

```cpp
#include "wengine/emit/routing.hpp"
#include "wengine/emit/sink.hpp"
#include "wengine/world/state.hpp"

#include <cstdint>

// A consumer implements ISink; the engine never knows its consumers.
class CountingSink final : public wengine::emit::ISink {
public:
  void on_snapshot(const wengine::world::Snapshot &s) override { rows_ += s.signal_count(); }
  [[nodiscard]] std::uint64_t rows() const noexcept { return rows_; }
private:
  std::uint64_t rows_ = 0;
};

int main() {
  CountingSink inner;
  wengine::emit::SignalSubsetSink subset(inner); // edge-side routing wrapper
  subset.set_subscription({1, 3}); // 3 does not exist: those rows count dropped

  wengine::world::Snapshot snap; // what a Runner hands to every sink
  snap.step_index = 1;
  snap.sig_serial = {1, 2};
  snap.sig_value = {5.0, 9.0};
  snap.sig_quantity = {0, 0}; // QuantityId bytes; all three arrays equal

  subset.on_snapshot(snap);
  subset.on_snapshot(snap); // every tick forwards, rows stay filtered
  return (inner.rows() == 2 && subset.dropped() == 2 && subset.matched() == 2) ? 0 : 1;
}
```

## Key entities

Lifetime contract (immutable observer): the `Snapshot` reference handed
to `on_snapshot` is valid only for the duration of that call — the
Runner reuses one buffer across emissions. Copy what you need before
returning, never retain the reference, and never reenter the driving
Runner from inside `on_snapshot`. The CSV logs write doubles at
`max_digits10` precision, so they round-trip exactly and double as the
deterministic replay format; their parsers live beside the writers so
format and parser can never drift apart. Drops are loud everywhere:
`matched`/`forwarded_rows` count delivered snapshots/rows, `dropped`
counts undelivered rows (unknown or despawned serials, decimated-away
frames, muted sinks) — never silent truncation.

```{doxygenclass} wengine::emit::ISink
:project: wengine
```

```{doxygenclass} wengine::emit::SignalSubsetSink
:project: wengine
```

```{doxygenclass} wengine::emit::StrideSink
:project: wengine
```

```{doxygenclass} wengine::emit::ConsoleSink
:project: wengine
```

```{doxygenclass} wengine::emit::CsvSink
:project: wengine
```

```{doxygenclass} wengine::emit::SignalsCsvSink
:project: wengine
```

```{doxygenclass} wengine::emit::LogicCsvSink
:project: wengine
```

## Complete module listing

Everything public in the module, including the shared CSV field parsers
in `emit::csv_detail` and the three replay parsers.

```{doxygennamespace} wengine::emit
:project: wengine
```
