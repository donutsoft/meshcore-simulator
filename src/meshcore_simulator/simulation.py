"""Lifecycle manager for the native MeshCore repeater simulation."""

from __future__ import annotations

import json
import math
import socket
import subprocess
import threading
import time
from pathlib import Path

COMPANION_PORTS = (5000, 5001)


class SimulationManager:
    """Build and supervise one interactive native simulation process."""

    def __init__(self, database_path: Path, project_root: Path | None = None) -> None:
        self.database_path = Path(database_path).resolve()
        self.project_root = (
            Path(project_root).resolve()
            if project_root is not None
            else Path(__file__).resolve().parents[2]
        )
        self.simulator_directory = self.project_root / "MeshCore" / "simulator"
        self.executable = self.simulator_directory / "build" / "meshcore-repeater-sim"
        self.companion_storage = self.database_path.parent / "companion-state"
        self._lock = threading.RLock()
        self._process: subprocess.Popen[str] | None = None
        self._stdout_thread: threading.Thread | None = None
        self._repeaters: tuple[str, str] | None = None
        self._packet_drop_rate = 0.0
        self._last_error: str | None = None
        self._packet_routes: dict[tuple[str, str, str, int], float] = {}
        self._packet_bytes_sent: dict[str, int] = {}
        self._counted_transmissions: set[tuple[str, str, int]] = set()

    def _build(self) -> None:
        if not self.simulator_directory.is_dir():
            raise RuntimeError("MeshCore/simulator is missing; clone and configure the native harness first")
        result = subprocess.run(
            ["make", "-C", str(self.simulator_directory)],
            cwd=self.project_root,
            capture_output=True,
            text=True,
            timeout=120,
            check=False,
        )
        if result.returncode != 0:
            detail = result.stderr.strip() or result.stdout.strip() or "unknown build error"
            raise RuntimeError(f"could not build the native simulator: {detail}")

    def _refresh_process(self) -> None:
        process = self._process
        if process is None or process.poll() is None:
            return
        error = process.stderr.read().strip() if process.stderr is not None else ""
        if error:
            self._last_error = error.splitlines()[-1]
        self._process = None
        self._packet_routes.clear()

    def _record_output_line(self, line: str) -> None:
        if line.startswith("TCP_FRAME "):
            print(line, flush=True)
            return
        if line.startswith("PACKET_SENT "):
            try:
                event = json.loads(line[12:])
                source = event["source"]
                packet = event["packet"]
                transmission = event["transmission"]
                packet_length = event["packet_length"]
            except (json.JSONDecodeError, KeyError, TypeError):
                return
            if (
                not isinstance(source, str)
                or not isinstance(packet, str)
                or isinstance(transmission, bool)
                or not isinstance(transmission, int)
                or transmission < 0
                or isinstance(packet_length, bool)
                or not isinstance(packet_length, int)
                or packet_length <= 0
            ):
                return
            with self._lock:
                self._count_packet_transmission(
                    source, packet, transmission, packet_length
                )
            return
        active = line.startswith("ROUTE_START ")
        if not active and not line.startswith("ROUTE_END "):
            return
        try:
            event = json.loads(line[12:] if active else line[10:])
            source = event["source"]
            target = event["target"]
            packet = event["packet"]
            transmission = event["transmission"]
        except (json.JSONDecodeError, KeyError, TypeError):
            return
        if (
            not isinstance(source, str)
            or not isinstance(target, str)
            or not isinstance(packet, str)
            or isinstance(transmission, bool)
            or not isinstance(transmission, int)
            or transmission < 0
        ):
            return
        key = (source, target, packet, transmission)
        with self._lock:
            if not active:
                self._packet_routes.pop(key, None)
                return
            duration_ms = event.get("duration_ms")
            if (
                isinstance(duration_ms, bool)
                or not isinstance(duration_ms, int)
                or duration_ms <= 0
            ):
                return
            self._packet_routes[key] = time.monotonic() + duration_ms / 1000.0
            packet_length = event.get("packet_length")
            if (
                not isinstance(packet_length, bool)
                and isinstance(packet_length, int)
                and packet_length > 0
            ):
                # Accept packet lengths from older native simulators while
                # deduplicating the edge event emitted for every flood receiver.
                self._count_packet_transmission(
                    source, packet, transmission, packet_length
                )

    def _count_packet_transmission(
        self, source: str, packet: str, transmission: int, packet_length: int
    ) -> None:
        transmission_key = (source, packet, transmission)
        if transmission_key in self._counted_transmissions:
            return
        self._counted_transmissions.add(transmission_key)
        self._packet_bytes_sent[packet] = (
            self._packet_bytes_sent.get(packet, 0) + packet_length
        )

    def _read_stdout(self, process: subprocess.Popen[str]) -> None:
        if process.stdout is None:
            return
        for line in process.stdout:
            self._record_output_line(line.strip())

    def start(
        self,
        repeaters: tuple[str, str],
        packet_drop_rate: float = 0.0,
    ) -> dict[str, object]:
        if len(set(repeaters)) != 2:
            raise ValueError("select two different repeaters")
        if (
            isinstance(packet_drop_rate, bool)
            or not isinstance(packet_drop_rate, (int, float))
            or not math.isfinite(packet_drop_rate)
            or not 0.0 <= packet_drop_rate <= 1.0
        ):
            raise ValueError("packet drop rate must be between 0 and 1")
        self.stop()
        with self._lock:
            self._last_error = None
            self._packet_routes.clear()
            self._packet_bytes_sent.clear()
            self._counted_transmissions.clear()
            self._packet_drop_rate = packet_drop_rate
            self._build()
            command = [
                str(self.executable),
                "--database",
                str(self.database_path),
                "--duration-ms",
                "0",
                "--realtime",
                "--links",
                "undirected",
                "--companion-storage",
                str(self.companion_storage),
                "--interference",
                str(packet_drop_rate),
                "--companion",
                f"{repeaters[0]}@{COMPANION_PORTS[0]}",
                "--companion",
                f"{repeaters[1]}@{COMPANION_PORTS[1]}",
            ]
            process = subprocess.Popen(
                command,
                cwd=self.project_root,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            self._process = process
            self._repeaters = repeaters
            self._stdout_thread = threading.Thread(
                target=self._read_stdout,
                args=(process,),
                name="meshcore-simulation-events",
                daemon=True,
            )
            self._stdout_thread.start()

        deadline = time.monotonic() + 3.0
        while time.monotonic() < deadline:
            if process.poll() is not None:
                detail = process.stderr.read().strip() if process.stderr is not None else ""
                with self._lock:
                    self._process = None
                    self._last_error = detail.splitlines()[-1] if detail else "native simulator exited during startup"
                raise RuntimeError(self._last_error)
            ready = True
            for port in COMPANION_PORTS:
                try:
                    with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                        pass
                except OSError:
                    ready = False
                    break
            if ready:
                return self.snapshot()
            time.sleep(0.05)

        self.stop()
        with self._lock:
            self._last_error = "native simulator did not open TCP ports 5000 and 5001"
        raise RuntimeError(self._last_error)

    def stop(self) -> None:
        with self._lock:
            process, self._process = self._process, None
            stdout_thread, self._stdout_thread = self._stdout_thread, None
            self._packet_routes.clear()
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=2)
        if stdout_thread is not None and stdout_thread.is_alive():
            stdout_thread.join(timeout=1)

    def snapshot(self) -> dict[str, object]:
        with self._lock:
            self._refresh_process()
            running = self._process is not None
            repeaters = self._repeaters
            now = time.monotonic()
            self._packet_routes = {
                route: expiry for route, expiry in self._packet_routes.items() if expiry > now
            }
            return {
                "status": "running" if running else "stopped",
                "repeaters": list(repeaters) if repeaters else [None, None],
                "companions": [
                    {
                        "slot": index,
                        "port": port,
                        "repeater_id": repeaters[index] if repeaters else None,
                        "storage_file": str(self.companion_storage / f"companion-{port}.fs"),
                    }
                    for index, port in enumerate(COMPANION_PORTS)
                ],
                "last_error": self._last_error,
                "packet_drop_rate": self._packet_drop_rate,
                "bytes_sent": dict(self._packet_bytes_sent),
                "active_routes": [
                    {
                        "source": source,
                        "target": target,
                        "packet": packet,
                        "transmission": transmission,
                        "bytes_sent": self._packet_bytes_sent.get(packet, 0),
                        "remaining_ms": max(0, round((expiry - now) * 1000)),
                    }
                    for (source, target, packet, transmission), expiry in sorted(
                        self._packet_routes.items(), key=lambda item: (item[1], item[0])
                    )
                ],
            }
