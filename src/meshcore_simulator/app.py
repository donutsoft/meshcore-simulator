"""Web dashboard and companion connection manager for MeshCore Simulator."""

from __future__ import annotations

import argparse
import logging
import math
import os
import signal
import threading
from pathlib import Path

from flask import Flask, jsonify, render_template, request

from .companion import BleCompanionCapture, TcpCompanionCapture, UsbCompanionCapture
from .simulation import SimulationManager
from .storage import TopologyStore

DEFAULT_TCP_PORT = 5000
DEFAULT_USB_BAUDRATE = 115200
DEFAULT_TILE_URL = "https://tile.openstreetmap.org/{z}/{x}/{y}.png"
DEFAULT_TILE_ATTRIBUTION = (
    '&copy; <a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a> contributors'
)


class CaptureState:
    def __init__(self, store: TopologyStore) -> None:
        self.store = store
        self._lock = threading.Lock()
        self._status = "disconnected"
        self._transport: str | None = None
        self._received = 0
        self._logged = 0
        self._ignored = 0
        self._last_error: str | None = None

    def set_transport(self, transport: str | None) -> None:
        with self._lock:
            self._transport = transport

    def set_status(self, status: str) -> None:
        with self._lock:
            self._status = status

    def add_packet(self, packet: bytes, snr: float, rssi: int) -> None:
        try:
            result = self.store.ingest_packet(packet, snr, rssi)
        except Exception as exc:
            logging.getLogger(__name__).exception("could not persist received packet")
            with self._lock:
                self._received += 1
                self._last_error = str(exc)
            return
        with self._lock:
            self._received += 1
            if result["logged"]:
                self._logged += 1
            else:
                self._ignored += 1

    def snapshot(self) -> dict[str, object]:
        with self._lock:
            return {
                "status": self._status,
                "transport": self._transport,
                "received": self._received,
                "logged": self._logged,
                "ignored": self._ignored,
                "last_error": self._last_error,
            }


class TransportManager:
    def __init__(self, state: CaptureState) -> None:
        self.state = state
        self._lock = threading.RLock()
        self._active = None
        self._ble_candidate: BleCompanionCapture | None = None

    def _replace(self, adapter, transport: str) -> None:
        with self._lock:
            previous = self._active
            candidate = self._ble_candidate
            if previous is not None and previous is not adapter:
                previous.stop()
            if candidate is not None and candidate is not adapter:
                candidate.stop()
                self._ble_candidate = None
            self._active = adapter
            self.state.set_transport(transport)

    def connect_tcp(self, host: str, port: int) -> None:
        adapter = TcpCompanionCapture(host, port, self.state)
        self._replace(adapter, "tcp")
        adapter.start()

    def connect_usb(self, port: str, baudrate: int) -> None:
        adapter = UsbCompanionCapture(port, self.state, baudrate)
        self._replace(adapter, "usb")
        adapter.start()

    def scan_ble(self) -> list[dict[str, str]]:
        with self._lock:
            candidate = self._ble_candidate
            if candidate is None:
                candidate = BleCompanionCapture(self.state)
                self._ble_candidate = candidate
        return candidate.scan_devices()

    def connect_ble(self, device_id: str) -> None:
        with self._lock:
            adapter = self._ble_candidate or BleCompanionCapture(self.state)
            self._ble_candidate = None
        self._replace(adapter, "ble")
        adapter.connect(device_id)

    def disconnect(self) -> None:
        with self._lock:
            adapter, self._active = self._active, None
            candidate, self._ble_candidate = self._ble_candidate, None
            self.state.set_transport(None)
        if adapter is not None:
            adapter.stop()
        if candidate is not None and candidate is not adapter:
            candidate.stop()
        self.state.set_status("disconnected")


def create_app(
    database_path: Path | str,
    tile_url: str = DEFAULT_TILE_URL,
    tile_attribution: str = DEFAULT_TILE_ATTRIBUTION,
    simulation_manager: SimulationManager | None = None,
) -> Flask:
    app = Flask(__name__)
    store = TopologyStore(Path(database_path))
    state = CaptureState(store)
    transports = TransportManager(state)
    simulation = simulation_manager or SimulationManager(Path(database_path))
    app.extensions["meshcore_store"] = store
    app.extensions["meshcore_state"] = state
    app.extensions["meshcore_transports"] = transports
    app.extensions["meshcore_simulation"] = simulation

    @app.get("/")
    def index():
        return render_template(
            "index.html",
            map_config={"tileUrl": tile_url, "attribution": tile_attribution},
        )

    @app.get("/api/topology")
    def topology():
        result = store.topology()
        result["capture"] = state.snapshot()
        result["simulation"] = simulation.snapshot()
        return jsonify(result)

    @app.get("/api/simulation/companions")
    def simulation_status():
        return jsonify(simulation.snapshot())

    @app.post("/api/simulation/companions")
    def start_simulation_companions():
        body = request.get_json(silent=True) or {}
        repeaters = body.get("repeaters")
        packet_drop_rate = body.get("packet_drop_rate", 0.0)
        if not isinstance(repeaters, list) or len(repeaters) != 2:
            return jsonify({"error": "repeaters must contain exactly two repeater IDs"}), 400
        if any(
            not isinstance(repeater_id, str)
            or len(repeater_id) != 6
            or any(character not in "0123456789abcdefABCDEF" for character in repeater_id)
            for repeater_id in repeaters
        ):
            return jsonify({"error": "each repeater ID must be six hexadecimal characters"}), 400
        selected = tuple(repeater_id.upper() for repeater_id in repeaters)
        if selected[0] == selected[1]:
            return jsonify({"error": "select two different repeaters"}), 400
        if (
            isinstance(packet_drop_rate, bool)
            or not isinstance(packet_drop_rate, (int, float))
            or not math.isfinite(packet_drop_rate)
            or not 0.0 <= packet_drop_rate <= 1.0
        ):
            return jsonify({"error": "packet_drop_rate must be a number between 0 and 1"}), 400
        if not store.has_repeaters(selected):
            return jsonify({"error": "one or more selected repeaters are not in the database"}), 404
        try:
            result = simulation.start(selected, packet_drop_rate=float(packet_drop_rate))
        except (OSError, RuntimeError, ValueError) as exc:
            app.logger.exception("could not start native repeater simulation")
            return jsonify({"error": str(exc)}), 503
        return jsonify(result), 201

    @app.delete("/api/simulation/companions")
    def stop_simulation_companions():
        simulation.stop()
        return jsonify(simulation.snapshot())

    @app.get("/api/companion/usb/ports")
    def usb_ports():
        try:
            return jsonify({"ports": UsbCompanionCapture.list_ports()})
        except Exception as exc:
            return jsonify({"error": str(exc)}), 503

    @app.post("/api/companion/tcp/connect")
    def connect_tcp():
        body = request.get_json(silent=True) or {}
        host, port = body.get("host"), body.get("port", DEFAULT_TCP_PORT)
        if not isinstance(host, str) or not host.strip():
            return jsonify({"error": "host must be a non-empty string"}), 400
        if isinstance(port, bool) or not isinstance(port, int) or not 1 <= port <= 65535:
            return jsonify({"error": "port must be an integer between 1 and 65535"}), 400
        transports.connect_tcp(host.strip(), port)
        return jsonify({"connected": True, "transport": "tcp"}), 202

    @app.post("/api/companion/usb/connect")
    def connect_usb():
        body = request.get_json(silent=True) or {}
        port, baudrate = body.get("port"), body.get("baudrate", DEFAULT_USB_BAUDRATE)
        if not isinstance(port, str) or not port.strip():
            return jsonify({"error": "port must be a non-empty string"}), 400
        if isinstance(baudrate, bool) or not isinstance(baudrate, int) or not 300 <= baudrate <= 4_000_000:
            return jsonify({"error": "baudrate must be an integer between 300 and 4000000"}), 400
        transports.connect_usb(port.strip(), baudrate)
        return jsonify({"connected": True, "transport": "usb"}), 202

    @app.post("/api/companion/ble/scan")
    def scan_ble():
        try:
            return jsonify({"devices": transports.scan_ble()})
        except Exception as exc:
            app.logger.exception("BLE scan failed")
            return jsonify({"error": str(exc)}), 503

    @app.post("/api/companion/ble/connect")
    def connect_ble():
        body = request.get_json(silent=True) or {}
        device_id = body.get("device_id")
        if not isinstance(device_id, str) or not device_id.strip():
            return jsonify({"error": "device_id must be a non-empty string"}), 400
        try:
            transports.connect_ble(device_id.strip())
        except Exception as exc:
            app.logger.exception("BLE connection failed")
            transports.disconnect()
            return jsonify({"error": str(exc)}), 503
        return jsonify({"connected": True, "transport": "ble"}), 202

    @app.post("/api/companion/disconnect")
    def disconnect():
        transports.disconnect()
        return jsonify({"disconnected": True})

    return app


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--database", type=Path, default=Path("data/meshcore.sqlite3"))
    parser.add_argument("--tcp", metavar="HOST", help="connect to a TCP companion at startup")
    parser.add_argument("--tcp-port", type=int, default=DEFAULT_TCP_PORT)
    parser.add_argument("--usb", metavar="PORT", help="connect to a USB serial companion at startup")
    parser.add_argument("--usb-baudrate", type=int, default=DEFAULT_USB_BAUDRATE)
    parser.add_argument("--ble", metavar="DEVICE_ID", help="connect to a BLE companion at startup")
    parser.add_argument(
        "--import-adverts",
        metavar="PATH",
        type=Path,
        action="append",
        default=[],
        help="import a meshcore-security adverts JSONL file (repeatable)",
    )
    parser.add_argument("--import-only", action="store_true", help="exit after importing adverts")
    parser.add_argument("--web-host", default="127.0.0.1")
    parser.add_argument("--web-port", type=int, default=8080)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    selected = sum(value is not None for value in (args.tcp, args.usb, args.ble))
    if selected > 1:
        raise SystemExit("choose only one startup transport: --tcp, --usb, or --ble")
    app = create_app(
        args.database,
        os.environ.get("MESHCORE_TILE_URL", DEFAULT_TILE_URL),
        os.environ.get("MESHCORE_TILE_ATTRIBUTION", DEFAULT_TILE_ATTRIBUTION),
    )
    store: TopologyStore = app.extensions["meshcore_store"]
    for path in args.import_adverts:
        try:
            result = store.import_adverts(path)
        except OSError as exc:
            raise SystemExit(f"could not import {path}: {exc}") from exc
        print(
            f"Imported {result['observations_imported']} observations for "
            f"{result['repeaters_imported']} repeaters from {path} "
            f"({result['duplicates']} duplicates, {result['invalid']} invalid, "
            f"{result['prefix_collisions']} prefix collisions).",
            flush=True,
        )
    if args.import_only:
        return
    transports: TransportManager = app.extensions["meshcore_transports"]
    simulation: SimulationManager = app.extensions["meshcore_simulation"]
    if args.tcp:
        transports.connect_tcp(args.tcp, args.tcp_port)
    elif args.usb:
        transports.connect_usb(args.usb, args.usb_baudrate)
    elif args.ble:
        transports.connect_ble(args.ble)

    def shutdown(signum: int, _frame: object) -> None:
        transports.disconnect()
        simulation.stop()
        raise SystemExit(128 + signum)

    signal.signal(signal.SIGINT, shutdown)
    signal.signal(signal.SIGTERM, shutdown)
    try:
        app.run(host=args.web_host, port=args.web_port, threaded=True)
    finally:
        transports.disconnect()
        simulation.stop()


if __name__ == "__main__":
    main()
