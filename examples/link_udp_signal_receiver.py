#!/usr/bin/env python3
"""Receive and print signal channels from WEngine compact UDP datagrams."""

from __future__ import annotations

import argparse
import socket
import sys

from link_receiver_accounting import (
    ADVANCED_WITH_GAP,
    DUPLICATE,
    IGNORED,
    STALE,
    session_for,
    summarize,
)
from link_socket_protocol import decode_compact_record, format_signal_frame


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
    print(f"Listening for WLV2 signal datagrams on {args.bind}:{args.port}", flush=True)

    count = 0
    # One session per UDP sender: unrelated senders never share a counter,
    # and a run_id change resets that sender's cursor (see
    # link_receiver_accounting, mirroring link/receiver.hpp).
    sessions: dict = {}
    try:
        while args.frames == 0 or count < args.frames:
            try:
                packet, address = receiver.recvfrom(65535)
            except socket.timeout:
                break
            run_id, seq, step, sim_time, channels = decode_compact_record(packet)
            session = session_for(sessions, address[0])
            prev_highest = session.track.highest if session.track.attached else 0
            session_reset, outcome = session.observe(run_id, seq)
            if session_reset:
                print(f"new publisher run run_id={run_id} from {address[0]} (cursor reset)", flush=True)
            if outcome in (DUPLICATE, STALE, IGNORED):
                print(f"duplicate/out-of-order seq={seq} from {address[0]} ({outcome})", file=sys.stderr)
                continue
            count += 1
            # Per-frame gap that opened with this datagram (pending until it
            # fills or ages out; see the final summary for confirmed loss).
            gap = seq - prev_highest - 1 if outcome == ADVANCED_WITH_GAP else 0
            print(format_signal_frame(seq, step, sim_time, channels, gap), flush=True)
    finally:
        receiver.close()

    print(f"Received {count} datagrams; {summarize(sessions)}")
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
