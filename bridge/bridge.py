"""HAB Phase 0 bridge: hab/rx/<receiver_id> (MQTT) -> InfluxDB.

Subscribes to every receiver's topic, decodes the v1 packet, computes
derived link-quality fields (great-circle distance, slant range, elevation
angle, packet loss per receiver) and writes everything to InfluxDB.

Expects each MQTT message to be JSON shaped like simulate_receiver.py
produces (and what the real receiver firmware will produce):

    {
      "receiver_id": "gs1",
      "receiver_lat": 52.52, "receiver_lon": 13.40, "receiver_alt_m": 40.0,
      "rx_timestamp": 1730000000.123,
      "rssi": -82.3, "snr": 6.5, "freq_error_hz": 120,
      "raw_hex": "<58 hex chars, the 29-byte packet>"
    }
"""
from __future__ import annotations

import json
import logging
import os
import time
from dataclasses import dataclass

import paho.mqtt.client as mqtt
from influxdb_client import InfluxDBClient, Point, WritePrecision
from influxdb_client.client.write_api import SYNCHRONOUS

from geo import elevation_angle_deg, haversine_distance_m, slant_range_m
from packet import PacketError, decode_packet

logging.basicConfig(
    level=os.environ.get("BRIDGE_LOG_LEVEL", "INFO"),
    format="%(asctime)s %(levelname)s %(name)s: %(message)s",
)
log = logging.getLogger("bridge")

MQTT_HOST = os.environ.get("MQTT_HOST", "localhost")
MQTT_PORT = int(os.environ.get("MQTT_PORT", "1883"))
MQTT_TOPIC = "hab/rx/#"

INFLUX_URL = os.environ.get("INFLUX_URL", "http://localhost:8086")
INFLUX_TOKEN = os.environ["INFLUX_TOKEN"]
INFLUX_ORG = os.environ["INFLUX_ORG"]
INFLUX_BUCKET = os.environ["INFLUX_BUCKET"]

MEASUREMENT = "telemetry"


@dataclass
class LinkState:
    """Per (node_id, receiver_id) sequence tracking for packet-loss calculation."""

    first_seq: int | None = None
    last_seq: int | None = None
    received: int = 0

    def observe(self, seq: int, rebooted: bool) -> float:
        if rebooted or self.first_seq is None or seq < self.last_seq:
            self.first_seq = seq
            self.last_seq = seq
            self.received = 1
            return 0.0
        self.last_seq = max(self.last_seq, seq)
        self.received += 1
        expected = self.last_seq - self.first_seq + 1
        if expected <= 0:
            return 0.0
        return max(0.0, (expected - self.received) / expected * 100.0)


link_states: dict[tuple[int, str], LinkState] = {}


def handle_message(write_api, receiver_id: str, payload: dict) -> None:
    receiver_lat = payload.get("receiver_lat")
    receiver_lon = payload.get("receiver_lon")
    receiver_alt_m = payload.get("receiver_alt_m", 0.0)
    rx_timestamp = payload.get("rx_timestamp", time.time())
    ts_ns = int(rx_timestamp * 1e9)

    if receiver_lat is not None and receiver_lon is not None:
        rx_point = (
            Point(MEASUREMENT)
            .tag("receiver_id", receiver_id)
            .field("receiver_lat", float(receiver_lat))
            .field("receiver_lon", float(receiver_lon))
            .field("receiver_alt_m", float(receiver_alt_m))
            .time(ts_ns, WritePrecision.NS)
        )
        write_api.write(bucket=INFLUX_BUCKET, org=INFLUX_ORG, record=rx_point)

    raw_hex = payload.get("raw_hex")
    if not raw_hex:
        log.warning("receiver %s: message with no raw_hex, skipping", receiver_id)
        return

    try:
        raw = bytes.fromhex(raw_hex)
        packet = decode_packet(raw)
    except (PacketError, ValueError) as exc:
        log.warning("receiver %s: dropping bad packet (%s)", receiver_id, exc)
        err_point = Point(MEASUREMENT).tag("receiver_id", receiver_id).field(
            "crc_ok", False
        ).time(ts_ns, WritePrecision.NS)
        write_api.write(bucket=INFLUX_BUCKET, org=INFLUX_ORG, record=err_point)
        return

    node_id = packet["node_id"]
    state = link_states.setdefault((node_id, receiver_id), LinkState())
    packet_loss_pct = state.observe(packet["seq"], packet["rebooted"])

    point = (
        Point(MEASUREMENT)
        .tag("node_id", str(node_id))
        .tag("receiver_id", receiver_id)
        .field("crc_ok", True)
        .field("seq", packet["seq"])
        .field("gps_time", packet["gps_time"])
        .field("lat", packet["lat"])
        .field("lon", packet["lon"])
        .field("alt_m", packet["alt_m"])
        .field("sats", packet["sats"])
        .field("fix", packet["fix"])
        .field("batt_mv", packet["batt_mv"])
        .field("temp_c", packet["temp_c"])
        .field("airborne_mode", packet["airborne_mode"])
        .field("rebooted", packet["rebooted"])
        .field("packet_loss_pct", packet_loss_pct)
    )

    for field_name in ("rssi", "snr", "freq_error_hz"):
        if field_name in payload:
            point = point.field(field_name, float(payload[field_name]))

    if receiver_lat is not None and receiver_lon is not None:
        ground_m = haversine_distance_m(receiver_lat, receiver_lon, packet["lat"], packet["lon"])
        alt_diff_m = packet["alt_m"] - float(receiver_alt_m)
        point = (
            point.field("distance_km", ground_m / 1000.0)
            .field("slant_range_km", slant_range_m(ground_m, alt_diff_m) / 1000.0)
            .field("elevation_deg", elevation_angle_deg(ground_m, alt_diff_m))
        )

    point = point.time(ts_ns, WritePrecision.NS)
    write_api.write(bucket=INFLUX_BUCKET, org=INFLUX_ORG, record=point)
    log.info(
        "receiver=%s node=%s seq=%s fix=%s sats=%s batt_mv=%s loss=%.1f%%",
        receiver_id, node_id, packet["seq"], packet["fix"], packet["sats"],
        packet["batt_mv"], packet_loss_pct,
    )


def main() -> None:
    influx_client = InfluxDBClient(url=INFLUX_URL, token=INFLUX_TOKEN, org=INFLUX_ORG)
    write_api = influx_client.write_api(write_options=SYNCHRONOUS)

    def on_connect(client, userdata, flags, rc, properties=None):
        if rc == 0:
            log.info("connected to MQTT broker %s:%s", MQTT_HOST, MQTT_PORT)
            client.subscribe(MQTT_TOPIC)
        else:
            log.error("MQTT connect failed, rc=%s", rc)

    def on_message(client, userdata, msg):
        receiver_id = msg.topic.split("/")[-1]
        try:
            payload = json.loads(msg.payload.decode("utf-8"))
        except (json.JSONDecodeError, UnicodeDecodeError) as exc:
            log.warning("receiver %s: bad JSON payload (%s)", receiver_id, exc)
            return
        try:
            handle_message(write_api, receiver_id, payload)
        except Exception:
            log.exception("receiver %s: error handling message", receiver_id)

    client = mqtt.Client(callback_api_version=mqtt.CallbackAPIVersion.VERSION2)
    client.on_connect = on_connect
    client.on_message = on_message

    while True:
        try:
            client.connect(MQTT_HOST, MQTT_PORT, keepalive=60)
            break
        except OSError as exc:
            log.warning("MQTT broker not ready (%s), retrying in 3s", exc)
            time.sleep(3)

    client.loop_forever()


if __name__ == "__main__":
    main()
