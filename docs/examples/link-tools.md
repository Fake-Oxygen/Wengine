# Link tools and CLI smoke

SHM link-bus tools (`link_sample`, `link_bridge`, `link_fastread`),
the TCP/UDP socket receiver and plotter scripts with their shared
protocol/accounting helpers, and the `cli_smoke` shell-API program.
All SHM entries below were run here against a live publisher: reader
started first, then a paced `sig run` publisher in a second terminal.

## cli_smoke.cpp

**Demonstrates**: the `cli::Shell` API in its smallest form — feeds
`ohm 12V 3A` through `Shell::execute` and exits nonzero unless the
command produced output.

**How to run**: no CMake target registers it; compile against the
already-built static libraries:

```sh
g++ -std=c++20 -I include examples/cli_smoke.cpp \
  -Wl,--start-group build/libwengine_*.a -Wl,--end-group \
  -lSDL2 -o build/cli_smoke_manual
./build/cli_smoke_manual; echo "rc=$?"
```

**Expected outcome**: exit `0` with no output (success is silent;
any nonzero exit means the shell call failed or printed nothing).

## link_sample.cpp

**Demonstrates**: the minimal external-app reader for the
wengine-link SHM bus — attaches read-only to a publisher segment and
prints each new frame (`seq`, `step`, `t`, item/channel counts, exact
missed-frame lag, then every channel as `name=value`).

**How to run**: terminal 1 starts the reader first, terminal 2 runs
the paced publisher:

```sh
./build/link_sample /wengine-demo --frames 5 --timeout-ms 15000
./build/PhysicsSimApp sig run --sig-kind sine --sig-freq 1Hz \
  --steps 50 --dt 1ms --emit-shm /wengine-demo --emit-every 10 \
  --paced --rate 50Hz
```

**Expected outcome**: one line per frame, then exit `0`:

```console
seq=1 step=10 t=0.01s items=0 channels=2 missed=0 sig.count=1 sig.0.U=0.0627905
seq=2 step=20 t=0.02s items=0 channels=2 missed=0 sig.count=1 sig.0.U=0.125333
```

A short unpaced burst that finishes before attach instead reports
`timed out waiting for segment` — the segment lives only while the
publisher lives, so always start the reader first.

## link_bridge.cpp

**Demonstrates**: the CSV↔bus replay bridge — `play` republishes a
`CsvSink` log onto a fresh SHM segment (paced by the log's own time
column); `record` subscribes to a live segment and appends received
frames to a CSV log in `CsvSink` format.

**How to run**: `play` needs a particle-domain log; start the reader
first because an unpaced replay floods past a late attach:

```sh
./build/PhysicsSimApp sim run --steps 20 --particles 1 --emit-csv /tmp/opencode/proj.csv
./build/link_sample /wengine-play --frames 3 --timeout-ms 12000 &
./build/link_bridge play /tmp/opencode/proj.csv /wengine-play --rate 0
./build/link_bridge record /wengine-rec /tmp/opencode/rec.csv --frames 5 &
./build/PhysicsSimApp sig run --sig-kind sine --steps 50 --dt 1ms \
  --emit-shm /wengine-rec --emit-every 10 --paced --rate 50Hz
```

**Expected outcome**: `play` prints `replayed 20 snapshots to
'/wengine-play'`; the pre-attached reader prints the retained tail
(`missed=9` on the first line it sees is normal drop-oldest flood
loss). `record` prints `recorded 5 frames to ... (missed 0)`.
Two limits found while running this: `play` refuses a
signal-domain (`sig run --emit-csv`) log with `bad header` — it only
parses particle `CsvSink` logs; and `record` on a signal-domain
segment writes the particle CSV header with no signal rows, so signal
waveform recording stays in `sig run --emit-csv`.

## link_fastread.cpp

**Demonstrates**: the minimum-loss SHM subscriber — the same wire
contract as `link_sample` minus per-frame printing (one status line
per `--status-every` frames plus a final `received/missed/max_gap/
last_seq/loss` summary), with a reused decode buffer.

**How to run**: reader first, paced publisher second (same shape as
`link_sample`):

```sh
./build/link_fastread /wengine-fast --frames 5 --timeout-ms 15000 --status-every 0
./build/PhysicsSimApp sig run --sig-kind sine --sig-freq 1Hz \
  --steps 50 --dt 1ms --emit-shm /wengine-fast --emit-every 10 \
  --paced --rate 50Hz
```

**Expected outcome**: exit `0` with the single summary line:

```console
done received=5 missed=0 max_gap=0 last_seq=5 loss=0% rate=5/s
```

An unreachable `--frames` count ends instead in an idle-timeout
summary (`timeout received=... missed=...`, exit `1`), never silence.

## link_fastread_stall_repro.py

**Demonstrates**: the cross-process reproducer for end-of-run reader
stalls — drives `PhysicsSimApp` against `link_fastread`/`link_sample`
through four bounded scenarios (`reader_first`, `burst_exit`,
`late_attach`, `restart`) and checks published vs. cursor vs.
received sequence accounting in each.

**How to run**:

```sh
python3 examples/link_fastread_stall_repro.py --build-dir build
```

**Expected outcome**: exit `0` with per-scenario lines ending in:

```console
4/4 scenarios as expected
```

## link_receiver_accounting.py

**Demonstrates**: the receiver-side sequence accounting shared by the
socket sample receivers — a Python mirror of
`include/wengine/link/receiver.hpp`: bounded reorder window,
per-session `run_id` cursors, separated pre-attach / intentional-skip
/ confirmed-loss counters.

**How to run**: a library, not a program (no `__main__` entry point);
exercise it by importing, or run any TCP/UDP receiver below, which
all import it.

```sh
python3 -c "import sys; sys.path.insert(0, 'examples'); import link_receiver_accounting; print('import ok')"
```

**Expected outcome**: clean import (`import ok`); observable behavior
is the `unique/duplicates/reordered/stale/pre_attach` summaries the
receivers print (see `link_udp_signal_receiver.py`).

## link_socket_protocol.py

**Demonstrates**: the wire decoder for the signal TCP/UDP examples —
compact v3 record (`WLV2`, schema 3) and TCP `WLT2` hello parsing,
mirroring `socket_publisher.hpp` / `compact_codec.hpp` bounds.

**How to run**: a library, not a program (no `__main__` entry
point); the four receiver/plotter scripts import it, so running any
of them exercises it against a live publisher.

```sh
python3 -c "import sys; sys.path.insert(0, 'examples'); import link_socket_protocol; print('import ok')"
```

**Expected outcome**: clean import; against a live stream the
receivers print decoded `seq/step/t/sig.*` frames or raise a loud
`ValueError` on malformed records (never silent misparse).

## link_tcp_signal_receiver.py

**Demonstrates**: receiving and printing signal channels from a
WEngine TCP publisher — `WLT2` hello/domain gate, length-prefixed
compact records, ordered-stream accounting (any non-increasing seq
is a fatal framing violation).

**How to run**: listener first, then point the publisher at it:

```sh
python3 examples/link_tcp_signal_receiver.py --port 9000 --frames 5
./build/PhysicsSimApp sig run --sig-kind sine --sig-freq 1Hz \
  --steps 50 --dt 1ms --emit-tcp 127.0.0.1:9000 --emit-every 10 \
  --paced --rate 50Hz
```

**Expected outcome**: the publisher reports `tcp: 127.0.0.1:9000 (5
frames, 0 dropped)`; the listener prints `Attached ... domain=signal`,
five `seq=... step=... sig.count=1 sig.0.U=...` lines, and `Received 5
frames; pre_attach=0 gaps=0; trailing=unknown ...`.

## link_tcp_signal_plot.py

**Demonstrates**: the live TCP signal plotter — every pending frame
drains into a rolling history while the figure redraws at most
`--draw-hz` times per second, so kHz publishers stay plottable.

**How to run**: needs Matplotlib (`python3 -m pip install
matplotlib`); a display for the live window — under a headless
shell set `MPLBACKEND=Agg`, which draws offscreen with `UserWarning:
... cannot show the figure` notices but otherwise runs the full
receive/draw path:

```sh
MPLBACKEND=Agg python3 examples/link_tcp_signal_plot.py --port 9000 --frames 3
./build/PhysicsSimApp sig run --sig-kind sine --sig-freq 1Hz \
  --steps 30 --dt 1ms --emit-tcp 127.0.0.1:9000 --emit-every 10 \
  --paced --rate 50Hz
```

**Expected outcome**: `Received 3 frames; pre_attach=0 gaps=0;
trailing=unknown ...; drew 3 times in 0.61s (4.9 draws/s)`.

## link_udp_signal_receiver.py

**Demonstrates**: receiving and printing signal channels from WEngine
compact UDP datagrams — one accounting session per sender address,
run-id resets, duplicate/stale datagrams reported on stderr without
breaking the stream.

**How to run**: bind first, then point the publisher at the port:

```sh
python3 examples/link_udp_signal_receiver.py --port 9001 --frames 5 --idle-timeout 10
./build/PhysicsSimApp sig run --sig-kind sine --sig-freq 1Hz \
  --steps 50 --dt 1ms --emit-udp 127.0.0.1:9001 --emit-every 10 \
  --paced --rate 50Hz
```

**Expected outcome**: the publisher reports `udp: 127.0.0.1:9001 (5
frames, 0 dropped, budget=1200)`; the listener prints five `seq=...
sig.0.U=...` lines and `Received 5 datagrams; unique=5 duplicates=0
reordered=0 stale=0 pre_attach=0 skipped=0 confirmed_lost=0 pending=0
trailing=unknown (no authoritative final sequence)`.

## link_udp_signal_plot.py

**Demonstrates**: the live UDP signal plotter — same
drain-then-capped-redraw design as the TCP plotter, over datagrams
with per-sender session accounting and an idle timeout for
open-ended runs.

**How to run**: needs Matplotlib; headless shells use
`MPLBACKEND=Agg` as with the TCP plotter:

```sh
MPLBACKEND=Agg python3 examples/link_udp_signal_plot.py --port 9001 --frames 3 --idle-timeout 10
./build/PhysicsSimApp sig run --sig-kind sine --sig-freq 1Hz \
  --steps 30 --dt 1ms --emit-udp 127.0.0.1:9001 --emit-every 10 \
  --paced --rate 50Hz
```

**Expected outcome**: `Received 3 datagrams; unique=3 duplicates=0
... trailing=unknown ...; drew 3 times in 1.21s (2.5 draws/s)`.
