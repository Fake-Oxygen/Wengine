# Troubleshooting

Failures surface in two places: the process exit code (first
diagnostic) and the console message naming the cause. Every example
below was executed exactly as shown while writing this guide.

## Exit codes first

`0` success, `1` runtime/check failure, `2` usage error, `-1` internal
exit signal (REPL loop only).

A clean run exits 0:

```console
$ ./build/PhysicsSimApp ohm 12V 3A; echo $?
U = 12 V
I = 3 A
R = 4 Ohm
P = 36 W
0
```

A missing argument is a usage error (exit 2) — the usage text follows
the error line:

```console
$ ./build/PhysicsSimApp ohm 12V; echo $?
Error: provide exactly 2 of U/I/R (got 1)
ohm [U] [I]
...
2
```

An unknown command is a runtime error (exit 1):

```console
$ ./build/PhysicsSimApp boguscmd; echo $?
Unknown command: boguscmd
Type 'help' for available commands.
1
```

A failed check also exits 1 but says why — an LED supply below its
forward voltage prints `headroom = -4 V INFEASIBLE` with `summary: FAIL`
instead of crashing. When a run misbehaves, read the code before the
text: 2 means fix your arguments, 1 means the inputs were understood
but rejected or failed mid-run.

## Common argument errors

**Stray positionals are rejected, never ignored.** Anything the command
does not declare is a usage error:

```console
$ ./build/PhysicsSimApp ohm 12V 3A extra; echo $?
Error: invalid number 'extra'
2
```

**Cross-dimension quantities fail loudly.** Unit text is parsed against
the expected physical dimension, so current units where volts belong are
an error, not a silent conversion:

```console
$ ./build/PhysicsSimApp ohm --u 2mA --i 3A; echo $?
Error: invalid voltage '2mA' (unknown unit suffix)
...
2
```

The same firewall guards signal stamping — an amp-quantity suffix on a
voltage source is refused with the offending value named:

```console
$ ./build/PhysicsSimApp sig run --sig-kind sine --sig-amp 2mA --sig-quantity U --steps 2; echo $?
Error: invalid signal amplitude '2mA'
...
2
```

**Unknown solvers list what exists.** A typo in `--solver` is a usage
error that prints the registry, so the fix is in the message:

```console
$ ./build/PhysicsSimApp sim run --solver nonsense --steps 5; echo $?
Error: unknown solver 'nonsense'. Available: projectile signal logic
2
```

## Interactive mode needs a real terminal

Line editing, history, and Tab completion engage only on a TTY. Piped
stdin gets plain `getline` behavior (scripts keep working), and the
fullscreen TUI refuses non-terminal use loudly:

```console
$ ./build/PhysicsSimApp --tui < /dev/null; echo $?
PhysicsSimApp --tui needs an interactive terminal
1
```

`--tui` also takes no further arguments (`--tui extra` exits 2). PTY
smoke tests pin this behavior; see [Terminal UI](../tui.md). If keys
misbehave inside the TUI, suspect the terminal (`TERM=dumb`/cursor-less
terminals are refused before startup) rather than the engine.

## Where engine errors surface

Engine code reports through `core::Status`, and the CLI prints it: sink
and run failures appear as console `Error:` lines (often followed by
the command's usage), batch `FAIL` summaries name the violated limit,
and live per-source verbs answer `OK ...` / `ERR ...` per line. A
signal run stopped by a bad expression or table row freezes at that
step boundary with the cause named — time and step index do not advance
past a failed step.

## Build problems

- **In-source builds are rejected.** Configure out-of-source (`cmake -S
  . -B build`); the top-level `CMakeLists.txt` enforces this.
- **Missing submodules.** `external/glm` must be checked out (`git
  submodule update --init`); the interpreter, scripting, and YAML
  extras are vendored the same way.
- **Missing system packages.** Without `libncurses-dev` the CLI still
  builds but `--tui` prints a rebuild hint; without `libsdl2-dev` and
  `libvulkan-dev` the GUI target cannot configure. The core engine and
  all commands in [CLI commands](cli-commands.md) need neither.
- **Compiler baseline.** GCC 13.3 with C++20 is the tested toolchain;
  other compilers may trip the strict warning set (`-Wall -Wextra
  -Wpedantic -Wconversion -Wshadow -Wnon-virtual-dtor`, zero warnings
  expected).
