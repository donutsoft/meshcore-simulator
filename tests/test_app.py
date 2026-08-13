from meshcore_simulator.app import create_app


class FakeSimulation:
    def __init__(self):
        self.repeaters = None
        self.running = False
        self.packet_drop_rate = 0.0

    def snapshot(self):
        return {
            "status": "running" if self.running else "stopped",
            "repeaters": list(self.repeaters) if self.repeaters else [None, None],
            "companions": [
                {"slot": 0, "port": 5000, "repeater_id": self.repeaters[0] if self.repeaters else None},
                {"slot": 1, "port": 5001, "repeater_id": self.repeaters[1] if self.repeaters else None},
            ],
            "last_error": None,
            "packet_drop_rate": self.packet_drop_rate,
            "active_routes": [],
        }

    def start(self, repeaters, packet_drop_rate=0.0):
        self.repeaters = repeaters
        self.packet_drop_rate = packet_drop_rate
        self.running = True
        return self.snapshot()

    def stop(self):
        self.running = False


def test_dashboard_and_empty_topology(tmp_path):
    app = create_app(tmp_path / "mesh.sqlite3")
    client = app.test_client()

    page = client.get("/")
    topology = client.get("/api/topology")

    assert page.status_code == 200
    assert b"MeshCore Simulator" in page.data
    assert b"leaflet@1.9.4" in page.data
    assert b"tile.openstreetmap.org" in page.data
    assert b"Coincident estimates are spread" in page.data
    assert b"TCP 5000" in page.data
    assert b"TCP 5001" in page.data
    assert b"Random packet drop rate (%)" in page.data
    assert b"Bytes sent per packet" in page.data
    assert b"Repeater transmission currently on air" in page.data
    assert b"GOOGLE_MAPS_API_KEY" not in page.data
    assert topology.status_code == 200
    assert topology.get_json()["stats"] == {"adverts": 0, "links": 0, "packets": 0, "repeaters": 0}


def test_connection_validation(tmp_path):
    app = create_app(tmp_path / "mesh.sqlite3")
    client = app.test_client()

    assert client.post("/api/companion/tcp/connect", json={"host": ""}).status_code == 400
    assert client.post("/api/companion/usb/connect", json={"port": ""}).status_code == 400
    assert client.post("/api/companion/ble/connect", json={"device_id": ""}).status_code == 400


def test_custom_tile_provider_is_embedded_as_json(tmp_path):
    app = create_app(
        tmp_path / "mesh.sqlite3",
        "https://tiles.example/{z}/{x}/{y}.png",
        "Example maps",
    )

    page = app.test_client().get("/")

    assert b"https://tiles.example/{z}/{x}/{y}.png" in page.data
    assert b"Example maps" in page.data


def test_two_map_repeaters_start_and_stop_native_companions(tmp_path):
    simulation = FakeSimulation()
    app = create_app(tmp_path / "mesh.sqlite3", simulation_manager=simulation)
    store = app.extensions["meshcore_store"]
    # A two-hop, three-byte route remembers both repeaters and links them.
    store.ingest_packet(bytes([0x3D, 0x82]) + bytes.fromhex("010203A0B0C0") + b"x", 5.0, -80)
    client = app.test_client()

    response = client.post(
        "/api/simulation/companions",
        json={"repeaters": ["010203", "a0b0c0"], "packet_drop_rate": 0.125},
    )

    assert response.status_code == 201
    assert response.get_json()["repeaters"] == ["010203", "A0B0C0"]
    assert response.get_json()["packet_drop_rate"] == 0.125
    topology = client.get("/api/topology").get_json()
    assert topology["simulation"]["status"] == "running"
    assert [item["port"] for item in topology["simulation"]["companions"]] == [5000, 5001]

    stopped = client.delete("/api/simulation/companions")
    assert stopped.status_code == 200
    assert stopped.get_json()["status"] == "stopped"


def test_simulated_companion_selection_validation(tmp_path):
    app = create_app(tmp_path / "mesh.sqlite3", simulation_manager=FakeSimulation())
    client = app.test_client()

    assert client.post("/api/simulation/companions", json={"repeaters": []}).status_code == 400
    assert client.post(
        "/api/simulation/companions", json={"repeaters": ["010203", "010203"]}
    ).status_code == 400
    assert client.post(
        "/api/simulation/companions", json={"repeaters": ["010203", "ABCDEF"]}
    ).status_code == 404


def test_simulated_packet_drop_rate_validation(tmp_path):
    app = create_app(tmp_path / "mesh.sqlite3", simulation_manager=FakeSimulation())
    client = app.test_client()

    for value in (-0.01, 1.01, True, "10"):
        response = client.post(
            "/api/simulation/companions",
            json={"repeaters": ["010203", "A0B0C0"], "packet_drop_rate": value},
        )
        assert response.status_code == 400
