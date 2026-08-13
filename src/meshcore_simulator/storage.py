"""SQLite persistence and graph inference for captured MeshCore routes."""

from __future__ import annotations

import json
import math
import sqlite3
import threading
from pathlib import Path

from .protocol import packet_route, packet_type, parse_repeater_advert, utc_now

SCHEMA = """
CREATE TABLE IF NOT EXISTS repeaters (
    repeater_id TEXT PRIMARY KEY CHECK(length(repeater_id) = 6),
    public_key TEXT UNIQUE,
    name TEXT,
    latitude REAL,
    longitude REAL,
    first_seen TEXT NOT NULL,
    last_seen TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS packets (
    id INTEGER PRIMARY KEY,
    received_at TEXT NOT NULL,
    packet_type TEXT NOT NULL,
    route_type TEXT NOT NULL,
    path_hash_size INTEGER NOT NULL CHECK(path_hash_size = 3),
    route_json TEXT NOT NULL,
    raw BLOB NOT NULL,
    snr REAL NOT NULL,
    rssi INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS packet_hops (
    packet_id INTEGER NOT NULL REFERENCES packets(id) ON DELETE CASCADE,
    ordinal INTEGER NOT NULL,
    repeater_id TEXT NOT NULL REFERENCES repeaters(repeater_id),
    PRIMARY KEY(packet_id, ordinal)
);

CREATE TABLE IF NOT EXISTS advert_observations (
    id INTEGER PRIMARY KEY,
    packet_id INTEGER REFERENCES packets(id) ON DELETE SET NULL,
    received_at TEXT NOT NULL,
    advert_timestamp INTEGER NOT NULL,
    repeater_id TEXT NOT NULL REFERENCES repeaters(repeater_id),
    public_key TEXT NOT NULL,
    name TEXT,
    latitude REAL,
    longitude REAL,
    raw BLOB NOT NULL
);

CREATE TABLE IF NOT EXISTS directed_links (
    source_id TEXT NOT NULL REFERENCES repeaters(repeater_id),
    target_id TEXT NOT NULL REFERENCES repeaters(repeater_id),
    first_seen TEXT NOT NULL,
    last_seen TEXT NOT NULL,
    observation_count INTEGER NOT NULL DEFAULT 1,
    PRIMARY KEY(source_id, target_id),
    CHECK(source_id <> target_id)
);

CREATE INDEX IF NOT EXISTS idx_packets_received_at ON packets(received_at);
CREATE INDEX IF NOT EXISTS idx_adverts_repeater ON advert_observations(repeater_id, received_at);
CREATE INDEX IF NOT EXISTS idx_links_target ON directed_links(target_id);
CREATE UNIQUE INDEX IF NOT EXISTS idx_adverts_observation_identity
    ON advert_observations(received_at, public_key, raw);
"""


class TopologyStore:
    """Thread-safe store. Each operation gets a short-lived SQLite connection."""

    def __init__(self, path: Path) -> None:
        self.path = Path(path)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self._lock = threading.RLock()
        with self._connect() as database:
            database.executescript(SCHEMA)
            # Some firmware advertises 0,0 when GPS is unset. Preserve the
            # advert itself, but never treat Null Island as a location anchor.
            database.execute(
                "UPDATE repeaters SET latitude = NULL, longitude = NULL WHERE latitude = 0 AND longitude = 0"
            )
            database.execute(
                "UPDATE advert_observations SET latitude = NULL, longitude = NULL WHERE latitude = 0 AND longitude = 0"
            )

    def _connect(self) -> sqlite3.Connection:
        database = sqlite3.connect(self.path, timeout=10)
        database.row_factory = sqlite3.Row
        database.execute("PRAGMA foreign_keys = ON")
        database.execute("PRAGMA journal_mode = WAL")
        return database

    def has_repeaters(self, repeater_ids: list[str] | tuple[str, ...]) -> bool:
        """Return whether every requested repeater exists in SQLite."""
        unique_ids = set(repeater_ids)
        if not unique_ids:
            return False
        placeholders = ",".join("?" for _ in unique_ids)
        with self._lock, self._connect() as database:
            count = database.execute(
                f"SELECT count(*) FROM repeaters WHERE repeater_id IN ({placeholders})",
                tuple(sorted(unique_ids)),
            ).fetchone()[0]
        return int(count) == len(unique_ids)

    @staticmethod
    def _remember_repeater(database: sqlite3.Connection, repeater_id: str, seen_at: str) -> None:
        database.execute(
            """
            INSERT INTO repeaters(repeater_id, first_seen, last_seen)
            VALUES (?, ?, ?)
            ON CONFLICT(repeater_id) DO UPDATE SET
                first_seen = min(repeaters.first_seen, excluded.first_seen),
                last_seen = max(repeaters.last_seen, excluded.last_seen)
            """,
            (repeater_id, seen_at, seen_at),
        )

    def ingest_packet(self, raw: bytes, snr: float, rssi: int, received_at: str | None = None) -> dict[str, object]:
        """Persist one packet and derive route links.

        Repeater adverts are always retained. General packet/route logging is
        deliberately restricted to three-byte path hashes.
        """
        raw = bytes(raw)
        received_at = received_at or utc_now()
        route = packet_route(raw)
        advert = parse_repeater_advert(raw)
        logged_packet = route is not None and route.path_hash_size == 3
        packet_id: int | None = None

        with self._lock, self._connect() as database:
            if logged_packet:
                cursor = database.execute(
                    """
                    INSERT INTO packets(received_at, packet_type, route_type, path_hash_size,
                                        route_json, raw, snr, rssi)
                    VALUES (?, ?, ?, 3, ?, ?, ?, ?)
                    """,
                    (
                        received_at,
                        packet_type(raw),
                        route.route_type,
                        json.dumps(route.repeater_ids, separators=(",", ":")),
                        raw,
                        float(snr),
                        int(rssi),
                    ),
                )
                packet_id = int(cursor.lastrowid)
                for ordinal, repeater_id in enumerate(route.repeater_ids):
                    self._remember_repeater(database, repeater_id, received_at)
                    database.execute(
                        "INSERT INTO packet_hops(packet_id, ordinal, repeater_id) VALUES (?, ?, ?)",
                        (packet_id, ordinal, repeater_id),
                    )

            if advert is not None:
                self._remember_repeater(database, advert.repeater_id, received_at)
                database.execute(
                    """
                    UPDATE repeaters
                    SET public_key = ?,
                        name = COALESCE(?, name),
                        latitude = COALESCE(?, latitude),
                        longitude = COALESCE(?, longitude),
                        last_seen = ?
                    WHERE repeater_id = ?
                    """,
                    (
                        advert.public_key,
                        advert.name,
                        advert.latitude,
                        advert.longitude,
                        received_at,
                        advert.repeater_id,
                    ),
                )
                database.execute(
                    """
                    INSERT INTO advert_observations(
                        packet_id, received_at, advert_timestamp, repeater_id,
                        public_key, name, latitude, longitude, raw
                    ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
                    """,
                    (
                        packet_id,
                        received_at,
                        advert.timestamp,
                        advert.repeater_id,
                        advert.public_key,
                        advert.name,
                        advert.latitude,
                        advert.longitude,
                        raw,
                    ),
                )

            if logged_packet:
                chain = list(route.repeater_ids)
                if advert is not None and (not chain or chain[0] != advert.repeater_id):
                    chain.insert(0, advert.repeater_id)
                self._observe_chain(database, chain, received_at)

        return {
            "logged": logged_packet,
            "packet_id": packet_id,
            "advert_logged": advert is not None,
            "ignored_reason": None if logged_packet else "route does not use three-byte repeater IDs",
        }

    def import_adverts(self, path: Path) -> dict[str, int]:
        """Import meshcore-security JSONL adverts without duplicating observations."""
        path = Path(path)
        stats = {
            "records": 0,
            "repeater_records": 0,
            "observations_imported": 0,
            "duplicates": 0,
            "non_repeaters": 0,
            "invalid": 0,
            "prefix_collisions": 0,
            "repeaters_imported": 0,
        }
        imported_repeaters: set[str] = set()
        with self._lock, self._connect() as database, path.open("r", encoding="utf-8") as source:
            for line in source:
                stats["records"] += 1
                try:
                    record = json.loads(line)
                except json.JSONDecodeError:
                    stats["invalid"] += 1
                    continue
                if not isinstance(record, dict):
                    stats["invalid"] += 1
                    continue
                if record.get("node_type") != 2:
                    stats["non_repeaters"] += 1
                    continue
                stats["repeater_records"] += 1
                try:
                    public_key = bytes.fromhex(str(record["pubkey"])).hex().upper()
                    if len(public_key) != 64:
                        raise ValueError("public key must be 32 bytes")
                    repeater_id = public_key[:6]
                    received_at = str(record["received_at"])
                    if not received_at:
                        raise ValueError("received_at is required")
                    advert_timestamp = int(record["advert_timestamp"])
                    if isinstance(record["advert_timestamp"], bool):
                        raise ValueError("advert timestamp cannot be boolean")
                    raw = bytes.fromhex(str(record["raw_hex"] or ""))
                    if not raw:
                        raise ValueError("raw packet is required")
                    name = record.get("name")
                    if name is not None and not isinstance(name, str):
                        raise ValueError("name must be a string")
                    latitude, longitude = record.get("latitude"), record.get("longitude")
                    if (latitude is None) != (longitude is None):
                        raise ValueError("coordinates must be a pair")
                    if latitude is not None:
                        if isinstance(latitude, bool) or isinstance(longitude, bool):
                            raise ValueError("coordinates cannot be boolean")
                        latitude, longitude = float(latitude), float(longitude)
                        if not (-90 <= latitude <= 90 and -180 <= longitude <= 180):
                            raise ValueError("coordinates are out of range")
                        if latitude == 0 and longitude == 0:
                            latitude = longitude = None
                except (KeyError, TypeError, ValueError):
                    stats["invalid"] += 1
                    continue

                existing = database.execute(
                    "SELECT public_key FROM repeaters WHERE repeater_id = ?", (repeater_id,)
                ).fetchone()
                if existing is not None and existing["public_key"] not in (None, public_key):
                    stats["prefix_collisions"] += 1
                    continue

                database.execute(
                    """
                    INSERT INTO repeaters(
                        repeater_id, public_key, name, latitude, longitude, first_seen, last_seen
                    ) VALUES (?, ?, ?, ?, ?, ?, ?)
                    ON CONFLICT(repeater_id) DO UPDATE SET
                        public_key = COALESCE(repeaters.public_key, excluded.public_key),
                        name = CASE
                            WHEN repeaters.name IS NULL OR excluded.last_seen >= repeaters.last_seen
                            THEN COALESCE(excluded.name, repeaters.name) ELSE repeaters.name END,
                        latitude = CASE
                            WHEN repeaters.latitude IS NULL OR excluded.last_seen >= repeaters.last_seen
                            THEN COALESCE(excluded.latitude, repeaters.latitude) ELSE repeaters.latitude END,
                        longitude = CASE
                            WHEN repeaters.longitude IS NULL OR excluded.last_seen >= repeaters.last_seen
                            THEN COALESCE(excluded.longitude, repeaters.longitude) ELSE repeaters.longitude END,
                        first_seen = min(repeaters.first_seen, excluded.first_seen),
                        last_seen = max(repeaters.last_seen, excluded.last_seen)
                    """,
                    (repeater_id, public_key, name, latitude, longitude, received_at, received_at),
                )
                cursor = database.execute(
                    """
                    INSERT OR IGNORE INTO advert_observations(
                        packet_id, received_at, advert_timestamp, repeater_id,
                        public_key, name, latitude, longitude, raw
                    ) VALUES (NULL, ?, ?, ?, ?, ?, ?, ?, ?)
                    """,
                    (
                        received_at,
                        advert_timestamp,
                        repeater_id,
                        public_key,
                        name,
                        latitude,
                        longitude,
                        raw,
                    ),
                )
                if cursor.rowcount:
                    stats["observations_imported"] += 1
                    imported_repeaters.add(repeater_id)
                else:
                    stats["duplicates"] += 1
        stats["repeaters_imported"] = len(imported_repeaters)
        return stats

    def _observe_chain(self, database: sqlite3.Connection, chain: list[str], seen_at: str) -> None:
        for source_id, target_id in zip(chain, chain[1:]):
            if source_id == target_id:
                continue
            self._remember_repeater(database, source_id, seen_at)
            self._remember_repeater(database, target_id, seen_at)
            database.execute(
                """
                INSERT INTO directed_links(source_id, target_id, first_seen, last_seen, observation_count)
                VALUES (?, ?, ?, ?, 1)
                ON CONFLICT(source_id, target_id) DO UPDATE SET
                    last_seen = excluded.last_seen,
                    observation_count = directed_links.observation_count + 1
                """,
                (source_id, target_id, seen_at, seen_at),
            )

    def topology(self) -> dict[str, object]:
        with self._lock, self._connect() as database:
            repeater_rows = database.execute("SELECT * FROM repeaters ORDER BY repeater_id").fetchall()
            directed_rows = database.execute(
                "SELECT * FROM directed_links ORDER BY source_id, target_id"
            ).fetchall()
            counts = database.execute(
                "SELECT (SELECT count(*) FROM packets), (SELECT count(*) FROM advert_observations)"
            ).fetchone()

        all_repeaters = {str(row["repeater_id"]): dict(row) for row in repeater_rows}
        linked_ids = {
            str(row[column])
            for row in directed_rows
            for column in ("source_id", "target_id")
        }
        # Isolated identities remain in SQLite and automatically appear once
        # a captured route creates their first link.
        repeaters = {
            repeater_id: repeater
            for repeater_id, repeater in all_repeaters.items()
            if repeater_id in linked_ids
        }
        neighbors: dict[str, set[str]] = {repeater_id: set() for repeater_id in repeaters}
        edge_data: dict[tuple[str, str], dict[str, object]] = {}
        for row in directed_rows:
            source, target = str(row["source_id"]), str(row["target_id"])
            neighbors.setdefault(source, set()).add(target)
            neighbors.setdefault(target, set()).add(source)
            pair = tuple(sorted((source, target)))
            edge = edge_data.setdefault(
                pair,
                {
                    "source": pair[0],
                    "target": pair[1],
                    "observation_count": 0,
                    "directions": [],
                    "last_seen": "",
                },
            )
            edge["observation_count"] = int(edge["observation_count"]) + int(row["observation_count"])
            edge["directions"].append(
                {
                    "source": source,
                    "target": target,
                    "observation_count": int(row["observation_count"]),
                }
            )
            edge["last_seen"] = max(str(edge["last_seen"]), str(row["last_seen"]))

        positions, inferred_from = self._infer_positions(repeaters, neighbors)
        nodes = []
        for repeater_id, repeater in repeaters.items():
            position = positions.get(repeater_id)
            nodes.append(
                {
                    "id": repeater_id,
                    "public_key": repeater["public_key"],
                    "name": repeater["name"],
                    "latitude": position[0] if position else None,
                    "longitude": position[1] if position else None,
                    "position_source": "advert" if _has_usable_coordinates(repeater) else ("midpoint" if position else "unknown"),
                    "inferred_from": inferred_from.get(repeater_id, []),
                    "neighbor_count": len(neighbors.get(repeater_id, set())),
                    "first_seen": repeater["first_seen"],
                    "last_seen": repeater["last_seen"],
                }
            )
        return {
            "nodes": nodes,
            "edges": list(edge_data.values()),
            "stats": {
                "repeaters": len(nodes),
                "links": len(edge_data),
                "packets": int(counts[0]),
                "adverts": int(counts[1]),
            },
        }

    @staticmethod
    def _infer_positions(
        repeaters: dict[str, dict[str, object]], neighbors: dict[str, set[str]]
    ) -> tuple[dict[str, tuple[float, float]], dict[str, list[str]]]:
        positions = {
            repeater_id: (float(row["latitude"]), float(row["longitude"]))
            for repeater_id, row in repeaters.items()
            if _has_usable_coordinates(row)
        }
        inferred_from: dict[str, list[str]] = {}
        pending = set(repeaters) - set(positions)
        for _ in range(len(repeaters)):
            updates: dict[str, tuple[float, float]] = {}
            for repeater_id in pending:
                located = sorted(node for node in neighbors.get(repeater_id, set()) if node in positions)
                if located:
                    updates[repeater_id] = _geographic_midpoint([positions[node] for node in located])
                    inferred_from[repeater_id] = located
            if not updates:
                break
            positions.update(updates)
            pending -= updates.keys()
        return positions, inferred_from


def _geographic_midpoint(points: list[tuple[float, float]]) -> tuple[float, float]:
    """Return the spherical centroid, avoiding longitude wraparound errors."""
    x = y = z = 0.0
    for latitude, longitude in points:
        lat, lon = math.radians(latitude), math.radians(longitude)
        x += math.cos(lat) * math.cos(lon)
        y += math.cos(lat) * math.sin(lon)
        z += math.sin(lat)
    count = len(points)
    x, y, z = x / count, y / count, z / count
    longitude = math.atan2(y, x)
    latitude = math.atan2(z, math.sqrt(x * x + y * y))
    return math.degrees(latitude), math.degrees(longitude)


def _has_usable_coordinates(row: dict[str, object]) -> bool:
    latitude, longitude = row["latitude"], row["longitude"]
    return (
        latitude is not None
        and longitude is not None
        and not (float(latitude) == 0 and float(longitude) == 0)
    )
