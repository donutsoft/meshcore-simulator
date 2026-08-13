# MeshCore host repeater simulator

This target runs MeshCore's real repeater and companion firmware on macOS. Each SQLite `repeaters` row instantiates `examples/simple_repeater/MyMesh`; each requested TCP endpoint instantiates `examples/companion_radio/MyMesh`. Each `directed_links` observation supplies a bidirectional edge in the simulated radio reachability graph.

The physical LoRa driver is replaced by an in-process `mesh::Radio` adapter. Each firmware instance owns an isolated mailbox; the central medium delivers a transmitted datagram only to connected endpoints. This avoids roughly one thousand kernel sockets and threads while leaving MeshCore's packet, routing, repeater, and companion command logic in control above the radio boundary.

## Build and run

From the cloned `MeshCore` directory:

```bash
make -C simulator test
simulator/build/meshcore-repeater-sim \
  --database ../data/meshcore.sqlite3 \
  --duration-ms 10000 \
  --interference 0.02 \
  --seed 42
```

For an interactive run with two TCP companion radios:

```bash
simulator/build/meshcore-repeater-sim \
  --database ../data/meshcore.sqlite3 \
  --duration-ms 0 \
  --companion 010101@5000 \
  --companion 7A2AC7@5001
```

Supplying a companion endpoint automatically enables real-time pacing. Each endpoint listens on loopback by default and is connected bidirectionally to its selected repeater and every direct neighbor of that repeater. Use `--companion-bind IP` only when the listener intentionally needs to be exposed beyond this Mac.

The TCP adapter writes every decoded companion protocol frame to standard output as a `TCP_FRAME` line containing the endpoint port, direction, payload length, and uppercase hexadecimal payload. TCP marker and length bytes are not included in the payload dump.

Companion flash is persistent. By default, native runs store one filesystem image per
TCP port under `<database>.companions/`; the website uses `data/companion-state/`.
Identity, preferences (including auto-add configuration and hop limit), contacts,
channels, and other files written by the upstream companion firmware survive simulator
restarts. Use `--companion-storage DIR` to select another location.

During interactive runs, repeater sends are emitted as line-delimited `PACKET_SENT` events containing the packet length and a stable packet identifier derived from MeshCore's payload identity. Repeater-to-repeater edges are emitted separately as `ROUTE_START` and `ROUTE_END` events; start events include the calculated LoRa airtime. The dashboard therefore shows each edge only while that transmission is on air and accumulates the bytes sent for each packet identifier. Every radio repeat adds its full packet length, including transmissions with no eligible receiver. Flood transmissions are shown on every compatible neighboring edge but their `PACKET_SENT` event is emitted once per physical transmission; direct-route telemetry includes only the intended next repeater. A transmission is still shown during its on-air interval if it ultimately collides or is dropped by simulated interference.

TCP uses MeshCore companion framing: client commands begin with `<` plus a little-endian 16-bit length, and server responses/pushes begin with `>`. Framing and sockets are host adapters only. Commands and responses—including device configuration, contacts, channels, messages, identity operations, and `PUSH_CODE_LOG_RX_DATA`—are processed by the upstream companion firmware. Each endpoint has an isolated in-memory filesystem and a normally generated Ed25519 identity.

In batch mode, if no `--inject` argument is supplied, one flood advert is injected at the first connected repeater. Explicit, repeatable injections use `ID@MILLISECONDS`:

```bash
simulator/build/meshcore-repeater-sim \
  --database ../data/meshcore.sqlite3 \
  --duration-ms 5000 \
  --inject 8B861B@0 \
  --inject 6800D6@0
```

Collisions are evaluated at each receiver: an overlapping same-channel transmission only blocks receivers which can hear both senders. The medium also models hidden-terminal collisions at a common receiver and half-duplex receive loss while that receiver transmits. Radios must agree on frequency, bandwidth, spreading factor, and header mode; implicit-header radios must also agree on coding rate. `--interference P` is an independent seeded drop probability for each otherwise eligible receiver.

`directed_links` retains the order in which adjacency was observed, but radio reachability is symmetric by default: either endpoint can transmit to the other. `--links directed` is available only as an explicit diagnostic override.

## LoRa airtime

Transmission completion and overlap use the Semtech LoRa time-on-air equation with explicit header and CRC. Defaults match the repeater defaults and MeshCore's adaptive preamble length: 915 MHz, 250 kHz bandwidth, SF10, CR 4/5, and 16 preamble symbols. SF5-SF8 default to 32 preamble symbols as the firmware does.

Configure it with `--frequency`, `--bandwidth`, `--sf`, `--cr`, and `--preamble`. Frequency is retained as radio metadata but does not affect time-on-air; bandwidth, spreading factor, coding rate, preamble, payload length, header mode, and CRC do.

## Identity prefixes and host scope

Repeater identities are generated with MeshCore's bundled Ed25519 implementation, then the first three public-key bytes are replaced with the SQLite `repeater_id`. This provides the observed routing prefix without computing vanity keys. Since the modified public key cannot validate signatures from its generated private key, the host build bypasses advert signature verification only for the exact forced repeater identities registered by the simulator. Companion and unrelated identities use normal Ed25519 validation. AES-128, SHA-256/HMAC, and companion key generation use real cryptographic implementations.

Board power, sensors, serial console output, and flash storage are host adapters. Mesh behavior, repeater behavior, companion commands, packet parsing, encryption, queues, contacts, channels, retransmit timing, and duplicate handling come from upstream MeshCore code.

The host target defines the companion's upstream `MAX_CONTACTS=100` setting for every
translation unit. This is required because `BaseChatMesh` contains the contact array
directly; compiling that class with a different value changes its C++ object layout.
