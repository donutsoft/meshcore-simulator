"""TCP, USB, and BLE adapters copied and pared down from meshcore-security."""

from __future__ import annotations

import asyncio
import queue
import socket
import threading
from typing import Protocol

from .protocol import MAX_FRAME_LENGTH, PUSH_CODE_LOG_RX_DATA, signed_byte

COMPANION_CMD_SEND_RAW_PACKET = 0x41
COMPANION_CMD_DEVICE_QUERY = 0x16
COMPANION_RESP_OK = 0x00
COMPANION_RESP_ERR = 0x01
COMPANION_RESP_DEVICE_INFO = 0x0D
USB_SERIAL_MAX_PAYLOAD_LENGTH = 172
USB_MAX_RAW_PACKET_LENGTH = USB_SERIAL_MAX_PAYLOAD_LENGTH - 2
MAX_RAW_PACKET_LENGTH = MAX_FRAME_LENGTH - 2


class CaptureSink(Protocol):
    def set_status(self, status: str) -> None: ...

    def add_packet(self, packet: bytes, snr: float, rssi: int) -> None: ...


class BaseCompanionCapture:
    """Common companion-frame handling shared by all transports."""

    def __init__(self, sink: CaptureSink) -> None:
        self.sink = sink
        self.stop_event = threading.Event()
        self._responses: queue.Queue[bytes] = queue.Queue()
        self._send_lock = threading.Lock()

    def start(self) -> None:
        raise NotImplementedError

    def stop(self) -> None:
        self.stop_event.set()

    def process_frame(self, frame: bytes) -> None:
        frame = bytes(frame)
        if frame and frame[0] in (COMPANION_RESP_OK, COMPANION_RESP_ERR):
            self._responses.put(frame)
            return
        if len(frame) >= 4 and frame[0] == PUSH_CODE_LOG_RX_DATA:
            self.sink.add_packet(frame[3:], signed_byte(frame[1]) / 4.0, signed_byte(frame[2]))

    def _drain_responses(self) -> None:
        while True:
            try:
                self._responses.get_nowait()
            except queue.Empty:
                return

    def _check_response(self, timeout: float) -> None:
        try:
            response = self._responses.get(timeout=timeout)
        except queue.Empty as exc:
            raise RuntimeError("companion did not acknowledge the command") from exc
        if not response or response[0] == COMPANION_RESP_OK:
            return
        error_code = response[1] if len(response) > 1 else "unknown"
        raise RuntimeError(f"companion rejected command (error {error_code})")

    @staticmethod
    def _raw_command(packet: bytes, priority: int, maximum: int) -> bytes:
        packet = bytes(packet)
        if not 0 <= priority <= 0xFF:
            raise ValueError("priority must be a byte")
        if not 1 <= len(packet) <= maximum:
            raise ValueError(f"raw packet must be 1 to {maximum} bytes")
        return bytes([COMPANION_CMD_SEND_RAW_PACKET, priority]) + packet


class TcpCompanionCapture(BaseCompanionCapture):
    """Reconnectable MeshCore TCP companion client."""

    def __init__(self, host: str, port: int, sink: CaptureSink) -> None:
        super().__init__(sink)
        self.host, self.port = host, port
        self._socket: socket.socket | None = None
        self._socket_lock = threading.Lock()
        self._thread = threading.Thread(target=self._capture_loop, name="meshcore-tcp", daemon=True)

    def start(self) -> None:
        self._thread.start()

    def stop(self) -> None:
        super().stop()
        with self._socket_lock:
            companion = self._socket
        if companion is not None:
            try:
                companion.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        if self._thread.is_alive():
            self._thread.join(timeout=2)
        self.sink.set_status("disconnected")

    def send_raw_packet(self, packet: bytes, *, priority: int = 0, timeout: float = 5.0) -> None:
        command = self._raw_command(packet, priority, MAX_RAW_PACKET_LENGTH)
        with self._send_lock:
            with self._socket_lock:
                companion = self._socket
            if companion is None:
                raise RuntimeError("companion is not connected")
            self._drain_responses()
            companion.sendall(b"<" + len(command).to_bytes(2, "little") + command)
            self._check_response(timeout)

    def _capture_loop(self) -> None:
        while not self.stop_event.is_set():
            self.sink.set_status("connecting")
            companion = None
            try:
                with socket.create_connection((self.host, self.port), timeout=10) as companion:
                    companion.settimeout(1)
                    with self._socket_lock:
                        self._socket = companion
                    self.sink.set_status("connected")
                    while not self.stop_event.is_set():
                        frame = self._read_frame(companion)
                        if frame is not None:
                            self.process_frame(frame)
            except (EOFError, OSError, RuntimeError) as exc:
                if not self.stop_event.is_set():
                    self.sink.set_status(f"disconnected: {exc}")
                    self.stop_event.wait(2)
            finally:
                with self._socket_lock:
                    if self._socket is companion:
                        self._socket = None

    def _read_frame(self, companion: socket.socket) -> bytes | None:
        marker = self._recv_exact(companion, 1)
        if marker is None:
            return None
        if marker != b">":
            raise RuntimeError(f"unexpected frame marker 0x{marker[0]:02X}")
        length_bytes = self._recv_exact(companion, 2)
        if length_bytes is None:
            return None
        length = int.from_bytes(length_bytes, "little")
        if not 1 <= length <= MAX_FRAME_LENGTH:
            raise RuntimeError(f"invalid companion frame length {length}")
        return self._recv_exact(companion, length)

    def _recv_exact(self, companion: socket.socket, size: int) -> bytes | None:
        data = bytearray()
        while len(data) < size and not self.stop_event.is_set():
            try:
                chunk = companion.recv(size - len(data))
            except socket.timeout:
                continue
            if not chunk:
                raise EOFError("companion closed the connection")
            data.extend(chunk)
        return bytes(data) if len(data) == size else None


class UsbCompanionCapture(BaseCompanionCapture):
    """MeshCore USB serial client with streaming frame reassembly."""

    def __init__(self, port: str, sink: CaptureSink, baudrate: int = 115200) -> None:
        super().__init__(sink)
        self.port, self.baudrate = port, baudrate
        self._serial = None
        self._serial_lock = threading.Lock()
        self._thread = threading.Thread(target=self._capture_loop, name="meshcore-usb", daemon=True)

    @staticmethod
    def list_ports() -> list[dict[str, str]]:
        try:
            from serial.tools import list_ports
        except ImportError as exc:
            raise RuntimeError("USB support requires pyserial") from exc
        return [
            {
                "id": str(port.device),
                "name": " — ".join(
                    item for item in (str(port.device), str(getattr(port, "description", "") or "")) if item
                ),
            }
            for port in list_ports.comports()
        ]

    def start(self) -> None:
        self._thread.start()

    def stop(self) -> None:
        super().stop()
        with self._serial_lock:
            companion, self._serial = self._serial, None
        if companion is not None:
            try:
                companion.close()
            except Exception:
                pass
        if self._thread.is_alive():
            self._thread.join(timeout=2)
        self.sink.set_status("disconnected")

    def send_raw_packet(self, packet: bytes, *, priority: int = 0, timeout: float = 5.0) -> None:
        command = self._raw_command(packet, priority, USB_MAX_RAW_PACKET_LENGTH)
        with self._send_lock:
            with self._serial_lock:
                companion = self._serial
            if companion is None:
                raise RuntimeError("companion is not connected")
            self._drain_responses()
            framed = b"<" + len(command).to_bytes(2, "little") + command
            written = companion.write(framed)
            if written is not None and written != len(framed):
                raise OSError(f"short USB write: wrote {written} of {len(framed)} bytes")
            self._check_response(timeout)

    def _capture_loop(self) -> None:
        try:
            import serial
        except ImportError:
            self.sink.set_status("disconnected: USB support requires pyserial")
            return
        while not self.stop_event.is_set():
            self.sink.set_status("connecting")
            companion = None
            try:
                companion = serial.Serial(self.port, self.baudrate, timeout=1)
                with self._serial_lock:
                    self._serial = companion
                self.sink.set_status("connected")
                buffer = bytearray()
                while not self.stop_event.is_set():
                    chunk = companion.read(4096)
                    if chunk:
                        buffer.extend(chunk)
                        self._process_bytes(buffer)
            except (OSError, RuntimeError, ValueError) as exc:
                if not self.stop_event.is_set():
                    self.sink.set_status(f"disconnected: {exc}")
                    self.stop_event.wait(2)
            finally:
                with self._serial_lock:
                    if self._serial is companion:
                        self._serial = None
                if companion is not None:
                    try:
                        companion.close()
                    except Exception:
                        pass

    def _process_bytes(self, buffer: bytearray) -> None:
        while buffer:
            start = next((index for index, byte in enumerate(buffer) if byte in (0x3C, 0x3E)), None)
            if start is None:
                buffer.clear()
                return
            if start:
                del buffer[:start]
            if len(buffer) < 3:
                return
            length = int.from_bytes(buffer[1:3], "little")
            if length > USB_SERIAL_MAX_PAYLOAD_LENGTH:
                del buffer[0]
                continue
            frame_end = 3 + length
            if len(buffer) < frame_end:
                return
            marker, frame = buffer[0], bytes(buffer[3:frame_end])
            del buffer[:frame_end]
            if marker == 0x3E:
                self.process_frame(frame)


class BleCompanionCapture(BaseCompanionCapture):
    """MeshCore Nordic UART BLE client running on a private asyncio loop."""

    SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
    RX_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
    TX_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

    def __init__(self, sink: CaptureSink) -> None:
        super().__init__(sink)
        self._loop: asyncio.AbstractEventLoop | None = None
        self._thread: threading.Thread | None = None
        self._ready = threading.Event()
        self._protocol_ready = threading.Event()
        self._client = None
        self._rx = None

    def start(self) -> None:
        if self._thread is not None and self._thread.is_alive():
            return
        self._thread = threading.Thread(target=self._run_loop, name="meshcore-ble", daemon=True)
        self._thread.start()
        if not self._ready.wait(2):
            raise RuntimeError("BLE event loop did not start")

    def stop(self) -> None:
        super().stop()
        if self._loop is not None and self._loop.is_running():
            try:
                self._run(self._disconnect(), timeout=5)
            except Exception:
                pass
            self._loop.call_soon_threadsafe(self._loop.stop)
        if self._thread is not None:
            self._thread.join(timeout=2)
        self._loop = None
        self._thread = None
        self.sink.set_status("disconnected")

    def scan_devices(self, timeout: float = 5.0) -> list[dict[str, str]]:
        self.start()
        return self._run(self._scan(timeout), timeout=timeout + 5)

    def connect(self, device_id: str) -> None:
        if not device_id.strip():
            raise ValueError("device_id is required")
        self.start()
        self.sink.set_status("connecting")
        try:
            self._run(self._connect(device_id.strip()), timeout=140)
        except Exception:
            self.sink.set_status("disconnected")
            raise

    def send_raw_packet(self, packet: bytes, *, priority: int = 0, timeout: float = 5.0) -> None:
        command = self._raw_command(packet, priority, MAX_RAW_PACKET_LENGTH)
        with self._send_lock:
            if self._client is None or not self._client.is_connected or self._rx is None:
                raise RuntimeError("companion is not connected")
            self._drain_responses()
            self._run(self._client.write_gatt_char(self._rx, command, response=True), timeout=timeout)
            self._check_response(timeout)

    def _run_loop(self) -> None:
        self._loop = asyncio.new_event_loop()
        asyncio.set_event_loop(self._loop)
        self._ready.set()
        self._loop.run_forever()
        self._loop.close()

    def _run(self, coroutine, *, timeout: float):
        if self._loop is None or not self._loop.is_running():
            raise RuntimeError("BLE event loop is not running")
        return asyncio.run_coroutine_threadsafe(coroutine, self._loop).result(timeout=timeout)

    async def _scan(self, timeout: float) -> list[dict[str, str]]:
        try:
            from bleak import BleakScanner
        except ImportError as exc:
            raise RuntimeError("BLE support requires bleak") from exc
        advertisements = await BleakScanner.discover(
            timeout=timeout, return_adv=True, service_uuids=[self.SERVICE_UUID]
        )
        devices = []
        for device, advert in advertisements.values():
            services = {str(value).lower() for value in (advert.service_uuids or [])}
            if self.SERVICE_UUID not in services:
                continue
            devices.append(
                {
                    "id": str(device.address),
                    "name": advert.local_name or device.name or "MeshCore BLE companion",
                }
            )
        return devices

    async def _connect(self, device_id: str) -> None:
        try:
            from bleak import BleakClient
        except ImportError as exc:
            raise RuntimeError("BLE support requires bleak") from exc
        await self._disconnect()
        client = BleakClient(device_id, disconnected_callback=self._on_disconnected, pair=True, timeout=120)
        tx = None
        try:
            await client.connect()
            service = client.services.get_service(self.SERVICE_UUID)
            if service is None:
                raise RuntimeError("MeshCore BLE service was not found")
            rx = service.get_characteristic(self.RX_UUID)
            tx = service.get_characteristic(self.TX_UUID)
            if rx is None or tx is None:
                raise RuntimeError("MeshCore BLE UART characteristics were not found")
            await client.start_notify(tx, self._notification_handler)
            self._protocol_ready.clear()
            await client.write_gatt_char(rx, bytes([COMPANION_CMD_DEVICE_QUERY, 1]), response=True)
            ready = await asyncio.to_thread(self._protocol_ready.wait, 10)
            if not ready:
                raise RuntimeError("BLE companion did not answer the protocol probe")
        except Exception:
            if tx is not None:
                try:
                    await client.stop_notify(tx)
                except Exception:
                    pass
            try:
                await client.disconnect()
            except Exception:
                pass
            raise
        self._client, self._rx = client, rx
        self.sink.set_status("connected")

    async def _disconnect(self) -> None:
        client, self._client = self._client, None
        self._rx = None
        if client is not None and client.is_connected:
            try:
                await client.stop_notify(self.TX_UUID)
            except Exception:
                pass
            await client.disconnect()

    def _on_disconnected(self, _client) -> None:
        self._client, self._rx = None, None
        self.sink.set_status("disconnected")

    def _notification_handler(self, _characteristic, data: bytearray) -> None:
        frame = bytes(data)
        if frame and frame[0] in (COMPANION_RESP_OK, COMPANION_RESP_ERR, COMPANION_RESP_DEVICE_INFO):
            self._protocol_ready.set()
        self.process_frame(frame)
