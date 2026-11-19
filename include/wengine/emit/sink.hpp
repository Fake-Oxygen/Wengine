#pragma once

/**
 * @file
 * Sink observer interface: the engine's only emission edge. Owns ISink,
 * the best-effort non-throwing observer contract. Depends only on world
 * state snapshots; never the reverse (sinks attach at the app edge, never
 * inside the engine). Consumed by the Runner and every CSV, console, and
 * link endpoint.
 */
#include "wengine/world/state.hpp"

namespace wengine::emit {

// Sink for simulated environment: other apps (GUI, IPC, file, network)
// implement this. Engine only produces snapshots, never knows consumers.
//
// LIFETIME (immutable-observer contract): the Snapshot reference handed to
// on_snapshot is valid only for the duration of that call — the Runner
// reuses one buffer across emissions, so the next emission overwrites it.
// A sink that needs the data after returning takes an owned copy
// (take_snapshot, push_back, adapt-into-owned-Frame, ...) or enqueues an
// owned block into a pooled queue (see link/output_queue.hpp) before
// returning. Never retain the reference (or a pointer into it), never
// enqueue a reference to it for async consumption, and never reenter the
// driving Runner from inside on_snapshot. Sinks are best-effort
// observers: they must not throw.
/**
 * @brief Sink for simulated environments: other apps (GUI, IPC, file, network) implement this.
 * @note Immutable-observer contract: the Snapshot reference handed to
 * on_snapshot is valid only for that call (the Runner reuses one buffer,
 * so the next emission overwrites it). A sink needing data past return
 * takes an owned copy or enqueues an owned block before returning; never
 * retain the reference, never reenter the driving Runner. Best-effort:
 * implementations must not throw, and a failure must not alter simulation
 * state or timing.
 */
class ISink {
public:
/** @brief Virtual destructor. */
  virtual ~ISink() = default;
/**
 * @brief Observe one emitted snapshot.
 * @param[in] snapshot independent snapshot copy, valid during the call only; never live mutable state.
 * @note Best-effort, non-throwing: a sink failure must not alter simulation state or timing.
 */
  virtual void on_snapshot(const world::Snapshot &snapshot) = 0;
};

} // namespace wengine::emit
