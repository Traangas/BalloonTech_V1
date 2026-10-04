# HAB Phase 0 — ground telemetry pipeline

Implements the Phase 0 data pipeline from the project spec:

```
Tracker --LoRa--> Receiver --WiFi/MQTT--> Mosquitto --> Bridge --> InfluxDB --> Grafana
```

> **Start with [`PROJECT_GUIDE.md`](PROJECT_GUIDE.md)**: current status, how to flash the base
> station and balloon boards separately, the Grafana dashboard, and all learnings.

Real firmware for both boards now lives in [`firmware/`](firmware/). Without hardware,
[`bridge/simulate_receiver.py`](bridge/simulate_receiver.py) still stands in for a receiver: it
encodes v1 packets and publishes them to `hab/rx/<receiver_id>` exactly like the real firmware,
so the rest of the chain (bridge, InfluxDB, Grafana) can be tested on its own.

## Quickstart

```bash
cp .env.example .env   # set real values, especially the Influx/Grafana passwords
docker compose up -d --build
```

This brings up:
- **Mosquitto** on `localhost:1883`
- **InfluxDB 2.x** on `localhost:8086` (org/bucket/token from `.env`)
- **Grafana** on `localhost:3000` (login from `.env`), pre-provisioned with
  the "HAB Phase 0" dashboard and an InfluxDB (Flux) datasource
- **bridge**: subscribes to `hab/rx/#`, decodes packets, writes to InfluxDB

Then, from a second terminal, simulate a receiver feeding it data:

```bash
cd bridge
python3 -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt
python simulate_receiver.py
```

Open Grafana at http://localhost:3000 — the dashboard should start filling
in within a few seconds (altitude, RSSI/SNR, distance/slant range growing
as the simulated tracker "drives away").

## Packet format v1

See [`bridge/packet.py`](bridge/packet.py) for the authoritative encode/decode
(29-byte little-endian struct, CRC-16/CCITT-FALSE over bytes 0–26). This is
the same module the real tracker/receiver firmware's packet layer should
match byte-for-byte.

## Bridge behavior

[`bridge/bridge.py`](bridge/bridge.py) expects each MQTT message to be JSON:

```json
{
  "receiver_id": "gs1",
  "receiver_lat": 52.52, "receiver_lon": 13.405, "receiver_alt_m": 40.0,
  "rx_timestamp": 1730000000.123,
  "rssi": -82.3, "snr": 6.5, "freq_error_hz": 120,
  "raw_hex": "<58 hex chars — the 29-byte packet>"
}
```

This is what `simulate_receiver.py` publishes, and what real receiver
firmware should publish once it exists. The bridge:
- validates length/version/CRC, drops and logs anything invalid
- computes great-circle distance, slant range, and elevation angle from
  receiver to tracker (`bridge/geo.py`)
- tracks per-`(node_id, receiver_id)` sequence numbers to compute packet
  loss %, resetting the baseline when the tracker reports a reboot
- writes everything to the `telemetry` measurement in InfluxDB

## Dashboard

[`grafana/dashboards/hab-phase0.json`](grafana/dashboards/hab-phase0.json)
implements the 7 panels from the spec: geomap (tracker track + receiver),
altitude over time, RSSI/SNR over time, RSSI vs slant range (scatter), SNR
vs slant range (scatter — split into its own panel because this Grafana
build's `xychart` plugin only supports one Y-series per panel), packet loss
% per receiver, battery voltage/internal temperature, and a status table.

The geomap and both scatter panels assume a single ground station (`gs1`);
with more than one receiver, add a `receiver_id` filter/group to those
queries or duplicate the panels per receiver.

## Verification

This was built without Docker available in the build environment, so it was
verified in layers: the packet codec against a known CRC-16/CCITT-FALSE test
vector, the bridge's decode/geo/packet-loss logic against synthetic
payloads, and a full local smoke test (`simulate_receiver.py` → a real
Mosquitto broker → `bridge.py` → a stub InfluxDB HTTP server).

Once Docker was available, the full stack was brought up for real and
checked end to end: `docker compose up -d --build`, confirmed all four
containers healthy, confirmed the Grafana→InfluxDB datasource authenticates
and its env-var substitution resolves correctly, ran `simulate_receiver.py`
against the live stack, and visually confirmed all 7 dashboard panels render
with real data and zero panel errors. Along the way this caught and fixed
three real bugs that only show up against a live Grafana instance:
- two Flux queries called `last()` after `pivot()`, which left no `_value`
  column to reduce (fixed by reordering)
- Grafana bakes every remaining tag/metadata column into a field's display
  name (e.g. `rssi {node_id="1", receiver_id="gs1"}`), which broke the
  scatter panels' field matchers — fixed by dropping `_start`, `_stop`,
  `_measurement`, `node_id`, and `receiver_id` before pivoting
- the battery panel's unit was `volt` while the field holds millivolts,
  showing "4.14 kV" instead of ~4.1 V — fixed to `mvolt`
- a panel JSON missing `pluginVersion` made Grafana run migration logic
  meant for an older schema against already-current panel options, crashing
  the panel — fixed by pinning `pluginVersion: "11.2.0"` on hand-authored
  panels

## Everything still to do for Phase 0 exit criteria

This repo covers the pipeline and working tracker/receiver firmware. Still open: the T1–T9
hardware test plan, the regulatory inquiry, and the deferred firmware items listed in
[`PROJECT_GUIDE.md`](PROJECT_GUIDE.md) section 11.
