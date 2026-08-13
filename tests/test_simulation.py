from pathlib import Path
from unittest.mock import patch

from meshcore_simulator.simulation import COMPANION_PORTS, SimulationManager


def test_simulation_snapshot_exposes_fixed_ports(tmp_path):
    manager = SimulationManager(tmp_path / "mesh.sqlite3", project_root=tmp_path)

    snapshot = manager.snapshot()

    assert snapshot["status"] == "stopped"
    assert snapshot["repeaters"] == [None, None]
    assert tuple(item["port"] for item in snapshot["companions"]) == COMPANION_PORTS
    assert [Path(item["storage_file"]).name for item in snapshot["companions"]] == [
        "companion-5000.fs",
        "companion-5001.fs",
    ]
    assert snapshot["packet_drop_rate"] == 0.0
    assert snapshot["bytes_sent"] == {}
    assert snapshot["active_routes"] == []


def test_packet_route_hops_are_highlighted_only_during_each_transmission(tmp_path):
    manager = SimulationManager(tmp_path / "mesh.sqlite3", project_root=tmp_path)
    with patch("meshcore_simulator.simulation.time.monotonic", return_value=100.0):
        manager._record_output_line(
            'ROUTE_START {"source":"010203","target":"A0B0C0","packet":"packet-1",'
            '"transmission":1000,"duration_ms":500}'
        )
    with patch("meshcore_simulator.simulation.time.monotonic", return_value=100.2):
        manager._record_output_line(
            'ROUTE_START {"source":"A0B0C0","target":"D0E0F0","packet":"packet-1",'
            '"transmission":1200,"duration_ms":1000}'
        )
    with patch("meshcore_simulator.simulation.time.monotonic", return_value=100.49):
        routes = manager.snapshot()["active_routes"]
    assert {(route["source"], route["target"]) for route in routes} == {
        ("010203", "A0B0C0"),
        ("A0B0C0", "D0E0F0"),
    }
    assert {route["packet"] for route in routes} == {"packet-1"}

    # The later hop does not extend the first hop's visibility.
    with patch("meshcore_simulator.simulation.time.monotonic", return_value=100.51):
        routes = manager.snapshot()["active_routes"]
    assert [(route["source"], route["target"]) for route in routes] == [
        ("A0B0C0", "D0E0F0")
    ]

    with patch("meshcore_simulator.simulation.time.monotonic", return_value=100.6):
        manager._record_output_line(
            'ROUTE_END {"source":"A0B0C0","target":"D0E0F0","packet":"packet-1",'
            '"transmission":1200}'
        )
        assert manager.snapshot()["active_routes"] == []


def test_active_routes_preserve_packet_identity_on_shared_edges(tmp_path):
    manager = SimulationManager(tmp_path / "mesh.sqlite3", project_root=tmp_path)
    with patch("meshcore_simulator.simulation.time.monotonic", return_value=100.0):
        manager._record_output_line(
            'ROUTE_START {"source":"010203","target":"A0B0C0","packet":"message-hash",'
            '"transmission":1000,"duration_ms":1000}'
        )
        manager._record_output_line(
            'ROUTE_START {"source":"010203","target":"A0B0C0","packet":"ack-hash",'
            '"transmission":1001,"duration_ms":1000}'
        )

    with patch("meshcore_simulator.simulation.time.monotonic", return_value=100.5):
        routes = manager.snapshot()["active_routes"]

    assert len(routes) == 2
    assert {route["packet"] for route in routes} == {"message-hash", "ack-hash"}


def test_repeats_add_packet_length_once_per_radio_transmission(tmp_path):
    manager = SimulationManager(tmp_path / "mesh.sqlite3", project_root=tmp_path)
    with patch("meshcore_simulator.simulation.time.monotonic", return_value=100.0):
        manager._record_output_line(
            'PACKET_SENT {"source":"010203","packet":"packet-1",'
            '"transmission":1000,"packet_length":42}'
        )
        # One flood transmission is displayed on two edges, but its bytes were sent once.
        manager._record_output_line(
            'ROUTE_START {"source":"010203","target":"A0B0C0","packet":"packet-1",'
            '"transmission":1000,"duration_ms":500}'
        )
        manager._record_output_line(
            'ROUTE_START {"source":"010203","target":"D0E0F0","packet":"packet-1",'
            '"transmission":1000,"duration_ms":500}'
        )
        # A later repeat adds that repeat's packet length, which may have changed.
        manager._record_output_line(
            'PACKET_SENT {"source":"010203","packet":"packet-1",'
            '"transmission":1500,"packet_length":45}'
        )
        manager._record_output_line(
            'ROUTE_START {"source":"010203","target":"A0B0C0","packet":"packet-1",'
            '"transmission":1500,"duration_ms":500}'
        )

        snapshot = manager.snapshot()

    assert snapshot["bytes_sent"] == {"packet-1": 87}
    assert {route["bytes_sent"] for route in snapshot["active_routes"]} == {87}


def test_tcp_frame_trace_is_relayed_to_web_server_terminal(tmp_path, capsys):
    manager = SimulationManager(tmp_path / "mesh.sqlite3", project_root=tmp_path)

    manager._record_output_line(
        "TCP_FRAME port=5001 direction=app_to_companion length=3 hex=01 02 FF"
    )

    assert capsys.readouterr().out == (
        "TCP_FRAME port=5001 direction=app_to_companion length=3 hex=01 02 FF\n"
    )
