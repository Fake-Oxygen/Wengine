#!/usr/bin/env python3
"""Receive signal channels from a WEngine TCP publisher and plot them live.

Install Matplotlib first (for example, ``python3 -m pip install matplotlib``),
start this listener, then point ``sig run --emit-tcp host:port`` at it.

Receive and draw are decoupled: every pending frame is drained into the
rolling history, but the figure is redrawn at most --draw-hz times per
second. Redrawing once per received frame caps the stream at the plotter's
draw speed (tens of frames/s: full re-render, limit recompute and legend
layout per draw); draining keeps up with kHz publishers while drawing stays
at a sustainable display rate. Publisher-side drops still show up as sequence
gaps in the final summary.
"""

from __future__ import annotations

import argparse
from collections import deque
import select
import socket
import struct
import sys
import time

from link_receiver_accounting import SessionReceiver
from link_socket_protocol import (
    MAX_COMPACT_RECORD,
    TCP_FRAME_PREFIX_SIZE,
    TCP_HELLO_SIZE,
    decode_compact_record,
    decode_tcp_hello,
)


def read_exact(listener: socket.socket, count: int) -> bytes:
    """Blocking read of exactly count bytes (used only for the hello)."""
    chunks = []
    received = 0
    while received < count:
        chunk = listener.recv(count - received)
        if not chunk:
            raise EOFError(f"peer closed with {received}/{count} bytes received")
        chunks.append(chunk)
        received += len(chunk)
    return b"".join(chunks)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bind", default="0.0.0.0", help="local address to listen on")
    parser.add_argument("--port", type=int, default=9000, help="TCP listen port (default: 9000)")
    parser.add_argument(
        "--frames", type=int, default=0, help="exit after N frames (0 means until disconnect)"
    )
    parser.add_argument(
        "--history", type=int, default=1000, help="number of recent samples to display (default: 1000)"
    )
    parser.add_argument(
        "--draw-hz",
        type=float,
        default=30.0,
        help="max figure redraws per second; received frames between redraws "
        "accumulate in history without drawing (default: 30)",
    )

    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error("--port must be in 1..65535")
    if args.frames < 0:
        parser.error("--frames cannot be negative")
    if args.history < 1:
        parser.error("--history must be positive")
    if not args.draw_hz > 0:
        parser.error("--draw-hz must be positive")
    return args


def main() -> int:
    args = parse_args()
    if sys.byteorder != "little" or struct.calcsize("d") != 8:
        print("TCP LinkFrame v1 sample requires a little-endian host with 8-byte doubles", file=sys.stderr)
        return 2

    try:
        import matplotlib.pyplot as plt
    except ImportError:
        print("TCP signal plotter requires Matplotlib; install it with 'python3 -m pip install matplotlib'", file=sys.stderr)
        return 2

    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind((args.bind, args.port))
    listener.listen(1)
    print(f"Listening for WLT2 signal stream on {args.bind}:{args.port}", flush=True)

    try:
        connection, address = listener.accept()
        with connection:
            hello = read_exact(connection, TCP_HELLO_SIZE)
            domain = decode_tcp_hello(hello)
            if domain != "signal":
                raise ValueError(f"expected signal domain, received {domain!r}")

            plt.ion()
            figure, axes = plt.subplots()
            axes.set_title("WEngine signal stream")
            axes.set_xlabel("Simulation time (s)")
            axes.set_ylabel("Value (SI units)")
            axes.grid(True, alpha=0.3)
            figure.show()

            connection.setblocking(False)
            buf = bytearray()
            eof = False
            times = deque(maxlen=args.history)
            values: dict[str, deque[float]] = {}
            lines = {}
            legend_names: set[str] | None = None
            count = 0
            draws = 0
            # TCP is ordered: non-increasing seq stays fatal. The session
            # separates pre-attach history per publisher run (see
            # link_receiver_accounting, mirroring link/receiver.hpp).
            session = SessionReceiver()
            pre_attach_total = 0
            gaps_total = 0
            draw_period = 1.0 / args.draw_hz
            last_draw = 0.0
            dirty = False
            t_start = time.monotonic()
            while (args.frames == 0 or count < args.frames) and plt.fignum_exists(figure.number):
                # Wait briefly for data (no busy spin, Ctrl-C stays responsive).
                if not eof:
                    readable, _, _ = select.select([connection], [], [], 0.05)
                    if readable:
                        try:
                            chunk = connection.recv(256 * 1024)
                        except BlockingIOError:
                            chunk = None
                        if chunk is not None:
                            if not chunk:
                                eof = True  # peer closed; drain the buffer, draw, exit
                            else:
                                buf += chunk
                # Drain every complete frame already in the buffer.
                while len(buf) >= TCP_FRAME_PREFIX_SIZE:
                    (frame_size,) = struct.unpack_from("<I", buf, 0)
                    if frame_size > MAX_COMPACT_RECORD:
                        raise ValueError(
                            f"compact record size {frame_size} exceeds {MAX_COMPACT_RECORD}"
                        )
                    if len(buf) < TCP_FRAME_PREFIX_SIZE + frame_size:
                        break  # partial frame: wait for the rest
                    payload = bytes(buf[TCP_FRAME_PREFIX_SIZE : TCP_FRAME_PREFIX_SIZE + frame_size])
                    del buf[: TCP_FRAME_PREFIX_SIZE + frame_size]
                    run_id, seq, _step, sim_time, channels = decode_compact_record(payload)
                    was_attached = session.track.attached
                    prev_highest = session.track.highest if was_attached else 0
                    session_reset, _outcome = session.observe(run_id, seq)
                    if session_reset and was_attached:
                        print(f"new publisher run run_id={run_id} (cursor reset)", flush=True)
                    if session_reset:
                        pre_attach_total += session.track.pre_attach
                    missed = seq - prev_highest - 1 if was_attached and not session_reset else 0
                    if was_attached and not session_reset and seq <= prev_highest:
                        raise ValueError(f"non-increasing TCP frame sequence {seq} after {prev_highest}")
                    gaps_total += missed
                    count += 1

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

                # One capped redraw per batch of drained frames.
                now = time.monotonic()
                finished = eof and not buf
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

            wall = time.monotonic() - t_start
            plt.close(figure)
            print(
                f"Received {count} frames; pre_attach={pre_attach_total} gaps={gaps_total}; "
                f"trailing=unknown (no authoritative final sequence); "
                f"drew {draws} times in {wall:.2f}s "
                f"({draws / wall:.1f} draws/s)" if wall > 0 else
                f"Received {count} frames; pre_attach={pre_attach_total} gaps={gaps_total}; "
                f"trailing=unknown (no authoritative final sequence); drew {draws} times"
            )
    finally:
        listener.close()
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, EOFError, ValueError) as error:
        print(f"TCP signal plotter: {error}", file=sys.stderr)
        raise SystemExit(1)
