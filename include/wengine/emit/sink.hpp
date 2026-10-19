#pragma once

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
class ISink {
public:
  virtual ~ISink() = default;
  virtual void on_snapshot(const world::Snapshot &snapshot) = 0;
};

} // namespace wengine::emit
