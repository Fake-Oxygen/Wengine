# Zephyr HIL wiring guide (SIG-8)

Host-to-device serial path for physical simulation: `sig run`/`sim run`
`--emit-uart` streams signal rows over a UART the Zephyr sample in
`examples/zephyr/` consumes, and `--control-uart` (`sig run` only)
interleaves the SIG-10 control verbs on the same wire. Full protocol in
`docs/signals.md` sec. 5 (stream) and sec. 6 (control); this file is the
practical companion: wiring, budgets, resync, and bring-up.

## Wiring

USB-UART bridge (`/dev/ttyUSB0`) or on-board debugger CDC port, 8N1, no
flow control, `CLOCAL` (the host sets raw 8N1 via termios; configure the
device UART identically — the sample overlay uses 115200). Cross TX/RX,
share GND. Mind logic levels: most Zephyr boards are 3V3-tolerant only;
a 5V bridge needs a level shifter, never a direct tie.

Without hardware: the host test drives `--emit-uart` through a pty pair
(see `tests/test_uart_pub.cpp`), and a TX-RX-shorted loopback plug
exercises a real bridge at real baud rates.

## Line format

One CSV-shaped row per source per emission (identical to the signals log,
so goldens are shared):

```text
<step>,<time>,<sig_id>,<value>,<quantity>\n      ascii stream
!<control line>\n                                ascii control, both ways
COBS('S' + line) 0x00                           cobs stream
COBS('Q' + line) 0x00                           device->host command
COBS('R' + line) 0x00                           host->device response
```

Doubles print with `%.17g`: the wire round-trips bit-exactly (`==` on
re-read, asserted in tests). Control lines are the SIG-10 verbs verbatim;
stream lines never start with `!` and never contain `0x00`, so the demux
is unambiguous in both modes.

## Baud budget

8N1 carries 10 symbols per byte. A measured 1 kHz sine line is ~46 B
(`%.17g` values dominate; step digits add ~1 B per decade), so:

| Baud | Payload B/s | Lines/s (~46 B) | 1 source ceiling | 1 kHz needs |
|---:|---:|---:|---:|---|
| 9600 | 960 | ~20 | ~20 Hz | 50x decimation |
| 57600 | 5760 | ~125 | ~125 Hz | 8x decimation |
| 115200 | 11520 | ~250 | ~250 Hz | 4x decimation |
| 921600 | 92160 | ~2000 | ~2 kHz | none (2x margin) |

COBS mode adds ~1 type byte + ~1–2 framing bytes per line (~+6%):
budget 43 B of payload per 46 B of line. Control traffic is a few lines
per interaction — negligible next to the stream.

Rule of thumb: `decimation >= ceil(sample_rate * n_sources * 46 /
baud_bytes)` with margin, enforced with `--emit-every` (global) and/or
`--emit-uart-stride` (per-output); `--emit-uart-select` shrinks rows,
never the rate. An under-budgeted config is visible, never silent: the
`uart:` summary reports `lines dropped`, and `--emit-health` adds the
per-cause report. UART never back-pressures the sim (overdriven lines
drop UDP-style; the next line resyncs).

Rate extremes: sub-Hz is trivial (a line every N seconds, same parser);
100 kHz-class `dt` must decimate (e.g. `dt=10us --emit-every 1000`
yields 100 lines/s — the soak test runs 200 k steps exactly so).

## Resync after garbage

A noise burst ends at the next delimiter in both modes (`\n` / `0x00`);
the offending unit counts malformed (host `UartControl::malformed`,
device `malformed` counter, both printed) and parsing resumes —
proven by injecting garbage mid-stream in `tests/test_uart_pub.cpp`
and around COBS frames in `tests/test_uart_codec.cpp`. If malformed
counts climb steadily (not in bursts), the two sides disagree on
ascii/cobs or on baud — recheck `--emit-uart-mode` against
`CONFIG_WUART_COBS` first.

## Single-UART operation

Boards with one serial wire (e.g. STM32F469I-DISCO: UART3 via the
ST-Link VCP) run stream, diagnostics, and control full-duplex on that
single UART — no second port needed. Three sharing rules:

1. **Stream is RX-only on the device; TX is console + control.**
   Printks never disturb the stream parser (opposite direction), so
   stream-only runs (`--emit-uart` without `--control-uart`) work with
   full diagnostics on.
2. **With `--control-uart`, device TX must be control frames only.**
   Bare printks (`sig id=...`, `wengine: ...`) arrive at the host
   control pump as non-`!` lines and count `malformed` — tolerated and
   loud, but noisy at hundreds of lines per second. Build the sample
   with `CONFIG_WUART_QUIET=y`: TX then carries control frames only
   (plus the boot banner), the host `malformed` stays at zero, and the
   host `control-uart:` summary (`commands=`/`responses=`) is the
   round-trip proof instead of console echoes.
3. **One opener per direction at a time.** Two host processes can open
   the same `/dev/ttyACM0`, but their reads compete: view the console
   (`minicom`, `tio`) to check the banner, then quit the viewer before
   starting the host run. For unattended capture, `cat /dev/ttyACM0 >
   console.log &` before the run and diff afterwards.

The startup `PING` the sample sends on boot is the cheapest end-to-end
check: with `--control-uart` attached the host answers `OK pong`
(`responses=` ticks up); without it, the command is simply never
answered — no error on either side.

## Bring-up checklist

1. Loopback first: short the bridge TX-RX (or use a pty pair) and run
   `sig run ... --emit-uart /dev/ttyUSB0 --steps 10`; every emitted line
   must echo back byte-identical.
2. One source at a decimated rate (`--emit-every 10` at 1 kHz/115200);
   confirm `0 lines dropped` in the summary.
3. Attach the board; the sample prints `sig id=... step=...` lines and
   the startup `ctl: OK pong` from `--control-uart`.
4. Raise the rate toward the budget; watch `lines dropped` stay zero.
5. `SET 0 FREQ 50` from the device (or any TCP control client) retunes
   the live run — stream, control, and CSV stay sample-identical.
