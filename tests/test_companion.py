from meshcore_simulator.companion import BaseCompanionCapture, UsbCompanionCapture


class Sink:
    def __init__(self):
        self.packets = []
        self.status = ""

    def set_status(self, status):
        self.status = status

    def add_packet(self, packet, snr, rssi):
        self.packets.append((packet, snr, rssi))


def test_companion_rx_frame_reaches_sink():
    sink = Sink()
    capture = BaseCompanionCapture(sink)

    capture.process_frame(bytes([0x88, 4, 0xB0]) + b"radio")

    assert sink.packets == [(b"radio", 1.0, -80)]


def test_usb_stream_reassembles_frames_and_ignores_tx_echo():
    sink = Sink()
    capture = UsbCompanionCapture("/dev/test", sink)
    rx = b">" + (4).to_bytes(2, "little") + bytes([0x88, 4, 0xB0, 0xAA])
    tx = b"<" + (2).to_bytes(2, "little") + b"\x16\x01"
    buffer = bytearray()

    for chunk in (b"noise" + rx[:2], rx[2:5], rx[5:] + tx):
        buffer.extend(chunk)
        capture._process_bytes(buffer)

    assert sink.packets == [(b"\xAA", 1.0, -80)]
