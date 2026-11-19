# Link

`link` is the external-app transport (`wengine-link`): a versioned
seqlock shared-memory snapshot bus for realtime local consumers, plus
TCP/UDP/UART socket publishers, a compact codec, and the control-plane
client/server. `link::Frame` is the single source of truth shared by GUI
views and the wire encoder — a new solver adds one adapter function
beside it, never new view structs and never protocol changes. The
publish path performs no syscalls or I/O (adapt, encode, seqlock copy
or non-blocking send); failures are sticky `core::Status`, never
throws. Publishers attach as `emit::ISink`s owned at the app edge
(see [Emit](emit.md)); the dependency direction stays `link -> world`
(see [Architecture](../dev/architecture.md)).

The delivery policy — latest-state viewing vs. ordered bounded
streaming vs. complete recording, the four independent clocks, and the
per-cause counter vocabulary — is pinned in
[delivery guarantees](../delivery.md) and is not restated here.

## Usage pattern

Headers plus one POSIX platform TU: compile with
`g++ -std=c++20 -I include demo.cpp -Lbuild -lwengine_link_shm`, run,
expect `published=1 x=1.5` and exit status 0. Construction creates a
fresh segment (stale same-name segments are unlinked first); the
destructor unmaps, closes, and unlinks, so the segment exists exactly
while the publisher lives.

```cpp
#include "wengine/link/frame.hpp"
#include "wengine/link/shm_publisher.hpp"
#include "wengine/link/shm_reader.hpp"
#include "wengine/world/state.hpp"
#include <cstdio>

int main() {
  // Publisher: an emit::ISink owned at the app edge (never in RunConfig).
  wengine::link::ShmPublisher pub("/wengine-api-demo", "projectile");
  if (!pub.ok()) { return 1; }

  wengine::world::Snapshot snap; // what a Runner hands to every sink
  snap.step_index = 1;
  snap.sim_time = 0.01;
  snap.pos_x = {1.5};
  snap.pos_y = {2.5};
  snap.vel_x = {3.0}; // pool invariant: all four particle arrays equal
  snap.vel_y = {0.0};
  pub.on_snapshot(snap); // no syscalls: adapt -> encode -> seqlock copy

  // Reader: read-only attach; it never unlinks the segment.
  wengine::link::ShmReader reader("/wengine-api-demo");
  if (!reader.ok()) { return 1; }

  wengine::link::Frame frame; // decoded into the shared view-model
  if (!reader.read_latest_frame(frame) || frame.items.empty()) {
    return 1;
  }
  std::printf("published=%llu x=%.1f\n",
             static_cast<unsigned long long>(reader.published()), frame.items.front().x);
  return (reader.published() == 1 && frame.step == 1 && frame.items.front().x == 1.5) ? 0 : 1;
}
```

## Key entities

Attach is the version negotiation, in order: name shape (POSIX
`/name`), segment exists, size matches the publisher layout, version
commit word set, then exact match of version/capacities/domain. Wire
versions match exactly at attach with a loud found-version refusal;
version 0 is reserved as the mid-construction marker and never
assigned. Readers never unlink — the publisher owns the segment
lifetime — and a same-name publisher restart needs an explicit
re-attach: a stale mapping keeps showing the old run's frozen frames,
never corrupt data.

```{doxygenstruct} wengine::link::Frame
:project: wengine
```

```{doxygenclass} wengine::link::ShmPublisher
:project: wengine
```

```{doxygenclass} wengine::link::ShmReader
:project: wengine
```

```{doxygenenum} wengine::link::AttachState
:project: wengine
```

```{doxygenstruct} wengine::link::PublisherStats
:project: wengine
```

```{doxygenfunction} wengine::link::decode_frame
:project: wengine
```

```{doxygenfunction} wengine::link::reader_missed
:project: wengine
```

## Complete module listing

Everything public in the module, including the compact codec
(`link::compact`), the paced sample/record queue helpers
(`link::sample`), the socket internals (`link::socket_detail`), and the
UART transport (`link::uart`).

```{doxygennamespace} wengine::link
:project: wengine
```
