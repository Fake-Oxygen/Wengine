# Examples

The example catalog: one entry for every file shipped under `examples/`
(the Zephyr embedded sample is a single entry, `zephyr/`, covering its
whole tree). Each entry states what the example demonstrates, how to
build and run it (including transport/endpoint prerequisites when it
talks to a live bus), and the expected observable outcome.

Run ground rules:

- C++ tools are built via the standard build tree (`cmake -S . -B build`
  then `cmake --build build`; the link tools also build as the
  `link_sample`, `link_bridge`, `link_fastread` targets).
- Live-bus examples need a publisher running in a second terminal:
  start the reader first, then the paced publisher (an unpaced burst
  finishes before a late reader can attach — the segment lives only
  while the publisher lives).
- Hardware/ROS examples state their prerequisites and were not run
  here; their entries say so explicitly.

Pages:

- {doc}`link-tools` — the SHM link tools, the socket receiver/plotter
  scripts and their shared protocol/accounting helpers, and the CLI
  smoke program.
- {doc}`integrations` — the ROS 2 bridge, the Lua signal sources, the
  logic-domain sample workspaces, and the Zephyr embedded sample.

Completeness of this catalog is audited by
`docs/audit_catalogs.py` (`python3 -B docs/audit_catalogs.py examples`
must report zero missing entries and zero ghost anchors).

```{toctree}
:maxdepth: 1

link-tools
integrations
```
