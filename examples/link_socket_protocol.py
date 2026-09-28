"""Small Python decoder for the WEngine signal TCP/UDP socket examples.

Wire constants follow include/wengine/link/socket_publisher.hpp and
include/wengine/link/compact_codec.hpp. Both transports carry compact v2
records (magic "WLV2", schema 2, explicit little-endian fields, used
entries only); TCP adds a u32 length prefix per record plus a "WLT2" hello,
UDP sends bare datagrams (no handshake).
"""

from __future__ import annotations

import struct

SOCKET_SCHEMA = 2
DOMAIN_SIZE = 32
CHANNEL_NAME_SIZE = 32
MAX_ITEMS = 1024
MAX_CHANNELS = 64
MAX_UDP_PAYLOAD = 65507
MAX_COMPACT_RECORD = 65507  # == MAX_UDP_PAYLOAD: every v2 record fits a datagram

TCP_MAGIC = b"WLT2"

TCP_FRAME_PREFIX_SIZE = 4
TCP_HELLO_SIZE = 8 + DOMAIN_SIZE

COMPACT_HEADER_SIZE = 84
COMPACT_ITEM_SIZE = 61
COMPACT_MAX_NAME = CHANNEL_NAME_SIZE - 1


def _decode_domain(raw: bytes) -> str:
    end = raw.find(b"\0")
    if end <= 0 or any(raw[end + 1 :]):
        raise ValueError("invalid or unterminated domain field")
    try:
        return raw[:end].decode("ascii")
    except UnicodeDecodeError as exc:
        raise ValueError("domain is not ASCII") from exc


def decode_tcp_hello(data: bytes) -> str:
    if len(data) != TCP_HELLO_SIZE:
        raise ValueError(f"TCP hello must be {TCP_HELLO_SIZE} bytes")
    magic, version, raw_domain = struct.unpack("<II32s", data)
    if magic != int.from_bytes(TCP_MAGIC, "little"):
        raise ValueError("bad TCP hello magic (expected WLT2)")
    if version != SOCKET_SCHEMA:
        raise ValueError(f"unsupported socket schema {version}, expected {SOCKET_SCHEMA}")
    return _decode_domain(raw_domain)


def decode_compact_record(data: bytes) -> tuple[int, int, int, float, list[tuple[str, float]]]:
    """Decode one compact v2 record (variable length, explicit LE fields).

    Returns (run_id, frame_seq, step_index, sim_time, channels). Only kind 1
    (frame) records decode; anything else raises, mirroring the C++ codec's
    loud refusal (see compact_codec.hpp for the full layout).
    """
    if len(data) < COMPACT_HEADER_SIZE:
        raise ValueError(f"compact record shorter than {COMPACT_HEADER_SIZE}-byte header")
    magic, schema = struct.unpack_from("<II", data, 0)
    if magic != int.from_bytes(b"WLV2", "little"):
        raise ValueError("bad compact record magic (expected WLV2)")
    if schema != SOCKET_SCHEMA:
        raise ValueError(f"unsupported socket schema {schema}, expected {SOCKET_SCHEMA}")
    kind = data[8]
    if kind != 1:
        raise ValueError(f"unsupported compact record kind {kind}")
    if bytes(data[9:12]) != b"\0\0\0":
        raise ValueError("compact record reserved bytes must be zero")
    run_id, frame_seq, step_index = struct.unpack_from("<QQQ", data, 12)
    (sim_time,) = struct.unpack_from("<d", data, 36)
    # Domain shape is validated here; the value itself rides the TCP hello
    # gate (UDP has no hello: datagrams stand alone, domain included).
    _decode_domain(data[44:76])
    item_count, channel_count = struct.unpack_from("<II", data, 76)
    if item_count > MAX_ITEMS or channel_count > MAX_CHANNELS:
        raise ValueError("compact record entry count exceeds protocol bounds")
    offset = COMPACT_HEADER_SIZE + item_count * COMPACT_ITEM_SIZE
    if offset > len(data):
        raise ValueError("compact record truncated in item block")
    # Items carry spatial glyphs the signal samples ignore, but lengths
    # still gate the parse: skip the block exactly.
    channels: list[tuple[str, float]] = []
    for index in range(channel_count):
        if offset + 1 > len(data):
            raise ValueError(f"compact channel {index} truncated")
        name_len = data[offset]
        if name_len > COMPACT_MAX_NAME or offset + 1 + name_len + 8 > len(data):
            raise ValueError(f"compact channel {index} has a bad name length")
        raw_name = data[offset + 1 : offset + 1 + name_len]
        try:
            name = raw_name.decode("utf-8")
        except UnicodeDecodeError as exc:
            raise ValueError(f"compact channel {index} name is not UTF-8") from exc
        (value,) = struct.unpack_from("<d", data, offset + 1 + name_len)
        channels.append((name, value))
        offset += 1 + name_len + 8
    if offset != len(data):
        raise ValueError("compact record has trailing garbage")
    return run_id, frame_seq, step_index, sim_time, channels


def format_signal_frame(
    frame_seq: int,
    step_index: int,
    sim_time: float,
    channels: list[tuple[str, float]],
    missed: int,
) -> str:
    values = " ".join(
        f"{name}={value:.17g}" for name, value in channels if name.startswith("sig.")
    )
    return (
        f"seq={frame_seq} step={step_index} t={sim_time:.17g}s missed={missed}"
        + (f" {values}" if values else " (no signal channels)")
    )
