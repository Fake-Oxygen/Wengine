#pragma once

#include "wengine/world/state.hpp"

namespace wengine::emit {

// Sink for simulated environment: other apps (GUI, IPC, file, network)
// implement this. Engine only produces snapshots, never knows consumers.
class ISink {
public:
  virtual ~ISink() = default;
  virtual void on_snapshot(const world::Snapshot &snapshot) = 0;
};

} // namespace wengine::emit
