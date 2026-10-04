"""Forward the receiver's USB-serial JSON lines to the local MQTT broker.

Use with firmware built with HAB_USE_WIFI 0. Works on any network because the
broker is on this laptop (localhost) and the receiver is on USB.

    pip install pyserial paho-mqtt
    python tools/serial_to_mqtt.py            # auto-detects the port
    python tools/serial_to_mqtt.py --port /dev/cu.usbmodem1101
"""
from __future__ import annotations

import argparse
import glob
import json
import time

import paho.mqtt.client as mqtt
import serial


def find_port() -> str | None:
    ports = sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/cu.usbserial*")
                   + glob.glob("/dev/ttyACM*") + glob.glob("/dev/ttyUSB*"))
    return ports[0] if ports else None


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--mqtt-host", default="localhost")
    ap.add_argument("--mqtt-port", type=int, default=1883)
    args = ap.parse_args()

    client = mqtt.Client(callback_api_version=mqtt.CallbackAPIVersion.VERSION2)
    client.connect(args.mqtt_host, args.mqtt_port, keepalive=60)
    client.loop_start()

    while True:  # reopen the port if the board is unplugged/reset
        port = args.port or find_port()
        if not port:
            print("no serial port found, retrying...")
            time.sleep(2)
            continue
        try:
            with serial.Serial(port, args.baud, timeout=1) as ser:
                print(f"reading {port}")
                while True:
                    line = ser.readline().decode("utf-8", errors="replace").strip()
                    if not line.startswith("{"):
                        if line:
                            print(line)
                        continue
                    try:
                        msg = json.loads(line)
                    except json.JSONDecodeError:
                        continue
                    msg.setdefault("rx_timestamp", time.time())
                    topic = f"hab/rx/{msg.get('receiver_id', 'gs1')}"
                    client.publish(topic, json.dumps(msg))
                    print(f"-> {topic} rssi={msg.get('rssi')}")
        except (serial.SerialException, OSError) as e:
            print(f"serial error: {e}; retrying...")
            time.sleep(2)


if __name__ == "__main__":
    main()
