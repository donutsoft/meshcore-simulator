const state = {
  map: null,
  markers: [],
  lines: [],
  routeLines: [],
  routeTimers: [],
  routeSignature: null,
  located: new Map(),
  topology: null,
  boundsSignature: null,
  selections: [null, null],
  activeSlot: 0,
  selectionHydrated: false,
  packetBytesSignature: null,
};
const byId = (id) => document.getElementById(id);

async function requestJson(url, options = {}) {
  const response = await fetch(url, options);
  const body = await response.json();
  if (!response.ok) throw new Error(body.error || `Request failed (${response.status})`);
  return body;
}

function post(url, body = {}) {
  return requestJson(url, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });
}

function message(text = "") { byId("connection-message").textContent = text; }
function simulationMessage(text = "") { byId("simulation-message").textContent = text; }

function renderPacketBytes(bytesSent = {}) {
  const entries = Object.entries(bytesSent)
    .filter(([, count]) => Number.isFinite(Number(count)) && Number(count) >= 0)
    .sort(([left], [right]) => left.localeCompare(right));
  const signature = entries.map(([packet, count]) => `${packet}:${count}`).join("|");
  if (signature === state.packetBytesSignature) return;
  state.packetBytesSignature = signature;
  const list = byId("packet-bytes");
  if (!entries.length) {
    const empty = document.createElement("div");
    empty.className = "packet-bytes-empty";
    empty.textContent = "No packets sent yet.";
    list.replaceChildren(empty);
    return;
  }
  list.replaceChildren(...entries.map(([packet, count]) => {
    const row = document.createElement("div");
    const label = document.createElement("dt");
    const swatch = document.createElement("i");
    swatch.className = "packet-swatch";
    swatch.style.color = packetRouteColor(packet);
    swatch.style.backgroundColor = swatch.style.color;
    const code = document.createElement("code");
    code.textContent = packet;
    label.append(swatch, code);
    const value = document.createElement("dd");
    value.textContent = `${Number(count).toLocaleString()} B`;
    row.append(label, value);
    return row;
  }));
}

function setActiveSlot(slot) {
  state.activeSlot = slot;
  document.querySelectorAll(".companion-slot").forEach((button) => {
    button.classList.toggle("active", Number(button.dataset.slot) === slot);
  });
}

function renderCompanionSelections(simulation = null) {
  for (let slot = 0; slot < 2; slot += 1) {
    const repeaterId = state.selections[slot];
    const node = state.topology?.nodes.find((candidate) => candidate.id === repeaterId);
    byId(`companion-repeater-${slot}`).textContent = repeaterId
      ? `${node?.name ? `${node.name} · ` : ""}${repeaterId}`
      : "Choose repeater";
  }
  const running = simulation?.status === "running";
  renderPacketBytes(simulation?.bytes_sent || {});
  document.body.classList.toggle("simulation-running", running);
  if (running) {
    const dropPercent = (Number(simulation.packet_drop_rate || 0) * 100).toFixed(1).replace(/\.0$/, "");
    byId("packet-drop-rate").value = dropPercent;
    simulationMessage(`Listening on 127.0.0.1:5000 and :5001 · ${dropPercent}% random packet drop`);
  }
  else if (simulation?.last_error) simulationMessage(simulation.last_error);
}

function selectRepeater(node) {
  const otherSlot = state.activeSlot === 0 ? 1 : 0;
  if (state.selections[otherSlot] === node.id) {
    simulationMessage("Choose two different repeaters.");
    return;
  }
  state.selections[state.activeSlot] = node.id;
  simulationMessage(`TCP ${5000 + state.activeSlot} attached to ${node.name || node.id}.`);
  renderCompanionSelections(state.topology?.simulation);
  setActiveSlot(otherSlot);
  if (state.topology) renderMap(state.topology);
}

document.querySelectorAll(".companion-slot").forEach((button) => {
  button.addEventListener("click", () => setActiveSlot(Number(button.dataset.slot)));
});
byId("start-simulation").addEventListener("click", async () => {
  if (state.selections.some((selection) => !selection)) {
    simulationMessage("Select one map repeater for each TCP port.");
    return;
  }
  const packetDropPercent = Number(byId("packet-drop-rate").value);
  if (!Number.isFinite(packetDropPercent) || packetDropPercent < 0 || packetDropPercent > 100) {
    simulationMessage("Packet drop rate must be between 0% and 100%.");
    return;
  }
  simulationMessage("Building and starting the MeshCore simulation…");
  try {
    const simulation = await post("/api/simulation/companions", {
      repeaters: state.selections,
      packet_drop_rate: packetDropPercent / 100,
    });
    if (state.topology) state.topology.simulation = simulation;
    renderCompanionSelections(simulation);
  } catch (error) { simulationMessage(error.message); }
});
byId("stop-simulation").addEventListener("click", async () => {
  try {
    const simulation = await requestJson("/api/simulation/companions", { method: "DELETE" });
    if (state.topology) state.topology.simulation = simulation;
    simulationMessage("Simulation stopped.");
    renderCompanionSelections(simulation);
  } catch (error) { simulationMessage(error.message); }
});

document.querySelectorAll(".tab").forEach((tab) => tab.addEventListener("click", () => {
  document.querySelectorAll(".tab").forEach((item) => item.classList.toggle("active", item === tab));
  document.querySelectorAll(".transport").forEach((form) => form.classList.toggle("active", form.id === `${tab.dataset.tab}-form`));
}));

byId("tcp-form").addEventListener("submit", async (event) => {
  event.preventDefault();
  try {
    await post("/api/companion/tcp/connect", { host: byId("tcp-host").value, port: Number(byId("tcp-port").value) });
    message("TCP connection started.");
  } catch (error) { message(error.message); }
});

async function refreshUsb() {
  try {
    const { ports } = await requestJson("/api/companion/usb/ports");
    byId("usb-port").replaceChildren(...ports.map((port) => new Option(port.name, port.id)));
    message(ports.length ? "" : "No serial ports found.");
  } catch (error) { message(error.message); }
}
byId("refresh-usb").addEventListener("click", refreshUsb);
byId("usb-form").addEventListener("submit", async (event) => {
  event.preventDefault();
  try {
    await post("/api/companion/usb/connect", { port: byId("usb-port").value, baudrate: Number(byId("usb-baud").value) });
    message("USB connection started.");
  } catch (error) { message(error.message); }
});

byId("scan-ble").addEventListener("click", async () => {
  message("Scanning for MeshCore companions…");
  try {
    const { devices } = await post("/api/companion/ble/scan");
    byId("ble-device").replaceChildren(...devices.map((device) => new Option(device.name, device.id)));
    message(devices.length ? "Scan complete." : "No MeshCore BLE companions found.");
  } catch (error) { message(error.message); }
});
byId("connect-ble").addEventListener("click", async () => {
  message("Connecting over Bluetooth…");
  try {
    await post("/api/companion/ble/connect", { device_id: byId("ble-device").value });
    message("Bluetooth connected.");
  } catch (error) { message(error.message); }
});
byId("disconnect").addEventListener("click", async () => {
  try { await post("/api/companion/disconnect"); message("Disconnected."); }
  catch (error) { message(error.message); }
});

function clearMap() {
  state.markers.forEach((marker) => marker.remove());
  state.lines.forEach((line) => line.remove());
  state.routeLines.forEach((line) => line.remove());
  state.routeTimers.forEach((timer) => clearTimeout(timer));
  state.markers = [];
  state.lines = [];
  state.routeLines = [];
  state.routeTimers = [];
  state.routeSignature = null;
}

function packetRouteColor(packet) {
  let hash = 2166136261;
  for (const character of String(packet)) {
    hash ^= character.charCodeAt(0);
    hash = Math.imul(hash, 16777619);
  }
  return `hsl(${(hash >>> 0) % 360} 88% 60%)`;
}

function renderRouteHighlights(routes = []) {
  if (!state.map || !window.L) return;
  const visible = routes.filter((route) => state.located.has(route.source) && state.located.has(route.target));
  const signature = visible
    .map((route) => `${route.transmission}:${route.packet}:${route.source}>${route.target}:${route.bytes_sent || 0}`)
    .sort()
    .join("|");
  if (signature === state.routeSignature) return;
  state.routeLines.forEach((line) => line.remove());
  state.routeTimers.forEach((timer) => clearTimeout(timer));
  state.routeLines = [];
  state.routeTimers = [];
  state.routeSignature = signature;
  for (const route of visible) {
    const source = state.located.get(route.source), target = state.located.get(route.target);
    const color = packetRouteColor(route.packet);
    const line = L.polyline(
      [[source.latitude, source.longitude], [target.latitude, target.longitude]],
      { color, opacity: .95, weight: 8, className: "route-highlight" },
    ).addTo(state.map);
    line.getElement()?.style.setProperty("--route-color", color);
    line.bindTooltip(
      `${route.source} → ${route.target} · packet ${route.packet} · ${Number(route.bytes_sent || 0).toLocaleString()} B sent`,
      { sticky: true },
    );
    state.routeLines.push(line);
    state.routeTimers.push(setTimeout(() => line.remove(), Math.max(0, Number(route.remaining_ms) || 0)));
  }
}

function layoutCoincidentNodes(nodes) {
  const groups = new Map();
  for (const node of nodes) {
    if (node.latitude === null || node.longitude === null) continue;
    const key = `${node.latitude.toFixed(6)}:${node.longitude.toFixed(6)}`;
    if (!groups.has(key)) groups.set(key, []);
    groups.get(key).push(node);
  }

  const displayed = new Map();
  for (const group of groups.values()) {
    const ordered = [...group].sort((left, right) => {
      const leftAdvert = left.position_source === "advert" ? 0 : 1;
      const rightAdvert = right.position_source === "advert" ? 0 : 1;
      return leftAdvert - rightAdvert || left.id.localeCompare(right.id);
    });
    const center = ordered[0];
    displayed.set(center.id, { ...center, visually_offset: false });
    for (let index = 1; index < ordered.length; index += 1) {
      const node = ordered[index];
      // A one-neighbor geographic midpoint is identical to that neighbor.
      // Spread coincident estimates on a deterministic golden-angle spiral
      // solely for display, keeping the actual inferred coordinate in the API.
      const angle = index * 2.399963229728653;
      const radiusMeters = 140 * Math.sqrt(index);
      const latitudeScale = 111320;
      const longitudeScale = Math.max(111320 * Math.cos(center.latitude * Math.PI / 180), 1);
      displayed.set(node.id, {
        ...node,
        latitude: center.latitude + Math.sin(angle) * radiusMeters / latitudeScale,
        longitude: center.longitude + Math.cos(angle) * radiusMeters / longitudeScale,
        visually_offset: true,
      });
    }
  }
  return displayed;
}

function renderMap(topology) {
  if (!state.map || !window.L) return renderNodeList(topology);
  byId("node-list").style.display = "none";
  clearMap();
  const located = layoutCoincidentNodes(topology.nodes);
  state.located = located;
  const bounds = L.latLngBounds([]);
  for (const edge of topology.edges) {
    const source = located.get(edge.source), target = located.get(edge.target);
    if (!source || !target) continue;
    state.lines.push(L.polyline(
      [[source.latitude, source.longitude], [target.latitude, target.longitude]],
      {
        color: "#72a5ff",
        opacity: Math.min(.35 + Math.log2(edge.observation_count + 1) * .12, .9),
        weight: Math.min(1 + Math.log2(edge.observation_count + 1), 6),
        dashArray: source.visually_offset || target.visually_offset ? "5 4" : null,
      },
    ).addTo(state.map));
  }
  for (const node of located.values()) {
    const selectedSlot = state.selections.indexOf(node.id);
    const selectedColor = selectedSlot === 0 ? "#ff72c6" : "#72d8ff";
    const marker = L.circleMarker([node.latitude, node.longitude], {
      radius: selectedSlot >= 0 ? 10 : (node.position_source === "advert" ? 7 : 6),
      fillColor: selectedSlot >= 0 ? selectedColor : (node.position_source === "advert" ? "#77e6bd" : "#ffc66d"),
      fillOpacity: 1,
      color: selectedSlot >= 0 ? "#ffffff" : "#07100f",
      weight: selectedSlot >= 0 ? 3 : 2,
      title: `${node.name || node.id} · ${node.position_source}`,
    }).addTo(state.map);
    const offsetNote = node.visually_offset ? "<br><small>Spread from coincident midpoint for visibility</small>" : "";
    const selectedNote = selectedSlot >= 0 ? `<br><strong>TCP ${5000 + selectedSlot}</strong>` : "<br><small>Click to attach the active companion slot</small>";
    marker.bindPopup(`<strong>${escapeHtml(node.name || "Unnamed repeater")}</strong><br><code>${node.id}</code><br>${node.neighbor_count} neighbors · ${node.position_source}${offsetNote}${selectedNote}`);
    marker.on("click", () => selectRepeater(node));
    state.markers.push(marker);
    bounds.extend([node.latitude, node.longitude]);
  }
  if (located.size) {
    byId("map-empty").style.display = "none";
    const boundsSignature = [...located.values()]
      .map((node) => `${node.id}:${node.latitude}:${node.longitude}`)
      .sort()
      .join("|");
    if (boundsSignature !== state.boundsSignature) {
      state.map.fitBounds(bounds, { padding: [60, 60], maxZoom: 15 });
      state.boundsSignature = boundsSignature;
    }
  } else {
    state.boundsSignature = null;
    byId("map-empty").style.display = "grid";
    byId("map-message").textContent = topology.nodes.length ? "Routes captured; waiting for a GPS advert to anchor inferred positions." : "Connect a companion to begin capturing.";
  }
  renderRouteHighlights(topology.simulation?.active_routes || []);
}

function renderNodeList(topology) {
  const list = byId("node-list");
  list.style.display = "block";
  list.replaceChildren(...topology.nodes.map((node) => {
    const card = document.createElement("div");
    card.className = "node-card";
    card.innerHTML = `<strong>${escapeHtml(node.name || node.id)}</strong><small>${node.id} · ${node.neighbor_count} neighbors · ${node.position_source}</small>`;
    card.addEventListener("click", () => selectRepeater(node));
    return card;
  }));
}

function escapeHtml(value) {
  const span = document.createElement("span");
  span.textContent = value;
  return span.innerHTML;
}

async function refreshTopology() {
  try {
    const topology = await requestJson("/api/topology");
    state.topology = topology;
    if (!state.selectionHydrated) {
      state.selections = topology.simulation.repeaters || [null, null];
      state.selectionHydrated = true;
    } else if (topology.simulation.status === "running") {
      state.selections = topology.simulation.repeaters;
    }
    for (const key of ["repeaters", "links", "packets", "adverts"]) byId(key).textContent = topology.stats[key];
    byId("ignored").textContent = topology.capture.ignored;
    byId("status").textContent = topology.capture.transport ? `${topology.capture.transport} · ${topology.capture.status}` : topology.capture.status;
    byId("status-dot").classList.toggle("connected", topology.capture.status === "connected");
    renderCompanionSelections(topology.simulation);
    renderMap(topology);
  } catch (error) { message(error.message); }
}

let simulationRefreshPending = false;
async function refreshSimulation() {
  if (simulationRefreshPending) return;
  simulationRefreshPending = true;
  try {
    const simulation = await requestJson("/api/simulation/companions");
    if (state.topology) state.topology.simulation = simulation;
    if (simulation.status === "running") state.selections = simulation.repeaters;
    renderCompanionSelections(simulation);
    renderRouteHighlights(simulation.active_routes || []);
  } catch (error) { simulationMessage(error.message); }
  finally { simulationRefreshPending = false; }
}

function initializeMap() {
  if (!window.L) {
    byId("map-message").textContent = "Leaflet could not be loaded. Topology data is listed here in the meantime.";
    return;
  }
  state.map = L.map("map", { zoomControl: true }).setView([0, 0], 2);
  L.tileLayer(window.MESHCORE_MAP_CONFIG.tileUrl, {
    maxZoom: 19,
    attribution: window.MESHCORE_MAP_CONFIG.attribution,
  }).addTo(state.map);
}

initializeMap();
refreshUsb();
refreshTopology();
setInterval(refreshTopology, 3000);
setInterval(refreshSimulation, 50);
