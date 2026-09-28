#!/usr/bin/env python3
"""Receive and print signal channels from a WEngine TCP publisher.

Start this listener first, then point `sig run --emit-tcp host:port` at it.
"""

from __future__ import annotations

import argparse
import socket
import struct
import sys

from link_socket_protocol import (
    MAX_COMPACT_RECORD,
    TCP_FRAME_PREFIX_SIZE,
    TCP_HELLO_SIZE,
    decode_compact_record,
    decode_tcp_hello,
    format_signal_frame,
)


def read_exact(connection: socket.socket, count: int, *, allow_clean_eof: bool = False) -> bytes | None:
    chunks = []
    received = 0
    while received < count:
        chunk = connection.recv(count - received)
        if not chunk:
            if allow_clean_eof and received == 0:
                return None
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
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error("--port must be in 1..65535")
    if args.frames < 0:
        parser.error("--frames cannot be negative")
    return args


def main() -> int:
    args = parse_args()
    if sys.byteorder != "little" or struct.calcsize("d") != 8:
        print("TCP LinkFrame v1 sample requires a little-endian host with 8-byte doubles", file=sys.stderr)
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
            assert hello is not None
            domain = decode_tcp_hello(hello)
            if domain != "signal":
                raise ValueError(f"expected signal domain, received {domain!r}")
            print(f"Attached {address[0]}:{address[1]} domain={domain}", flush=True)

            count = 0
            last_seq = 0
            last_run = 0
            missed_total = 0
            while args.frames == 0 or count < args.frames:
                prefix = read_exact(connection, TCP_FRAME_PREFIX_SIZE, allow_clean_eof=True)
                if prefix is None:
                    break
                frame_size = struct.unpack("<I", prefix)[0]
                if frame_size > MAX_COMPACT_RECORD:
                    raise ValueError(
                        f"compact record size {frame_size} exceeds {MAX_COMPACT_RECORD}"
                    )
                payload = read_exact(connection, frame_size)
                assert payload is not None
                run_id, seq, step, sim_time, channels = decode_compact_record(payload)
                if last_run and run_id != last_run:
                    print(f"new publisher run run_id={run_id} (cursor reset)", flush=True)
                    last_seq = 0
                last_run = run_id
                missed = max(0, seq - last_seq - 1) if last_seq else 0
                if last_seq and seq <= last_seq:
                    raise ValueError(f"non-increasing TCP frame sequence {seq} after {last_seq}")
                missed_total += missed
                last_seq = seq
                count += 1
                print(format_signal_frame(seq, step, sim_time, channels, missed), flush=True)

            print(f"Received {count} frames; sequence gaps observed={missed_total}")
    finally:
        listener.close()
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, EOFError, ValueError) as error:
        print(f"TCP signal receiver: {error}", file=sys.stderr)
        raise SystemExit(1)
