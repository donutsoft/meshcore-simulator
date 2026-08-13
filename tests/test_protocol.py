from meshcore_simulator.protocol import packet_route, parse_repeater_advert

from helpers import packet, repeater_advert


def test_route_splits_three_byte_repeater_ids():
    parsed = packet(["AABBCC", "102030"])
    route = packet_route(parsed)

    assert route is not None
    assert route.path_hash_size == 3
    assert route.route_type == "flood"
    assert route.repeater_ids == ("AABBCC", "102030")


def test_transport_route_skips_transport_codes():
    raw = bytes([(5 << 2) | 0]) + b"\x01\x02\x03\x04" + bytes([0x81]) + bytes.fromhex("AABBCC") + b"data"

    route = packet_route(raw)

    assert route is not None
    assert route.route_type == "transport-flood"
    assert route.repeater_ids == ("AABBCC",)


def test_repeater_advert_decodes_identity_and_coordinates():
    public_key = bytes.fromhex("AABBCC" + "11" * 29)
    advert = parse_repeater_advert(
        repeater_advert(public_key, ["102030"], latitude=37.7749, longitude=-122.4194, name="Hilltop")
    )

    assert advert is not None
    assert advert.repeater_id == "AABBCC"
    assert advert.name == "Hilltop"
    assert advert.latitude == 37.7749
    assert advert.longitude == -122.4194


def test_malformed_route_is_rejected():
    assert packet_route(bytes([(5 << 2) | 1, 0x82]) + bytes.fromhex("AABBCC")) is None


def test_zero_zero_advert_coordinates_are_treated_as_missing():
    public_key = bytes.fromhex("AABBCC" + "11" * 29)

    advert = parse_repeater_advert(repeater_advert(public_key, [], latitude=0, longitude=0))

    assert advert is not None
    assert advert.latitude is None
    assert advert.longitude is None
