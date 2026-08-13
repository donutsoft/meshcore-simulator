from meshcore_simulator.protocol import ADV_LATLON_MASK, ADV_NAME_MASK, ADV_TYPE_REPEATER, PAYLOAD_TYPE_ADVERT


def packet(path: list[str], *, hash_size: int = 3, payload: bytes = b"payload", type_id: int = 5) -> bytes:
    route_header = ((hash_size - 1) << 6) | len(path)
    return bytes([(type_id << 2) | 1, route_header]) + b"".join(bytes.fromhex(item) for item in path) + payload


def repeater_advert(
    public_key: bytes,
    path: list[str],
    *,
    hash_size: int = 3,
    latitude: float | None = None,
    longitude: float | None = None,
    name: str | None = None,
) -> bytes:
    flags = ADV_TYPE_REPEATER
    app_data = b""
    if latitude is not None and longitude is not None:
        flags |= ADV_LATLON_MASK
        app_data += round(latitude * 1_000_000).to_bytes(4, "little", signed=True)
        app_data += round(longitude * 1_000_000).to_bytes(4, "little", signed=True)
    if name is not None:
        flags |= ADV_NAME_MASK
        app_data += name.encode()
    payload = public_key + (123456).to_bytes(4, "little") + bytes(64) + bytes([flags]) + app_data
    return packet(path, hash_size=hash_size, payload=payload, type_id=PAYLOAD_TYPE_ADVERT)
