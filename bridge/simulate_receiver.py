"""Fake receiver for exercising the Phase 0 pipeline before real firmware exists.

Publishes JSON messages on hab/rx/<receiver_id> in the same shape the real
receiver firmware will produce. Simulates a tracker driving away from a
fixed ground station, with RSSI/SNR degrading and packet loss increasing
with distance, so the range-vs-RSSI and packet-loss panels have real data
to show during the T3 end-to-end soak test.
"""
from __future__ import annotations

import argparse
import json
import math
import random
import time

import paho.mqtt.client as mqtt

from packet import FIX_3D, FLAG_AIRBORNE_MODE, encode_packet


def run(args: argparse.Namespace) -> None:
    client = mqtt.Client(callback_api_version=mqtt.CallbackAPIVersion.VERSION2)
    client.connect(args.mqtt_host, args.mqtt_port, keepalive=60)
    client.loop_start()

    topic = f"hab/rx/{args.receiver_id}"
    seq = 0
    batt_mv = 4150.0
    bearing_rad = math.radians(args.bearing_deg)
    start = time.time()

    print(f"simulating receiver '{args.receiver_id}' -> {topic} every {args.interval}s "
          f"(ctrl-c to stop)")

    try:
        while True:
            elapsed = time.time() - start
            distance_m = args.speed_mps * elapsed
            dlat = (distance_m * math.cos(bearing_rad)) / 111320.0
            dlon = (distance_m * math.sin(bearing_rad)) / (
                111320.0 * math.cos(math.radians(args.receiver_lat))
            )
            tracker_lat = args.receiver_lat + dlat
            tracker_lon = args.receiver_lon + dlon

            batt_mv = max(3300.0, batt_mv - random.uniform(0.0, 0.5))
            temp_c = 20.0 - (distance_m / 100.0) * 0.01 + random.uniform(-0.3, 0.3)

            raw = encode_packet(
                node_id=args.node_id,
                seq=seq,
                gps_time=int(time.time()),
                lat=tracker_lat,
                lon=tracker_lon,
                alt_m=args.altitude_m,
                sats=random.randint(6, 12),
                fix=FIX_3D,
                batt_mv=int(batt_mv),
                temp_c=temp_c,
                flags=FLAG_AIRBORNE_MODE,
            )

            # Rough free-space-path-loss-shaped RSSI/SNR curve, plus noise.
            rssi = -40.0 - 20.0 * math.log10(max(distance_m, 1.0) / 100.0) + random.uniform(-3, 3)
            snr = max(-20.0, 12.0 - distance_m / 800.0 + random.uniform(-1.5, 1.5))
            drop_probability = min(0.9, distance_m / (args.max_range_m * 1.2))

            if random.random() >= drop_probability:
                payload = {
                    "receiver_id": args.receiver_id,
                    "receiver_lat": args.receiver_lat,
                    "receiver_lon": args.receiver_lon,
                    "receiver_alt_m": args.receiver_alt_m,
                    "rx_timestamp": time.time(),
                    "rssi": round(rssi, 1),
                    "snr": round(snr, 1),
                    "freq_error_hz": random.randint(-200, 200),
                    "raw_hex": raw.hex(),
                }
                client.publish(topic, json.dumps(payload), qos=0)
                print(f"seq={seq:5d} dist={distance_m / 1000:6.2f}km rssi={rssi:6.1f} "
                      f"snr={snr:5.1f} batt={batt_mv:.0f}mV")
            else:
                print(f"seq={seq:5d} dist={distance_m / 1000:6.2f}km  -- dropped --")

            seq = (seq + 1) % 65536
            time.sleep(args.interval)
    except KeyboardInterrupt:
        pass
    finally:
        client.loop_stop()
        client.disconnect()


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--mqtt-host", default="localhost")
    p.add_argument("--mqtt-port", type=int, default=1883)
    p.add_argument("--receiver-id", default="gs1")
    p.add_argument("--receiver-lat", type=float, default=52.5200)
    p.add_argument("--receiver-lon", type=float, default=13.4050)
    p.add_argument("--receiver-alt-m", type=float, default=40.0)
    p.add_argument("--node-id", type=int, default=1)
    p.add_argument("--altitude-m", type=float, default=150.0)
    p.add_argument("--bearing-deg", type=float, default=90.0, help="tracker heading, 0=N")
    p.add_argument("--speed-mps", type=float, default=15.0, help="~54 km/h test drive")
    p.add_argument("--max-range-m", type=float, default=15000.0, help="range where drops dominate")
    p.add_argument("--interval", type=float, default=2.3, help="seconds between packets (SF9 duty)")
    return p.parse_args()


if __name__ == "__main__":
    run(parse_args())
