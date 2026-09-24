#!/usr/bin/env python3
"""Receive and print signal channels from WEngine compact UDP datagrams."""

from __future__ import annotations

import argparse
import socket
import sys

from link_socket_protocol import decode_udp_datagram, format_signal_frame


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
        help="exit after this many idle seconds (0 waits forever; default: 3)",
    )
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error("--port must be in 1..65535")
    if args.frames < 0:
        parser.error("--frames cannot be negative")
    if args.idle_timeout < 0:
        parser.error("--idle-timeout cannot be negative")
    return args


def main() -> int:
    args = parse_args()
    receiver = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    receiver.bind((args.bind, args.port))
    receiver.settimeout(args.idle_timeout or None)
    print(f"Listening for WLU1 signal datagrams on {args.bind}:{args.port}", flush=True)

    count = 0
    last_seq = 0
    missed_total = 0
    try:
        while args.frames == 0 or count < args.frames:
            try:
                packet, address = receiver.recvfrom(65535)
            except socket.timeout:
                break
            seq, step, sim_time, channels = decode_udp_datagram(packet)
            if last_seq and seq <= last_seq:
                print(f"duplicate/out-of-order seq={seq} from {address[0]}", file=sys.stderr)
                continue
            missed = max(0, seq - last_seq - 1) if last_seq else 0
            missed_total += missed
            last_seq = seq
            count += 1
            print(format_signal_frame(seq, step, sim_time, channels, missed), flush=True)
    finally:
        receiver.close()

    print(f"Received {count} datagrams; sequence gaps observed={missed_total}")
    if count == 0:
        print("No signal datagrams received before idle timeout", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as error:
        print(f"UDP signal receiver: {error}", file=sys.stderr)
        raise SystemExit(1)
