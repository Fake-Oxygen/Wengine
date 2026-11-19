# Developing WEngine

You change or extend the engine. This portal explains how the engine
works as a whole — the module map, the life of one simulation step from
configuration through state mutation to emitted output, the rules every
change must obey (dependency direction, determinism, units, error
vocabulary), and the playbook for each extension point.

## Guides

- **Architecture** — module responsibilities, step lifecycle,
  dependency rules.
- **Determinism** — the ordering contracts that make runs
  reproducible.
- **Extension points** — where a new solver, command, sink, signal
  quantity, or GUI view plugs in.

Topic guides already shipped with the repository (they live at the
`docs/` root and join this portal's toctree from there):

```{toctree}
:maxdepth: 1

architecture
determinism
extension-points
regeneration
/signals
/delivery
/ros2
/zephyr-hil
```
