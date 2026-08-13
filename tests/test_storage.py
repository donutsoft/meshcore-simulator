import sqlite3
import json

import pytest

from meshcore_simulator.storage import TopologyStore, _geographic_midpoint

from helpers import packet, repeater_advert


def key(repeater_id: str, fill: str) -> bytes:
    return bytes.fromhex(repeater_id + fill * 29)


def test_only_three_byte_routes_are_logged(tmp_path):
    store = TopologyStore(tmp_path / "mesh.sqlite3")

    ignored = store.ingest_packet(packet(["AABB"], hash_size=2), 1.5, -90)
    logged = store.ingest_packet(packet(["AABBCC"], hash_size=3), 2.0, -80)

    assert ignored["logged"] is False
    assert logged["logged"] is True
    assert store.topology()["stats"]["packets"] == 1


def test_every_repeater_advert_is_logged_even_with_short_route_ids(tmp_path):
    store = TopologyStore(tmp_path / "mesh.sqlite3")

    result = store.ingest_packet(
        repeater_advert(key("AABBCC", "11"), ["12"], hash_size=1, name="Repeater A"),
        1.0,
        -70,
    )

    assert result == {
        "logged": False,
        "packet_id": None,
        "advert_logged": True,
        "ignored_reason": "route does not use three-byte repeater IDs",
    }
    topology = store.topology()
    assert topology["stats"]["adverts"] == 1
    assert topology["nodes"] == []
    with sqlite3.connect(tmp_path / "mesh.sqlite3") as database:
        assert database.execute(
            "SELECT name FROM repeaters WHERE repeater_id = 'AABBCC'"
        ).fetchone()[0] == "Repeater A"


def test_advert_route_builds_links_and_infers_missing_midpoint(tmp_path):
    store = TopologyStore(tmp_path / "mesh.sqlite3")
    store.ingest_packet(
        repeater_advert(key("AAAAAA", "11"), ["BBBBBB", "CCCCCC"], latitude=47, longitude=-123, name="West"),
        1,
        -70,
    )
    store.ingest_packet(
        repeater_advert(key("CCCCCC", "22"), [], latitude=47, longitude=-121, name="East"),
        1,
        -70,
    )

    topology = store.topology()
    nodes = {node["id"]: node for node in topology["nodes"]}
    edges = {(edge["source"], edge["target"]) for edge in topology["edges"]}

    assert edges == {("AAAAAA", "BBBBBB"), ("BBBBBB", "CCCCCC")}
    assert nodes["BBBBBB"]["position_source"] == "midpoint"
    assert nodes["BBBBBB"]["latitude"] == pytest.approx(47.0043, abs=0.001)
    assert nodes["BBBBBB"]["longitude"] == pytest.approx(-122)
    assert nodes["BBBBBB"]["inferred_from"] == ["AAAAAA", "CCCCCC"]

    with sqlite3.connect(tmp_path / "mesh.sqlite3") as database:
        assert database.execute("SELECT route_json FROM packets ORDER BY id LIMIT 1").fetchone()[0] == '["BBBBBB","CCCCCC"]'
        assert database.execute("SELECT count(*) FROM packet_hops").fetchone()[0] == 2


def test_geographic_midpoint_handles_the_date_line():
    latitude, longitude = _geographic_midpoint([(0, 179), (0, -179)])

    assert latitude == pytest.approx(0)
    assert abs(longitude) == pytest.approx(180)


def test_security_advert_import_is_idempotent_and_skips_non_repeaters(tmp_path):
    store = TopologyStore(tmp_path / "mesh.sqlite3")
    source = tmp_path / "adverts.jsonl"
    public_key = "AABBCC" + "11" * 29
    repeater = {
        "received_at": "2026-08-09T21:40:54.471Z",
        "raw_hex": "1100AA",
        "pubkey": public_key,
        "advert_timestamp": 123,
        "node_type": 2,
        "name": "Imported repeater",
        "latitude": 47.6,
        "longitude": -122.3,
    }
    companion = {**repeater, "pubkey": "DDEEFF" + "22" * 29, "node_type": 1}
    source.write_text("\n".join(json.dumps(row) for row in (repeater, companion)) + "\n")

    first = store.import_adverts(source)
    second = store.import_adverts(source)

    assert first["observations_imported"] == 1
    assert first["repeaters_imported"] == 1
    assert first["non_repeaters"] == 1
    assert second["observations_imported"] == 0
    assert second["duplicates"] == 1
    assert store.topology()["nodes"] == []
    with sqlite3.connect(tmp_path / "mesh.sqlite3") as database:
        node = database.execute(
            "SELECT repeater_id, public_key, latitude FROM repeaters"
        ).fetchone()
    assert node == ("AABBCC", public_key, 47.6)


def test_security_advert_import_rejects_three_byte_prefix_collision(tmp_path):
    store = TopologyStore(tmp_path / "mesh.sqlite3")
    source = tmp_path / "adverts.jsonl"
    records = [
        {
            "received_at": f"2026-08-09T21:40:5{index}.000Z",
            "raw_hex": f"110{index}",
            "pubkey": "AABBCC" + fill * 29,
            "advert_timestamp": index,
            "node_type": 2,
            "name": f"Repeater {index}",
            "latitude": 47.6,
            "longitude": -122.3,
        }
        for index, fill in ((1, "11"), (2, "22"))
    ]
    source.write_text("\n".join(json.dumps(row) for row in records) + "\n")

    result = store.import_adverts(source)

    assert result["observations_imported"] == 1
    assert result["prefix_collisions"] == 1


def test_zero_zero_import_coordinates_are_filtered(tmp_path):
    store = TopologyStore(tmp_path / "mesh.sqlite3")
    source = tmp_path / "adverts.jsonl"
    source.write_text(json.dumps({
        "received_at": "2026-08-09T21:40:54.471Z",
        "raw_hex": "1100AA",
        "pubkey": "AABBCC" + "11" * 29,
        "advert_timestamp": 123,
        "node_type": 2,
        "name": "No GPS",
        "latitude": 0,
        "longitude": 0,
    }) + "\n")

    store.import_adverts(source)

    with sqlite3.connect(tmp_path / "mesh.sqlite3") as database:
        assert database.execute(
            "SELECT latitude, longitude FROM repeaters WHERE repeater_id = 'AABBCC'"
        ).fetchone() == (None, None)
        assert database.execute(
            "SELECT latitude, longitude FROM advert_observations"
        ).fetchone() == (None, None)


def test_topology_filters_nodes_without_links_but_keeps_them_in_sqlite(tmp_path):
    store = TopologyStore(tmp_path / "mesh.sqlite3")
    store.ingest_packet(
        repeater_advert(key("AAAAAA", "11"), [], latitude=47, longitude=-122),
        1,
        -70,
    )
    store.ingest_packet(packet(["BBBBBB", "CCCCCC"]), 1, -70)

    topology = store.topology()

    assert {node["id"] for node in topology["nodes"]} == {"BBBBBB", "CCCCCC"}
    assert topology["stats"]["repeaters"] == 2
    with sqlite3.connect(tmp_path / "mesh.sqlite3") as database:
        assert database.execute("SELECT count(*) FROM repeaters").fetchone()[0] == 3
