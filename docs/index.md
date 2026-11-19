# WEngine documentation

WEngine is a high-performance C++20 physics simulation engine:
independent solvers for physical domains, a deterministic fixed-step
runner, signal generation and routing, an inter-process link bus, and
command-line, terminal, and graphical front ends.

This site is the complete documentation suite for the engine and
everything shipped around it — user guides, developer guides, the API
reference, and the quality catalogs (tests, benchmarks, examples). It is
generated from the repository: authored pages live under `docs/` as
MyST Markdown, the API reference is extracted from Doxygen-convention
comments in `include/wengine/`, and `docs/build.sh` regenerates the HTML
(see [Site maintenance](dev/regeneration.md)).

## Audience portals

**[Using WEngine](user/index.md)** — you run simulations and read what
they produced. Start at [Getting started](user/getting-started.md):
build the engine, run a first simulation, record output, work the
interactive terminal and the graphical app.

**[Developing WEngine](dev/index.md)** — you change or extend the
engine. Start at [Architecture](dev/architecture.md): the module map,
the life of one simulation step, the rules the code must obey, and
where each extension point plugs in.

**[API reference](api/index.md)** — you embed the engine in another
application. Per-module pages cover every public header with rendered
contracts and one minimal usage pattern per module.

**[Quality — tests & benchmarks](tests/index.md)** — you review or
maintain the project. Every registered test has a catalog entry (what
it checks, why it exists, what it guards against, how to run it); every
benchmark is documented with interpretation guidance. Benchmark catalog:
[Benchmarks](benchmarks/index.md).

**[Examples](examples/index.md)** — everyone. Every shipped example,
including the link-bus tools, the helper receiver and plotting scripts,
the Lua signal sources, the logic samples, and the Zephyr embedded
sample, with run instructions and expected outcomes.

## Repository documents

The repository's own documents remain canonical for their subjects and
are linked here rather than copied into the site (so the site's search
box does not cover them). Links are relative to the built site's
location (`build/docs/html/`).

<a href="../../../README.md">README.md</a> — repository landing page:
build configurations, module table, run examples.
<a href="../../../GLOSSARY.md">GLOSSARY.md</a> — project vocabulary.
<a href="../../../AGENTS.md">AGENTS.md</a> — the working agreement for
AI sessions and contributors.

```{toctree}
:maxdepth: 2

user/index
dev/index
api/index
tests/index
benchmarks/index
examples/index
```
