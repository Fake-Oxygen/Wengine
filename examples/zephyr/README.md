# Zephyr HIL samples (SIG-8)

Two minimal Zephyr apps speaking the WEngine wire protocols. Both are
plain C99 (no C++ STL, no heap, all state static); the parsers live in
`common/` and are compiled unchanged into the host unit test, which
cross-checks them against the C++ builders on golden corpora.

| App | Reads | Needs on the host |
|---|---|---|
| `uart/` | UART stream + control (`--emit-uart`, `--control-uart`) | serial port at 115200 8N1 |
| `socket/` | TCP compact records (`--emit-tcp`) | reachable IP:port |

## UART sample

```
west init -m https://github.com/zephyrproject-rtos/zephyr --mr v3.7.0 workspace
west update && west zephyr-export
west build -b nrf52dk examples/zephyr/uart
west flash
```

Wire the board's UART0 to the host (`/dev/ttyUSB0`, 115200 8N1) and run:

```
./build/PhysicsSimApp sig run --sig-kind sine --sig-freq 1kHz \
  --sig-amp 3.3V --steps 1000 --dt 0.1ms \
  --emit-uart /dev/ttyUSB0 --control-uart
```

The sample prints one line per sample (`sig id=<serial> step=<n>
t=<s> v=<value> <U|I>`, plus `GAP` when steps skip) and answers the
startup `PING` round-trip (`ctl: OK pong`). Display floats round to
single precision; the wire doubles stay bit-exact (verified on the
host). COBS twin: append `--emit-uart-mode cobs` on the host and
rebuild with `-DCONFIG_WUART_COBS=y`.

Try the interaction verbs from the device side (uncomment in
`uart/src/main.c`): `SET 0 FREQ 50` retunes the live run,
`STOP 0`/`START 0` freeze/resume it — same verbs as the TCP control
plane (docs/signals.md sec. 6).

## STM32F469I-DISCO (single UART via ST-Link)

Board target `stm32f469i_disco`; the console — and the whole HIL wire —
is UART3 (PB10/PB11) through the ST-Link Virtual COM Port at 115200
8N1, visible on the host as `/dev/ttyACM0`. An app overlay pinning
exactly that (`uart/boards/stm32f469i_disco.overlay`) is included, and
full duplex is all a UART HIL link needs: stream in, console/control
out, one wire. See `docs/zephyr-hil.md` ("Single-UART operation") for
the sharing rules.

Bring-up order (isolates board/flash/serial from the sample):

```
west build -b stm32f469i_disco samples/hello_world
west flash          # openocd via ST-Link; with a J-Link probe: west flash -r jlink
minicom -D /dev/ttyACM0   # expect "Hello World! arm", then quit minicom
west build -b stm32f469i_disco examples/zephyr/uart
west flash
```

Stream-only first (diagnostics on the console, no control side yet):

```
./build/PhysicsSimApp sig run --sig-kind sine --sig-freq 1kHz \
  --sig-amp 3.3V --steps 1000 --dt 0.1ms \
  --emit-uart /dev/ttyACM0 --emit-every 4
```

Expect the banner plus `sig id=...` lines (4x decimation: 1 kHz over
115200 carries ~250 lines/s — see the baud budget in
`docs/zephyr-hil.md`). Then the control round-trip, with the quiet wire
so the device TX carries control frames only:

```
west build -b stm32f469i_disco examples/zephyr/uart -- -DCONFIG_WUART_QUIET=y
west flash
./build/PhysicsSimApp sig run --sig-kind sine --sig-freq 1kHz \
  --sig-amp 3.3V --steps 1000 --dt 0.1ms \
  --emit-uart /dev/ttyACM0 --emit-every 4 --control-uart
```

The host summary proves the round-trip (`control-uart: ...
commands=1 responses=1 ...`, including the sample's startup `PING`).
COBS twin: host `--emit-uart-mode cobs` with `-DCONFIG_WUART_COBS=y`
(and quiet on top the same way).

Single-UART diagnosis without touching the wire: the sample blinks
`led0` at 1 Hz while its loop is alive and toggles once more per
stream line, so bytes arriving show as flicker. Dark LED = app never
reached the loop (or no `led0` alias — the banner then says nothing
about it); steady 1 Hz with no flicker = alive but nothing arriving
(host/wiring side); flicker with no console lines = quiet build doing
its job (check the host summary).

## Socket sample

Needs a board or emulator with IP networking. The documented target is
QEMU x86 with the host reachable at its address:

```
west build -b qemu_x86 examples/zephyr/socket \
  -DCONFIG_WENGINE_HOST='"192.0.2.10"' -DCONFIG_WENGINE_PORT=9000
```

On the host, bind the publisher to an interface the guest reaches:

```
./build/PhysicsSimApp sig run --sig-kind sine --sig-freq 1kHz \
  --sig-amp 3.3V --steps 1000 --dt 0.1ms --emit-tcp 0.0.0.0:9000
```

The sample validates the `WLT2` hello/version/domain gate, decodes
every compact v2 record, prints `step/seq/channels`, and reports
`frame_seq` gaps (same LagStats vocabulary as the Python receivers).
Sample limits: records over 8 KiB are refused loudly (a 63-source
signal record is ~1 KiB); sample-block records report their kind and
are skipped by length — full batch decode is a follow-up.

## Without hardware

Both parsers run on the host: `test_uart_codec` (COBS + line goldens)
feeds the exact `common/` sources, and the loopback tests drive
`--emit-uart` through a pty pair. A USB-serial loopback plug (TX-RX
shorted) exercises the same path at real baud rates.
