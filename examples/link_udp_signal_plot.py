#!/usr/bin/env python3
"""Receive signal channels from a WEngine UDP publisher and plot them live.

Install Matplotlib first (for example, ``python3 -m pip install matplotlib``),
start this listener, then point ``sig run --emit-udp host:port`` at it.

Receive and draw are decoupled: every pending datagram is drained into the
rolling history, but the figure is redrawn at most --draw-hz times per
second. Redrawing once per received datagram caps the stream at the
plotter's draw speed (tens of frames/s: full re-render, limit recompute and
legend layout per draw); draining keeps up with fast publishers while drawing
stays at a sustainable display rate. Dropped datagrams still show up as
sequence gaps in the final summary.
"""

from __future__ import annotations

import argparse
from collections import deque
import select
import socket
import sys
import time

from link_socket_protocol import decode_udp_datagram


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bind", default="0.0.0.0", help="local address to bind")
    parser.add_argument("--port", type=int, default=9001, help="UDP listen port (default: 9001)")
    parser.add_argument(
        "--frames", type=int, default=0, help="exit after N datagrams (0 means until idle timeout)"
    )
    parser.add_argument(
        "--idle-timeout",
        type=float,
        default=3.0,
        help="stop after this many idle seconds (0 waits forever; default: 3)",
    )
    parser.add_argument(
        "--history", type=int, default=1000, help="number of recent samples to display (default: 1000)"
    )
    parser.add_argument(
        "--draw-hz",
        type=float,
        default=30.0,
        help="max figure redraws per second; received datagrams between redraws "
        "accumulate in history without drawing (default: 30)",
    )
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error("--port must be in 1..65535")
    if args.frames < 0:
        parser.error("--frames cannot be negative")
    if args.idle_timeout < 0:
        parser.error("--idle-timeout cannot be negative")
    if args.history < 1:
        parser.error("--history must be positive")
    if not args.draw_hz > 0:
        parser.error("--draw-hz must be positive")
    return args


def main() -> int:
    args = parse_args()
    try:
        import matplotlib.pyplot as plt
    except ImportError:
        print("UDP signal plotter requires Matplotlib; install it with 'python3 -m pip install matplotlib'", file=sys.stderr)
        return 2

    receiver = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    receiver.bind((args.bind, args.port))
    receiver.setblocking(False)
    print(f"Listening for WLU1 signal datagrams on {args.bind}:{args.port}", flush=True)

    try:
        plt.ion()
        figure, axes = plt.subplots()
        axes.set_title("WEngine signal stream")
        axes.set_xlabel("Simulation time (s)")
        axes.set_ylabel("Value (SI units)")
        axes.grid(True, alpha=0.3)
        figure.show()

        times = deque(maxlen=args.history)
        values: dict[str, deque[float]] = {}
        lines = {}
        legend_names: set[str] | None = None
        count = 0
        draws = 0
        last_seq = 0
        missed_total = 0
        draw_period = 1.0 / args.draw_hz
        last_draw = 0.0
        last_rx = time.monotonic()
        dirty = False
        t_start = last_rx
        while (args.frames == 0 or count < args.frames) and plt.fignum_exists(figure.number):
            # Drain every datagram already queued, without drawing.
            while True:
                readable, _, _ = select.select([receiver], [], [], 0.0)
                if not readable:
                    break
                try:
                    packet, address = receiver.recvfrom(65535)
                except BlockingIOError:
                    break
                seq, _step, sim_time, channels = decode_udp_datagram(packet)
                if last_seq and seq <= last_seq:
                    print(f"duplicate/out-of-order seq={seq} from {address[0]}", file=sys.stderr)
                    continue
                missed = max(0, seq - last_seq - 1) if last_seq else 0
                missed_total += missed
                last_seq = seq
                count += 1
                last_rx = time.monotonic()

                signal_values = {name: value for name, value in channels if name.startswith("sig.")}
                times.append(sim_time)
                for name in values:
                    values[name].append(signal_values.get(name, float("nan")))
                for name, value in signal_values.items():
                    if name not in values:
                        values[name] = deque([float("nan")] * (len(times) - 1), maxlen=args.history)
                        values[name].append(value)
                        (lines[name],) = axes.plot([], [], label=name)
                    else:
                        values[name][-1] = value
                dirty = True
                if args.frames and count >= args.frames:
                    break

            # Idle timeout only applies to open-ended runs (mirrors --frames=0).
            if args.frames == 0 and args.idle_timeout and time.monotonic() - last_rx >= args.idle_timeout:
                break
            # One capped redraw per batch of drained datagrams.
            now = time.monotonic()
            finished = bool(args.frames and count >= args.frames)
            if dirty and (finished or now - last_draw >= draw_period):
                for name, line in lines.items():
                    line.set_data(times, values[name])
                axes.relim()
                axes.autoscale_view()
                if set(lines) != legend_names:
                    legend_names = set(lines)
                    if lines:
                        axes.legend(loc="upper right")
                figure.canvas.draw_idle()
                figure.canvas.flush_events()
                plt.pause(0.001)
                last_draw = now
                draws += 1
                dirty = False
            if finished:
                break
            # Nothing to do: short sleep instead of a busy spin.
            if not dirty:
                time.sleep(0.005)

        wall = time.monotonic() - t_start
        plt.close(figure)
    finally:
        receiver.close()

    if wall > 0:
        print(
            f"Received {count} datagrams; sequence gaps observed={missed_total}; "
            f"drew {draws} times in {wall:.2f}s ({draws / wall:.1f} draws/s)"
        )
    else:
        print(f"Received {count} datagrams; sequence gaps observed={missed_total}; drew {draws} times")
    if count == 0:
        print("No signal datagrams received before idle timeout", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as error:
        print(f"UDP signal plotter: {error}", file=sys.stderr)
        raise SystemExit(1)
