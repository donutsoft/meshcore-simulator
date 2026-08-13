"""Dependency-free parsing for the parts of MeshCore packets we persist."""

from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime, timezone

MAX_FRAME_LENGTH = 176
PUSH_CODE_LOG_RX_DATA = 0x88
PAYLOAD_TYPE_ADVERT = 0x04
PUB_KEY_SIZE = 32
SIGNATURE_SIZE = 64
MAX_ADVERT_DATA_SIZE = 32
ADV_LATLON_MASK = 0x10
ADV_NAME_MASK = 0x80
ADV_TYPE_REPEATER = 2

PACKET_TYPES = {
    0x00: "request",
    0x01: "response",
    0x02: "text",
    0x03: "ack",
    0x04: "advert",
    0x05: "group-text",
    0x06: "group-data",
    0x07: "anonymous-request",
    0x08: "path",
    0x09: "trace",
    0x0A: "multipart",
    0x0B: "control",
    0x0F: "raw-custom",
}


@dataclass(frozen=True)
class PacketRoute:
    route_type: str
    path_hash_size: int
    repeater_ids: tuple[str, ...]


@dataclass(frozen=True)
class RepeaterAdvert:
    repeater_id: str
    public_key: str
    timestamp: int
    name: str | None
    latitude: float | None
    longitude: float | None


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z")


def signed_byte(value: int) -> int:
    return value - 256 if value > 127 else value


def packet_type(raw_packet: bytes) -> str:
    if not raw_packet:
        return "invalid"
    type_id = (raw_packet[0] >> 2) & 0x0F
    return PACKET_TYPES.get(type_id, f"unknown-{type_id:02x}")


def packet_route(raw_packet: bytes) -> PacketRoute | None:
    """Parse the route header and split the path into one-to-three-byte IDs."""
    if len(raw_packet) < 2:
        return None
    route_id = raw_packet[0] & 0x03
    path_offset = 1 + (4 if route_id in (0x00, 0x03) else 0)
    if len(raw_packet) <= path_offset:
        return None
    path_length = raw_packet[path_offset]
    hash_size = (path_length >> 6) + 1
    hash_count = path_length & 0x3F
    if hash_size > 3:
        return None
    first = path_offset + 1
    end = first + hash_size * hash_count
    if end > len(raw_packet):
        return None
    path = raw_packet[first:end]
    route_types = {
        0x00: "transport-flood",
        0x01: "flood",
        0x02: "direct",
        0x03: "transport-direct",
    }
    return PacketRoute(
        route_type=route_types[route_id],
        path_hash_size=hash_size,
        repeater_ids=tuple(
            path[offset : offset + hash_size].hex().upper()
            for offset in range(0, len(path), hash_size)
        ),
    )


def packet_payload(raw_packet: bytes) -> bytes | None:
    route = packet_route(raw_packet)
    if route is None:
        return None
    route_id = raw_packet[0] & 0x03
    path_offset = 1 + (4 if route_id in (0x00, 0x03) else 0)
    payload_offset = path_offset + 1 + len(route.repeater_ids) * route.path_hash_size
    return raw_packet[payload_offset:] if payload_offset < len(raw_packet) else None


def parse_repeater_advert(raw_packet: bytes) -> RepeaterAdvert | None:
    """Decode a repeater advert, including the public-key-prefix repeater ID."""
    if not raw_packet or ((raw_packet[0] >> 2) & 0x0F) != PAYLOAD_TYPE_ADVERT:
        return None
    payload = packet_payload(raw_packet)
    minimum = PUB_KEY_SIZE + 4 + SIGNATURE_SIZE + 1
    if payload is None or len(payload) < minimum:
        return None
    public_key = payload[:PUB_KEY_SIZE]
    timestamp = int.from_bytes(payload[PUB_KEY_SIZE : PUB_KEY_SIZE + 4], "little")
    app_data = payload[PUB_KEY_SIZE + 4 + SIGNATURE_SIZE :]
    if not app_data or len(app_data) > MAX_ADVERT_DATA_SIZE:
        return None
    flags = app_data[0]
    if (flags & 0x0F) != ADV_TYPE_REPEATER:
        return None
    offset = 1
    latitude = longitude = None
    if flags & ADV_LATLON_MASK:
        if len(app_data) < offset + 8:
            return None
        latitude = int.from_bytes(app_data[offset : offset + 4], "little", signed=True) / 1_000_000
        longitude = int.from_bytes(app_data[offset + 4 : offset + 8], "little", signed=True) / 1_000_000
        if not (-90 <= latitude <= 90 and -180 <= longitude <= 180):
            return None
        if latitude == 0 and longitude == 0:
            latitude = longitude = None
        offset += 8
    if flags & 0x20:
        offset += 2
    if flags & 0x40:
        offset += 2
    if len(app_data) < offset:
        return None
    name = None
    if flags & ADV_NAME_MASK:
        name = app_data[offset:].split(b"\x00", 1)[0].decode("utf-8", errors="replace") or None
    return RepeaterAdvert(
        repeater_id=public_key[:3].hex().upper(),
        public_key=public_key.hex().upper(),
        timestamp=timestamp,
        name=name,
        latitude=latitude,
        longitude=longitude,
    )
