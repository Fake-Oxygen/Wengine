# Typed domain outputs vs visualization Frames

Status: contract (design/documentation). Depends on `docs/delivery.md`
(PHYSI-28); concrete template is `link/sample_block.hpp` (PHYSI-44) plus
the bounded paging chain (`link/output_queue.hpp`, `link/tcp_worker.hpp`,
`link/record_stream.hpp`).

## 1. Problem

`link/frame.hpp` currently promises that every future domain fits spatial
glyphs plus named scalar channels. That promise is retired by this
document. It is a useful *view model* but it does not represent mesh
topology, rigid-body orientation/constraints, or bulk tensor fields, and
it must not define the engine's scientific data limits.

Rule going forward:

- `link::Frame` is **one lossy view projection** for points-plus-scalars
  displays and the fixed-capacity SHM bus. New domains reuse it only when
  the projection is faithful (sec. 5).
- Each domain owns its **typed numeric output block(s)** for science
  traffic, its **checkpoint columns** for restart, and its **projection**
  into views. No general plugin framework is introduced (sec. 6).

Delivery classes, clocks, counters, and CSV-vs-checkpoint roles are
defined once in `docs/delivery.md` and reused verbatim here.

## 2. Three separate things

| Layer | Question it answers | Owner | Examples |
|---|---|---|---|
| Checkpoint | How do I restart bit-exactly? | domain pool + `world/serialize.hpp` | RNG state, `t0`, serials, table/Lua stores |
| Output block | What numbers came out this tick? | domain codec (`link/*block*.hpp`) | `SampleBlock`, particle/field blocks below |
| View projection | What does the operator see now? | adapter `Snapshot -> Frame` | `frame_from_snapshot`, `frame_from_signal_snapshot` |

Conflating them caused the old promise. They evolve independently:

- Checkpoint versions bump with fallback loaders; wire versions
  exact-match at attach and refuse loudly.
- Output blocks are bounded, owned, numeric; names live elsewhere.
- Views select a projection and may drop/coalesce; that never affects
  restart or the authoritative sample stream.

## 3. Typed output-block contract

Every domain output block follows the `SampleBlock` shape:

1. **Stable IDs, numeric.** `u64` serials (signals), `(u32 index, u32
   generation)` pairs (particles/entities), node/element `u32/u64` IDs
   (mesh). Dense slot order never substitutes for an ID.
2. **Quantity/dimension, numeric.** `u8` quantity code validated against
   the domain table (`signal::QuantityId`, `kQuantityCount`); dimensional
   binding (`DimensionFor`, `units::Dimension`) lives at parse/display
   edges. The hot path carries SI `double` + tag only.
3. **Shape, explicit.** SoA vectors + `channel_count`/`sample_count` (or
   `node_count`, `component_count`) with row-major values. No pointers,
   no strings, no padding in the codec spec.
4. **Schema version, exact-match.** Magic + schema `u32` + record `kind`
   `u8` + `run_id` + monotonic `seq` + NUL-terminated domain tag, same
   family as `compact_codec.hpp`. Readers support exactly one schema and
   refuse everything else loudly; version 0 is reserved (mid-construction).
5. **Bounded ownership.** Compile-time `k*Max` counts + byte cap (one
   datagram-sized maximum, 65507 B today). Builder seals on
   count/byte/schema-change; queue/worker refuse-or-coalesce loudly with
   per-cause counters. Hot encode is `noexcept`, allocation-free, bounded
   copies, validates-before-touching-output; decode may allocate (cold
   reader side only). Transports never silently truncate.

Names and static metadata never ride the hot path (sec. 4).

### 3a. Worked example: signal block (shipped, PHYSI-44)

`link/sample_block.hpp`: schema once per block + numeric sample arrays.

```text
schema = serial[u64] x C + quantity[u8] x C        (C <= 1024)
sample = step[u64] + time[f64] + value[f64] x C    (S <= 1024, total <= 65507 B)
```

Schema seals on serial/quantity/order change (add/drop/respawn all seal,
so `flatten(batch(stream)) == stream` exactly). Empty selections latch an
empty schema and still record every tick with zero values, so reader lag
stays exact. Names (`sig.<serial>.U`) exist only in the `Frame`/CSV/control
projection, never on this wire.

### 3b. Worked example: particle set block (pattern for a new domain)

Same contract, particle-shaped (illustrative layout, not shipped):

```text
header = magic + schema(=N) + kind(=particle) + run_id + block_seq
         + domain("projectile") + point_count[C] + sample_count[S]
schema = entity_index[u32] x C + entity_generation[u32] x C
sample = step[u64] + time[f64] + x[f64] x C + y[f64] x C
         + vx[f64] x C + vy[f64] x C
bounds = C <= 1024, S bounded by byte cap, total <= 65507 B
```

Stable IDs are the `(index, generation)` pair (cf. `Snapshot`
`particle_index`/`particle_generation`); `has_identity` false is
unavailable, never fabricated from dense order. No `z`/`color`/`size`
glyph hints here — those are view fields. A run with no live particles
records zero-count ticks (observability, same as the signal empty-schema
rule).

### 3c. Worked example: field/mesh block (pattern for topology)

Bulk fields need topology references plus paging, not glyphs:

```text
header = magic + schema(=N) + kind(=field) + run_id + block_seq
         + domain("em"/"fea") + mesh_id[u64] + field_quantity[u8]
         + node_count[C] + sample_count[S]
schema = node_id[u64] x C            (dense run or explicit IDs; order fixed per seal)
sample = step[u64] + time[f64] + value[f64] x C x K   (K = components: 1 scalar, 3 vector, 6 tensor)
topology = NOT per tick: mesh connectivity (elements, node coordinates)
           ships once via checkpoint + control-plane/schema edge, referenced by mesh_id
bounds = C x S x K <= byte cap; one tick that cannot fit one datagram
         is refused loudly (kOutOfRange + required bytes), never split silently;
         the packetizer pages node ranges across consecutive blocks
```

Static mesh (node coordinates, element connectivity, material tags) is
schema/control-edge metadata: versioned in the checkpoint, addressable by
`mesh_id`, never repeated per sample. Orientation/constraints
(rigid-body quaternion, joint graph) follow the same split: compact
numeric state per tick + static graph at the edge. Reducing a quaternion
to `x,y,size` glyph hints, or a tensor to one scalar channel, is a view
projection choice (sec. 5), never the block layout.

## 4. Hot values numeric; names/metadata at the edges

- Hot path: `u64` IDs, `u8` quantity codes, `u64` steps, `f64` SI values.
  Doubles cross as exact IEEE-754 bit patterns. No `std::string`, no
  channel names, no unit suffixes, no JSON.
- Names live in exactly three cold places: the `Frame` projection
  (`sig.<serial>.U`, `p0.x`), the CSV/log shape (shared goldens), and
  the control plane (`LIST`/`SRC` lines, GUI labels). Unit text parses
  exclusively through the domain quantity table (`cli::parse_quantity_value`,
  `quantity_suffix`/`quantity_name`); cross-dimension text fails loudly.
- Static metadata (mesh connectivity, table points, Lua script bytes,
  expression programs) lives in the checkpoint (`serialize.hpp` stores)
  and the batch/control configuration edge, addressed by ID. Per-tick
  records carry only the ID that selects it.

## 5. Checkpoints, views, transports

- **Checkpoints preserve authoritative restart state.** Versioned binary
  `save_state`/`load_state` with fallback loaders (v5 current: serials,
  operands, expression/table/Lua columns and stores). Dead slots retain
  columns; load re-checks sizes/ranges and live-source invariants (sums
  resolve to live non-sums of matching quantity, table ranges hold finite
  strictly-increasing points, Lua ranges land in the blob). CSV is output
  replay, never a restart substitute.
- **Views select a projection.** The GUI and the SHM/sock `Frame` path
  take `const link::Frame &` via one adapter per solver plus one
  frame-source registry entry. Adapters may subset (serial allowlists),
  decimate (`StrideSink`, `emit_every`), or reduce (primary-item
  channels, field slice/probe). Every tick is still forwarded even when a
  selection is empty, so lag accounting stays exact; drops are counted
  per cause, never silent.
- **Transports page/chunk bounded records.** Consecutive ticks batch via a
  `SampleBlockBuilder`-style sealer (schema once, numeric arrays after);
  the bounded owning queue (`output_queue.hpp`) bridges the sim thread to
  the worker; the worker (`tcp_worker.hpp`) pages blocks onto the wire in
  `kOrdered` (every sealed block, full queue reported) or `kLatest`
  (coalesce to newest, counted) mode; the stream decoder
  (`record_stream.hpp`) splits length-prefixed records with kind dispatch.
  SHM keeps the latest `Frame` window (drop-oldest ring, `frame_seq` gaps);
  sockets refuse over-bound records identically; UART drops overdriven
  lines loudly. Faster publishing alone is never a completeness claim.

## 6. When a new domain genuinely requires a new view

`Frame` (glyphs + scalar channels) is sufficient **iff** the domain is
faithfully readable as *points plus scalar time series*: particles,
signal probes, node-voltage probes. Keep the existing views untouched and
add one adapter + one registry entry.

A new view is required when any of these hold:

1. **Topology is the data.** Mesh connectivity, circuit netlists, and
   constraint/joint graphs cannot be reduced to a point cloud without
   misleading the operator. A mesh pane (wireframe + field overlay) or a
   schematic pane is a new view, not new channels.
2. **Orientation is the data.** Rigid-body attitude (quaternion/director),
   beam frames, and polarization need a pose renderer; stuffing them into
   `size`/`color` hints corrupts restarts by confusion.
3. **The field is tensor/bulk.** Per-node vectors/tensors or full-volume
   slices need probe/slice/contour affordances and node-range paging; one
   scalar channel per node explodes the 64-channel `Frame` cap and hides
   the quantity.
4. **Cardinality breaks the projection.** Beyond `kMaxItems`/`kMaxChannels`
   with no faithful subset (every node matters), paging the typed block
   is correct; cramming a lossy prefix into `Frame` and calling it the
   domain is not.

Retired sentence (do not repeat in new code/docs): *"every domain
describes itself in [glyphs + channels], so views and readers never learn
solver internals."* Replacement: *"`Frame` is one projection for
points-plus-scalars displays; each domain owns typed blocks first and
reuses `Frame` only when the projection is faithful."*

## 7. New-domain hook list (no plugin framework)

No dynamic plugins, no `dlopen`, no string factories beyond the existing
registries. A new domain owns exactly these touch points, each a plain
code entry reviewed in the domain change:

1. **Pool in `world::State`.** SoA vectors + occupancy + `live_count`,
   entity-attached (`attach`/`detach`), `despawn_entity` detaches.
2. **Snapshot arrays.** Dense slot-order copies + `fill_snapshot` reuse
   path + optional `select_*` edge filter (never in `Runner`).
3. **Solver.** `core::ISolver` subclass (`kName`, `kDescription`,
   `create`, `noexcept`, SI, validates `dt`, skips detached) + one
   `solver_registry.hpp` entry.
4. **RunConfig section.** One nested struct beside the solver (parsed
   values, no strings); driver honors it iff its solver is selected and
   rejects it when `set` otherwise. Never flat fields
   (`--gravity` is grandfathered).
5. **Checkpoint columns.** `world/serialize.hpp`: version bump + per-slot
   columns + store/blob tails + fallback loader for every prior version +
   invariant rechecks on load (bounds, ranges, cross-refs).
6. **Typed block + codec.** New `link/*block*.hpp` (or extension of the
   sample-block pattern): kind value, schema version, `k*Max` bounds,
   `encode` (`noexcept`, allocation-free, validate-before-touch) +
   `decode` (cold, may allocate) + builder if batching + `encoded_size`.
7. **Projection.** One `frame_from_*` adapter beside `link::Frame` + one
   frame-source registry entry + `default_channel` consideration. Views
   stay untouched unless sec. 5 requires a new one.
8. **Edges.** CSV/log section if replayable (header sniff + adapter);
   control-plane verbs if interactive (`control.hpp` vocabulary +
   `apply_command` at the step boundary + replay log); publisher wiring at
   the app edge only (`ISink`, never in `RunConfig`).

Omit any hook by explicit decision in the domain doc, never by drift.
