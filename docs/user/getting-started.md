# Getting started

This page takes you from a clean checkout to your first simulation run.
It assumes a plain terminal and no prior knowledge of the codebase. For
the full build matrix, module table, and test list, see
<a href="../../../../README.md">README.md</a>.

## Prerequisites

- A C++20-capable toolchain. GCC 13.3 is the baseline the project is
  developed and tested with.
- CMake 3.20 or newer.
- The `external/glm` submodule checked out (clone with
  `git clone --recursive`, or run `git submodule update --init` after
  cloning). Without it, configuration fails.
- Optional extras: `libncurses-dev` for the terminal UI, `libsdl2-dev`
  plus `libvulkan-dev` for the graphical app. The core engine and the
  commands below build without them.

Check the toolchain before configuring (this host reports the baseline
compiler and a new-enough CMake):

```console
$ g++ --version | head -1
g++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0
$ cmake --version | head -1
cmake version 3.28.3
```

Any C++20-capable compiler in this range works; GCC 13.3 is simply the
one the project is developed and tested with, so it is the one whose
warnings and goldens are pinned.

## Build

Out-of-source builds only (in-source configuration is rejected). From
the repository root:

```sh
cmake -S . -B build -DBUILD_TESTS=ON
cmake --build build
```

Both commands were re-run while writing this guide and succeed with
exit code 0 (`build/` already exists, so the rebuild is instant when
nothing changed).

For reproducible configurations, prefer the presets documented in
<a href="../../../../README.md">README.md</a> (`dev`, `test-opt`,
`bench`) over hand-rolled `-D` flags:

```sh
cmake --preset dev && cmake --build --preset dev && ctest --preset dev
```

## First headless run

The simplest end-to-end check runs 100 fixed steps of the default
projectile solver with one particle:

```console
$ ./build/PhysicsSimApp sim run --steps 100 --particles 1
sim run: solver=projectile g=9.81
steps: 100 x 0.01s -> t=1s
particles: 1
p0: x=10 y=5.04595 vx=10 vy=0.19
```

What the output means: the first line names the solver and gravity, the
second line shows the step budget (`100` steps of `0.01 s`, one simulated
second), and `p0:` gives the final position and velocity of particle 0.
Exit code is 0 on success.

## REPL smoke test

Without any arguments the program starts an interactive Read-Eval
shell (REPL): it prints a `> ` prompt, runs each line you type, and
waits for the next. Piped input keeps working through plain `getline`,
so this one-liner solves a resistor and quits:

```console
$ printf 'ohm 12V 3A\nexit\n' | ./build/PhysicsSimApp
> U = 12 V
I = 3 A
R = 4 Ohm
P = 36 W
>
```

The `> ` markers are the shell prompt: one is printed before each line
is read, including the final `exit`, so the last prompt lands on the
same line as any following shell output. The pipeline exits 0 — a clean
quit, not a crash.

## Where to go next

- [CLI commands](cli-commands.md) — every command family, with usage and
  worked examples.
- [Outputs](outputs.md) — console text, CSV logs, and signals logs.
- [Terminal UI](../tui.md) — the full CLI over ncurses (`--tui`).
- [Graphical app](gui.md) — panels, views, and external sources.
- [Troubleshooting](troubleshooting.md) — exit codes and common mistakes.
