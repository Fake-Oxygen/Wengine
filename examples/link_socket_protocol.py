"""Small Python decoder for the WEngine signal TCP/UDP socket examples.

Wire constants follow include/wengine/link/socket_publisher.hpp and
include/wengine/link/protocol.hpp (currently protocol v1).
"""

from __future__ import annotations

import struct

LINK_VERSION = 1
DOMAIN_SIZE = 32
CHANNEL_NAME_SIZE = 32
MAX_ITEMS = 1024
MAX_CHANNELS = 64
MAX_UDP_PAYLOAD = 65507

TCP_MAGIC = b"WLT1"
UDP_MAGIC = b"WLU1"

LINK_ITEM_SIZE = 64
LINK_CHANNEL_SIZE = 40
TCP_FRAME_SIZE = 8 * 4 + MAX_ITEMS * LINK_ITEM_SIZE + 8 + MAX_CHANNELS * LINK_CHANNEL_SIZE
TCP_FRAME_PREFIX_SIZE = 4
TCP_HELLO_SIZE = 8 + DOMAIN_SIZE
UDP_HEADER_FORMAT = "<II32sQQdQQ"
UDP_HEADER_SIZE = struct.calcsize(UDP_HEADER_FORMAT)


def _decode_domain(raw: bytes) -> str:
    end = raw.find(b"\0")
    if end <= 0 or any(raw[end + 1 :]):
        raise ValueError("invalid or unterminated domain field")
    try:
        return raw[:end].decode("ascii")
    except UnicodeDecodeError as exc:
        raise ValueError("domain is not ASCII") from exc


def _decode_channels(data: bytes, offset: int, count: int) -> list[tuple[str, float]]:
    channels = []
    for index in range(count):
        entry = offset + index * LINK_CHANNEL_SIZE
        raw_name = data[entry : entry + CHANNEL_NAME_SIZE]
        end = raw_name.find(b"\0")
        if end < 0:
            raise ValueError(f"channel {index} name is unterminated")
        try:
            name = raw_name[:end].decode("utf-8")
        except UnicodeDecodeError as exc:
            raise ValueError(f"channel {index} name is not UTF-8") from exc
        value = struct.unpack_from("<d", data, entry + CHANNEL_NAME_SIZE)[0]
        channels.append((name, value))
    return channels


def decode_tcp_hello(data: bytes) -> str:
    if len(data) != TCP_HELLO_SIZE:
        raise ValueError(f"TCP hello must be {TCP_HELLO_SIZE} bytes")
    magic, version, raw_domain = struct.unpack("<II32s", data)
    if magic != int.from_bytes(TCP_MAGIC, "little"):
        raise ValueError("bad TCP hello magic (expected WLT1)")
    if version != LINK_VERSION:
        raise ValueError(f"unsupported link version {version}, expected {LINK_VERSION}")
    return _decode_domain(raw_domain)


def decode_tcp_frame(data: bytes) -> tuple[int, int, float, list[tuple[str, float]]]:
    """Decode a full fixed-capacity LinkFrame payload (without its u32 prefix).

    TCP LinkFrame is the v1 same-host layout. This sample expects little-endian
    IEEE-754 doubles, as used by the WEngine POSIX/Linux build and typical
    little-endian Zephyr targets.
    """
    if len(data) != TCP_FRAME_SIZE:
        raise ValueError(f"TCP frame must be {TCP_FRAME_SIZE} bytes, got {len(data)}")
    frame_seq, step_index, sim_time, item_count = struct.unpack_from("<QQdQ", data, 0)
    if item_count > MAX_ITEMS:
        raise ValueError(f"item count exceeds protocol bound: {item_count}")
    channel_count_offset = 32 + MAX_ITEMS * LINK_ITEM_SIZE
    channel_count = struct.unpack_from("<Q", data, channel_count_offset)[0]
    if channel_count > MAX_CHANNELS:
        raise ValueError(f"channel count exceeds protocol bound: {channel_count}")
    channels_offset = channel_count_offset + 8
    channels = _decode_channels(data, channels_offset, channel_count)
    return frame_seq, step_index, sim_time, channels


def decode_udp_datagram(
    data: bytes, expected_domain: str = "signal"
) -> tuple[int, int, float, list[tuple[str, float]]]:
    if len(data) < UDP_HEADER_SIZE or len(data) > MAX_UDP_PAYLOAD:
        raise ValueError(f"invalid UDP datagram length: {len(data)}")
    magic, version, raw_domain, frame_seq, step_index, sim_time, item_count, channel_count = (
        struct.unpack_from(UDP_HEADER_FORMAT, data, 0)
    )
    if magic != int.from_bytes(UDP_MAGIC, "little"):
        raise ValueError("bad UDP datagram magic (expected WLU1)")
    if version != LINK_VERSION:
        raise ValueError(f"unsupported link version {version}, expected {LINK_VERSION}")
    domain = _decode_domain(raw_domain)
    if domain != expected_domain:
        raise ValueError(f"expected {expected_domain!r} domain, received {domain!r}")
    if item_count > MAX_ITEMS or channel_count > MAX_CHANNELS:
        raise ValueError("UDP entry count exceeds protocol bounds")
    expected = UDP_HEADER_SIZE + item_count * LINK_ITEM_SIZE + channel_count * LINK_CHANNEL_SIZE
    if len(data) != expected:
        raise ValueError(f"UDP datagram length mismatch: expected {expected}, got {len(data)}")
    channels_offset = UDP_HEADER_SIZE + item_count * LINK_ITEM_SIZE
    channels = _decode_channels(data, channels_offset, channel_count)
    return frame_seq, step_index, sim_time, channels


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
