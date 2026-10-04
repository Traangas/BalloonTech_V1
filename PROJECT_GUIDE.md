# HAB Project Guide — Phase 0 (ground telemetry chain)

**Read this first if you are picking this project up again, human or Claude Code session.**
It records what exists, how to flash each board on its own, how the Grafana dashboard
works, and everything non-obvious we learned the hard way. Last updated 2026-10-04 (added USB/offline field mode, section 7.1).

- [1. Status at a glance](#1-status-at-a-glance)
- [2. Architecture and repo layout](#2-architecture-and-repo-layout)
- [3. Cold start on a new machine](#3-cold-start-on-a-new-machine)
- [4. The shared contract (packet, radio, MQTT JSON)](#4-the-shared-contract-packet-radio-mqtt-json)
- [5. Heltec WiFi LoRa 32 V4 hardware notes](#5-heltec-wifi-lora-32-v4-hardware-notes)
- [6. Flashing playbook (either board, independently)](#6-flashing-playbook-either-board-independently)
- [7. Base station (receiver) firmware](#7-base-station-receiver-firmware)
- [8. Balloon (tracker) firmware: GPS + LoRa](#8-balloon-tracker-firmware-gps--lora)
- [9. Grafana dashboard and data pipeline](#9-grafana-dashboard-and-data-pipeline)
- [10. Master list of learnings and gotchas](#10-master-list-of-learnings-and-gotchas)
- [11. Open work, known gaps, flight-prep notes](#11-open-work-known-gaps-flight-prep-notes)
- [12. Notes for a future Claude Code session](#12-notes-for-a-future-claude-code-session)
- [13. Session log: 2026-10-04 (V3 base station, new V4 tracker, antenna path, battery)](#13-session-log-2026-10-04-v3-base-station-new-v4-tracker-antenna-path-battery)
- [14. Session log: 2026-10-04/05 (offline field mode, data-feed banner, walk test)](#14-session-log-2026-10-0405-offline-field-mode-data-feed-banner-walk-test)

---

## 1. Status at a glance

Phase 0 goal (from the project spec): prove the whole ground telemetry chain before any
balloon flies. Spec phases: 1 party-balloon tracker, 2 ground station upgrade, 3 latex
stratospheric flight, 5/6 superpressure float. We are finishing **Phase 0**, heading for Phase 1.

| Piece | State |
|---|---|
| Docker stack (Mosquitto, InfluxDB 2, Grafana 11.2, Python bridge) | Done, verified end to end with real hardware |
| Grafana dashboard, 7 panels | Done, shows live data |
| Packet codec (Python + C++, byte-identical, unit-tested natively) | Done |
| Base station firmware (LoRa RX -> WiFi/MQTT **or USB serial**, OLED, LED) | Done, flashed, verified. **Runs on a Heltec V3** (env `heltec_wifi_lora_32_V3`), RX boosted gain on (13). Flashed in USB mode (`HAB_USE_WIFI 0`) for field use, see 7.1 |
| Offline field operation (no WiFi/internet): USB forwarder + local map tile cache | Done; forwarder verified with hardware, tile cache needs warming online (7.1) |
| Balloon firmware (GNSS + LoRa TX, OLED, LED) | Done, flashed, verified on a second, flatter **V4.3** board; front-end (PA/LNA) now driven, TX power configurable, **real battery voltage** (see 8.7). **Temperature is still a placeholder** (8.5); battery accuracy not yet checked against a multimeter |
| Real GPS fix with real coordinates in Grafana | Verified outdoors / at a window. No fix indoors (normal) |
| Power-saving, watchdog, NVS seq/reboot flag, duty-cycle limiter, replay buffer | **Not built** (deferred, see 11) |
| Flight hardware (battery connector, balloon, enclosure) | In progress, see 11.3 |

The repo is a git repository (remote `Traangas/BalloonTech_V1`); `.gitignore` excludes secrets,
`.venv`, `.pio`, and the receiver's `config.h`. Today's work is on branch
`claude/heltec-firmware-flashing-a6e51d`, open as PR #1 (see 13). Note that a git worktree does
**not** contain the gitignored `config.h`: copy it from the main checkout before building the receiver.

---

## 2. Architecture and repo layout

```
Balloon ESP32 --LoRa 869.525 MHz--> Base-station ESP32 --WiFi/MQTT--> Mosquitto
        (GNSS + SX1262)                (SX1262 RX)                       |
                                                                    Python bridge
                                                                         |
                                                              InfluxDB 2 (Flux) --> Grafana
```

```
.
├── PROJECT_GUIDE.md            <- this file
├── README.md                   <- original pipeline readme (pipeline details still accurate)
├── docker-compose.yml          <- mosquitto, influxdb, grafana, bridge, tilecache
├── tilecache/nginx.conf        <- OSM tile caching proxy (offline map)
├── tools/serial_to_mqtt.py     <- USB serial -> MQTT forwarder (field mode, 7.1)
├── .env / .env.example         <- compose secrets (.env is gitignored)
├── mosquitto/config/mosquitto.conf   <- anonymous, port 1883 (bench only!)
├── bridge/
│   ├── bridge.py               <- MQTT hab/rx/# -> decode -> InfluxDB "telemetry"
│   ├── packet.py               <- authoritative packet codec (Python)
│   ├── geo.py                  <- haversine, slant range, elevation
│   └── simulate_receiver.py    <- fake receiver for testing without hardware
├── grafana/
│   ├── provisioning/datasources/influxdb.yml
│   ├── provisioning/dashboards/dashboard.yml
│   └── dashboards/hab-phase0.json     <- THE dashboard (source of truth)
└── firmware/                   <- three independent PlatformIO projects
    ├── common/                 <- shared by both boards via lib_extra_dirs = ../common
    │   ├── hab_packet/         <- C++ packet codec (mirror of packet.py)
    │   └── hab_board/          <- board_pins_v4.h, radio_config.h
    ├── packet_test/            <- native Unity tests (no board needed)
    ├── receiver/               <- BASE STATION firmware (+ include/config.h, gitignored)
    └── tracker/                <- BALLOON firmware
```

`receiver/` and `tracker/` are deliberately **separate PlatformIO projects** so either board
can be built and flashed alone.

---

## 3. Cold start on a new machine

Needs: Docker Desktop, Python 3, PlatformIO CLI, a USB cable that carries data.

```bash
# 1. Pipeline
cp .env.example .env            # change the passwords/token
docker compose up -d --build
docker compose ps               # 4 containers Up
# Grafana: http://localhost:3000  (login from .env) -> dashboard "HAB Phase 0 - Ground Telemetry"
# Direct link: http://localhost:3000/d/hab-phase0/hab-phase-0-ground-telemetry

# 2. PlatformIO (macOS)
pipx install platformio
pipx ensurepath                 # or: export PATH="$HOME/Library/Application Support/pipx/venvs/platformio/bin:$PATH"

# 3. Sanity-check the codec without any board
cd firmware/packet_test && pio test -e native

# 4. Pipeline smoke test without hardware
cd bridge && python3 -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt && python simulate_receiver.py
```

Then flash the boards (section 6). The base station must be able to reach the machine running
Docker on the LAN (port 1883), so the **Docker host's LAN IP goes into the receiver's
`config.h`** (`ipconfig getifaddr en0` on macOS). That IP is usually DHCP: if it changes, the
base station stops publishing until you edit `config.h` and reflash.

---

## 4. The shared contract (packet, radio, MQTT JSON)

Three things must stay in sync across tracker firmware, receiver firmware and the bridge.

### 4.1 Packet v1 — 29 bytes, little-endian, packed

| Off | Field | Type | Notes |
|---|---|---|---|
| 0 | version | u8 | currently 1 |
| 1 | node_id | u8 | tracker hard-codes `1` |
| 2 | seq | u16 | increments per TX, **resets on reboot** (no NVS yet) |
| 4 | gps_time | u32 | unix seconds, 0 if no GPS time |
| 8 | lat | i32 | degrees x 1e7 |
| 12 | lon | i32 | degrees x 1e7 |
| 16 | alt | i32 | decimetres above MSL |
| 20 | sats | u8 | |
| 21 | fix | u8 | 0 none, 2 = 2D, 3 = 3D |
| 22 | batt_mv | u16 | **placeholder 4100 in current tracker firmware** |
| 24 | temp | i16 | degC x 100, **placeholder 21.0 currently** |
| 26 | flags | u8 | bit0 airborne mode confirmed, bit1 rebooted |
| 27 | crc16 | u16 | CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) over bytes 0..26 |

Authoritative sources: `bridge/packet.py` and `firmware/common/hab_packet/hab_packet.{h,cpp}`.
They were verified byte-identical. **Change both together**, then run `pio test -e native` and
the Python round trip.

### 4.2 LoRa parameters (`firmware/common/hab_board/radio_config.h`)

869.525 MHz, BW 125 kHz, CR 4/5, preamble 8, explicit header, CRC on, **private sync word 0x12**
(0x34 is reserved for LoRaWAN). Spreading factor comes from the build flag `-DHAB_SF=9` in
**both** `platformio.ini` files. **Tracker and receiver SF must match.** SF9 packet airtime
is about 226-233 ms (calculated 226, earlier measured 233). Tracker TX power is `HAB_TX_POWER_DBM`
in `radio_config.h`, currently **17 dBm into the front-end** (see 5.2 and 11.2).

### 4.3 MQTT message (receiver -> bridge), topic `hab/rx/<receiver_id>`

```json
{"receiver_id":"gs1","receiver_lat":52.52,"receiver_lon":13.405,"receiver_alt_m":40,
 "rx_timestamp":1790528163.0,"rssi":-56,"snr":8.25,"freq_error_hz":-79.4,
 "raw_hex":"<58 hex chars = the 29 raw bytes>"}
```

The receiver relays the raw packet and the bridge re-validates it (length, version, CRC).
`rx_timestamp` comes from SNTP on the receiver, so the receiver needs internet at boot.

---

## 5. Heltec WiFi LoRa 32 V4 hardware notes

ESP32-S3 + SX1262, native USB, 0.96" OLED, onboard LiPo charger, GNSS connector.
Source of truth: Heltec's official datasheet (Rev 1.4, Sept 2025, downloadable from
`resource.heltec.cn/download/WiFi_LoRa_32_V4/datasheet/`). It supersedes third-party guides.
All pins live in `firmware/common/hab_board/board_pins_v4.h`.

| Function | GPIO | Notes |
|---|---|---|
| LoRa NSS/SCK/MOSI/MISO | 8 / 9 / 10 / 11 | |
| LoRa DIO1 / RST / BUSY | 14 / 12 / 13 | |
| OLED SDA / SCL / RST | 17 / 18 / 21 | I2C addr 0x3C, SSD1315 driven with the SSD1306 library |
| **Vext_Ctrl** | **36** | **Drive LOW to power the OLED rail** (see below) |
| User LED | 35 | |
| Battery ADC | 1 | divider 100k/390k, scale **4.9x**; needs **ADC_CTRL (GPIO37) HIGH** while reading |
| GNSS TX / RX (ESP side) | 39 (ESP RX) / 38 (ESP TX) | 9600 baud |
| **VGNSS_Ctrl** | **34** | **Drive LOW to power the GNSS module** (active low) |
| GNSS RST / PPS / WAKEUP | 42 / 41 / 40 | unused so far |

Other facts:
- **Battery socket is JST-SH1.25, 2-pin** (1.25 mm pitch). Common "2P-PH" LiPo packs are 2.0 mm
  JST-PH and will not fit. Do not feed a raw LiPo into the 5V/GND header: that bypasses the
  charger/protection circuit and the battery-sense divider.
- **PlatformIO has no V4 board entry**. Both projects use `board = heltec_wifi_lora_32_V3`
  (pin compatible for what we use).
- A **V4** enumerates as `/dev/cu.usbmodemXXXX` (native USB, VID:PID 303A:1001). A **V3**
  enumerates as `/dev/cu.usbserial-XXXX` (CP2102 UART bridge, VID:PID 10C4:EA60). That is the
  quickest way to tell which board is plugged in.

### 5.1 V3 vs V4 differences that matter here
Same ESP32-S3 and SX1262, and the LoRa, OLED, Vext_Ctrl, LED and battery-ADC pins used by this project
are identical, so the V4 pin header works unchanged on a V3 (checked by running it, see 13).
- **USB/serial**: V3 goes through a CP2102 to UART0, so it must NOT set `-DARDUINO_USB_CDC_ON_BOOT=1`
  (Serial would bind to the unused native USB and the monitor would be silent). V4 must set it.
  The receiver therefore has two envs (6.2, 6.6).
- **Flash**: V3 has 8 MB, V4 16 MB. Irrelevant for this firmware.
- **RF front-end**: V4 has an external PA/LNA (5.2); V3 does not. Receive-only firmware does not care.
- **Band**: the V3 is sold as 863-870 or 902-928 MHz, or a wideband 863-928 part. The team's V3
  sticker reads 863~928 MHz, fine for 869.525 MHz. Check the sticker before using a V3, a 915-only part
  would have poor sensitivity at 869 MHz.

### 5.2 V4 hardware revisions and the LoRa front-end (PA/LNA)
V4 boards carry an external front-end between the SX1262 and the antenna, and the part and control
pin differ by revision. Read the revision off the PCB silkscreen.

| Revision | Front-end | GPIO7 (VFEM) | GPIO2 (CSD) | TX path pin |
|---|---|---|---|---|
| V4.2 | GC1109 | high (power) | high (enable) | GPIO46 (CPS), high during TX |
| V4.3 / R8 | KCT8103L | high (power) | high (enable) | GPIO5 (CTX), high during TX |

`board_pins_v4.h` selects the pin from `-DHAB_FEM_KCT8103L` or `-DHAB_FEM_GC1109` (set in
`tracker/platformio.ini`; currently KCT8103L for the flatter second board). The tracker powers
and enables the front-end once at startup and raises the TX pin only around `radio.transmit()`.
**Source caveat**: these roles and levels come from Meshtastic's `LoRaFEMInterface`, because
Heltec's datasheet PDF has no extractable pin table (and their community post was unreachable). The
~60 dB RSSI jump (13) supports them, but conducted output power has **not** been measured, and the
V4.2/GC1109 path has not been run on hardware.

---

## 6. Flashing playbook (either board, independently)

Each board is flashed from its own project directory. Nothing needs to be flashed in pairs.

### 6.1 Identify which port is which board

```bash
ls /dev/cu.usb*               # V4 = usbmodemXXXX, V3 = usbserial-XXXX; note the list
# unplug one board, run again: the entry that vanished is that board
```

Port names change when you change USB port or after entering bootloader mode.
Only one board needs to be attached to flash it, which also avoids any mix-up.

### 6.2 Flash a new BASE STATION

```bash
cd firmware/receiver
cp include/config.h.example include/config.h   # first time only; config.h is gitignored
# edit config.h: WiFi SSID/password, MQTT host (Docker machine's LAN IP),
#                receiver id (e.g. "gs1"), receiver lat/lon/alt (used for slant range)
pio run -t upload --upload-port /dev/cu.usbmodemXXXX
```

Env choice: `-e heltec_wifi_lora_32_V3` for a V3 base station, the default V4 env otherwise, e.g.
`pio run -e heltec_wifi_lora_32_V3 -t upload --upload-port /dev/cu.usbserial-0001`.

A second base station: use a different `HAB_RECEIVER_ID` (e.g. "gs2"). The dashboard's
geomap and scatter panels assume one receiver, see 9.6.

### 6.3 Flash a new BALLOON tracker

```bash
cd firmware/tracker                 # no config.h needed
pio run -t upload --upload-port /dev/cu.usbmodemXXXX
```

`node_id` is hard-coded to `1` in `tracker/src/main.cpp` (`f.node_id = 1`). If you ever fly two
trackers, give each a different value before flashing (or make it a build flag).
Make sure `-DHAB_SF=` matches the receiver's. Check that `-DHAB_FEM_KCT8103L` / `-DHAB_FEM_GC1109` in
`tracker/platformio.ini` matches the board revision (5.2); the wrong one would drive the wrong pin.

### 6.4 If upload fails ("No serial data received" / port not found)

Enter the ROM bootloader manually: **hold BOOT, tap RESET, release BOOT**, then re-run
`ls /dev/cu.usbmodem*` (the port name usually changes) and upload to the new name. esptool's
automatic reset is unreliable on this board. Press RESET again after flashing if it does not
start by itself.

### 6.5 Reading serial output

`monitor_dtr = 0` / `monitor_rts = 0` are set in both `platformio.ini` files, but the method
verified to work every time is pyserial with the control lines cleared **before** opening
(opening with DTR/RTS asserted resets the board or drops it into the bootloader):

```bash
PY="$HOME/Library/Application Support/pipx/venvs/platformio/bin/python3"
"$PY" - <<'EOF'
import serial, time
s = serial.Serial()
s.port = '/dev/cu.usbmodemXXXX'; s.baudrate = 115200; s.timeout = 1
s.dtr = False; s.rts = False      # must be set before open()
s.open()
end = time.time() + 15
while time.time() < end:
    d = s.read(1024)
    if d: print(d.decode(errors='replace'), end='')
s.close()
EOF
```

### 6.6 Required `platformio.ini` flag (do not remove)

`-DARDUINO_USB_CDC_ON_BOOT=1` **on V4 envs only** (the V3 env deliberately omits it, 5.1). The V3 board manifest sets `ARDUINO_USB_MODE=1` but not this one;
without it `Serial` binds to a disconnected UART and the board looks dead even though it runs.

---

## 7. Base station (receiver) firmware

`firmware/receiver/src/main.cpp`. Continuous LoRa receive, validate, publish.

**Behaviour**
1. Boot: LED/OLED init (Vext_Ctrl low first), WiFi, SNTP time sync, MQTT connect, radio init, RX.
2. On every LoRa packet: LED on (about 80 ms flash), read RSSI/SNR/frequency error, decode with
   `hab::decodePacket` (length/version/CRC). Valid packets are published as the JSON in 4.3 and
   echoed to serial; invalid ones just bump a counter.
3. OLED refreshes every second and on each packet.

**OLED lines**: `HAB BASE <id>`, `WiFi:OK MQTT:OK`, `IP:<addr>`, `LoRa: LINKED | NO SIGNAL`
(linked = a valid packet in the last 15 s, `kLinkTimeoutMs`), `RSSI / SNR`, `Last: HH:MM:SS`
(**UTC**, from SNTP), `OK:<valid> BAD:<invalid>`.

**Healthy serial output** (from real hardware): `WiFi connected, IP=...`, `MQTT connected`,
`listening: 869.525 MHz, SF9, BW125kHz, CR4/5`, then a JSON line plus
`valid=N invalid=0 node=1 seq=...` per packet. Typical bench link: RSSI -54 to -59 dBm, SNR 8 to 10 dB.

**RX boosted gain** is on (`radio.setRxBoostedGainMode(true)` in `setupRadio()`): about +3 dB
sensitivity for slightly more receive current, fine on USB power. Its benefit has not been measured, because
the boards have only been close together where the receiver is near saturation; it needs a distance test.
The `17` in the receiver's `radio.begin()` is an unused required argument: the receiver never transmits.

**Not built yet**: LittleFS buffer-and-replay when WiFi/MQTT drops (packets are lost during an
outage), WiFi reconnect logic beyond MQTT retry, authenticated MQTT.

### 7.1 Field / offline mode (USB serial, no WiFi, no internet)

A WiFi receiver can only reach the broker on the network it was configured for, and it blocks at
boot until it joins. For field use the receiver stays plugged into the laptop and does not use WiFi:

```
Tracker --LoRa--> Receiver --USB serial--> tools/serial_to_mqtt.py --> Mosquitto (localhost) --> ... --> Grafana (localhost:3000)
```

- **Firmware**: `#define HAB_USE_WIFI 0` in `firmware/receiver/include/config.h` (default when the
  flag is absent is `1`, the old WiFi/MQTT behaviour). USB mode skips WiFi, SNTP and MQTT, keeps
  printing the same JSON line per packet to serial, and omits `rx_timestamp` (no clock on the
  board). The OLED shows `USB serial mode`.
- **Laptop**: run the forwarder (auto-detects `/dev/cu.usbserial*` / `usbmodem*`, reconnects on
  unplug, adds `rx_timestamp` from the laptop clock, publishes to `localhost:1883`):
  ```bash
  pip install pyserial paho-mqtt
  python tools/serial_to_mqtt.py [--port /dev/cu.usbserial-0001]
  ```
  Only one program may hold the serial port: close any serial monitor first. The Docker stack
  must be running; Grafana is `http://localhost:3000`, no network needed.
- **Map tiles**: the geomap basemap is now `xyz` pointing at `http://localhost:8080/{z}/{x}/{y}.png`,
  served by the `tilecache` container (nginx caching proxy to OpenStreetMap, 1 year / 2 GB cache in the
  `tile-cache` volume). Tiles are only cached once they have been viewed **while online**, so before
  going out, open the dashboard map over the launch and expected landing area and zoom/pan through the
  zoom levels you will want. Offline, uncached areas show blank tiles; the track and markers still draw.
- **Before leaving**: `docker compose up -d --build` once while online (pulls images), make sure Docker
  Desktop starts on its own, and stop the laptop sleeping (a sleeping laptop stops the forwarder).
- **Board variant matters**: the current base station is a Heltec **V3** (CP2102 UART bridge, shows up as
  `/dev/cu.usbserial-0001`). Build and flash it with `pio run -e heltec_wifi_lora_32_V3 -t upload
  --upload-port /dev/cu.usbserial-0001`. The default V4 env sets `ARDUINO_USB_CDC_ON_BOOT=1`, which on a V3
  sends `Serial` to a disconnected native-USB port: the board runs but prints nothing, so the forwarder
  sees no data (verified). A V4 board uses the V4 env and shows up as `usbmodem`.
- The receiver in this repo's current flashed state is USB mode on `/dev/cu.usbserial-0001`. To go back to WiFi/MQTT, set `HAB_USE_WIFI 1`
  and reflash.

**Troubleshooting**
- Hangs at "connecting to WiFi": wrong credentials, or a 5 GHz-only network (ESP32 is 2.4 GHz only).
- Hangs at "waiting for SNTP": no internet on that network.
- "MQTT connect failed, rc=-2": Docker host IP wrong or firewall blocks 1883.
- Receives nothing: SF/frequency mismatch with the tracker, tracker not powered or antenna missing.
- Display blank but LED works: Vext_Ctrl not driven low (see 5, 10).

---

## 8. Balloon (tracker) firmware: GPS + LoRa

`firmware/tracker/src/main.cpp`. Reads the GNSS module, builds a packet, transmits every ~2 s
(`delay(2000)`), forever.

### 8.1 Boot sequence
LED/OLED init (Vext_Ctrl low) -> GNSS power on (GPIO34 **low**) -> wait up to 3 s for NMEA ->
send the CASIC "flight mode" command and wait for its ACK -> radio init.

### 8.2 The GNSS module is CASIC AT6558R, not Quectel (important)
The spec assumed Quectel L76K and its `$PMTK886,3` Balloon Mode. The module's own boot banner
(`$GPTXT,...MA=CASIC`, `IC=AT6558R-5N-32...`) says otherwise, and PMTK commands are silently
ignored. The equivalent high-altitude setting is the binary **CSIP** command from the CASIC
protocol spec (V4.2.0.3, section 2.11.8):

- Packet: `0xBA 0xCE`, u16 LE payload length, class, id, payload, u32 LE checksum
  (`checksum = (id<<24)+(class<<16)+len`, plus every 4-byte LE payload word, wrapping u32).
- **CFG-NAVX** class `0x06` id `0x07`, 44-byte payload, mask bit0 set, `dyModel = 5`
  ("flight mode, acceleration < 1 g").
- Module replies ACK-ACK (class 0x05, id 0x01); firmware logs
  `GNSS dynamic model (flight <1g) CONFIRMED` and sets packet flag bit0.
- NMEA stays text on the same UART; binary frames never collide with printable ASCII.

Open question still unverified: whether this chipset keeps reporting above ~18 km. It has the
flight dynamic model, but the real limit needs a test or the AT6558R documentation before Phase 3.

### 8.3 What the tracker transmits
lat/lon/alt/sats/gps_time from TinyGPSPlus; fix is 3D if altitude is valid, 2D if only location,
else 0. When there is no fix the packet carries `lat=0, lon=0, fix=0`, which is why the
dashboard map pins the tracker at 0N 0E (see 9.6).

### 8.4 OLED and LED
OLED: `HAB TRACKER #<node>`, `Alt / Sat`, `GPS: LOCK | NO FIX`, `LoRa TX: OK | FAIL`,
`Batt: <pct>% <volts>`, `Seq`. LED flashes about 80 ms per transmit.

Deliberate limitation: **the tracker cannot show "connected to base station" or RSSI/SNR**.
The link is one-way broadcast; the tracker never hears the receiver. `LoRa TX: OK` only means the
radio accepted the transmit. A real "connected" indicator needs a downlink ACK from the base
station (costs airtime and protocol work). That was the user's original wish and is open (see 11.1).

### 8.5 Placeholders to replace before trusting the numbers
- `f.temp_c = 21.0` is still hard-coded. Use the ESP32-S3 internal sensor or an external sensor.
- (Battery is no longer a placeholder, see 8.7. Its absolute accuracy is still unchecked.)

### 8.6 Troubleshooting
- Tracker transmits but the receiver sees a very weak signal (about -74 dBm at 2 dBm TX on a bench):
  the V4 front-end was not powered or enabled (5.2). With it driven the same setup read about -14 dBm.
- **No fix**: needs open sky or at least a window, cold start takes about 30 s to a few minutes,
  never fixes indoors. Check `gps_chars` rising and `gps_fail` staying low in the TX log line.
- `GNSS module NOT responding`: module unpowered. GPIO34 must be driven LOW.
- `gps_fail` (NMEA checksum failures) climbing fast: UART RX buffer overflowing. The code sets
  `gnssSerial.setRxBufferSize(2048)` before `begin()`. Keep that.
- `$GPTXT,...ANTENNA OPEN` is printed on some boots (seen again on the second V4 even though it got a
  fix with 7-8 satellites a few minutes later). During bring-up it appeared while the antenna
  was fine (satellite GSV data proved it) because the real fault was the buffer overflow above.
  Trust satellite counts, not that line alone.
- Tracker LED flashes but nothing reaches the base station: SF/frequency mismatch, no LoRa antenna
  attached (**never transmit without an antenna**, it can damage the PA).

### 8.7 Battery voltage and percentage
`readBatteryMv()` in `tracker/src/main.cpp`: set `HAB_BATT_ADC_CTRL_PIN` (GPIO37) HIGH to connect the
divider, wait 10 ms, discard 2 reads, take 16 `analogReadMilliVolts()` samples on GPIO1 (6 dB attenuation,
factory-calibrated), sort and average the middle half (rejects spikes), multiply by 4.9
(`HAB_BATT_ADC_SCALE`), then set GPIO37 low. `setupBattery()` runs 6 warm-up reads at boot.
`battPercent()` interpolates a typical LiPo resting-voltage table (3.30 V = 0%, 3.84 V = 50%,
4.20 V = 100%); approximate, not calibrated to the actual pack. `batt_mv` also appears in the serial TX line.

What was observed (real pack, the second V4):
- Battery alone: about 3830-3840 mV, OLED 48%. Stable to a few mV.
- USB connected: reading steps up about 135 mV at once and then creeps up (4013 mV at 77% after a few
  minutes, 4043 mV later), while the board's red charge LED is lit. **While USB charges, the reading and
  percentage overstate the cell**; read battery state only when running on battery.
- A cold power-up on battery sent one packet (seq 0) reading 4234 mV, then 3837. Cause not isolated; the
  warm-up reads were added to target it but the fix has **not** been re-verified on a true cold battery boot
  (the test after flashing was a USB reset).
- Not done: comparison against a multimeter. 3.84 V on a part-charged pack is plausible and the OLED
  percentage matches the curve, but both come from the same ADC number, so absolute accuracy is
  unconfirmed. If a multimeter disagrees by more than about 30 mV, add a correction factor.

---

## 9. Grafana dashboard and data pipeline

### 9.1 Pieces
| Component | Role |
|---|---|
| Mosquitto | MQTT broker (anonymous, bench only) |
| `bridge/bridge.py` | subscribes `hab/rx/#`, validates, derives link metrics, writes to InfluxDB |
| InfluxDB 2.7 | one measurement `telemetry` in the bucket from `.env` (default `telemetry`, org `hab`) |
| Grafana 11.2.0 | pinned version, datasource + dashboard fully provisioned from files |

### 9.2 InfluxDB schema (written by the bridge)
Measurement `telemetry`. Tags: `node_id` (string) and `receiver_id`.
Tracker fields: `seq, gps_time, lat, lon, alt_m, sats, fix, batt_mv, temp_c, airborne_mode,
rebooted, packet_loss_pct, crc_ok`. Link fields (when the receiver supplies them): `rssi, snr,
freq_error_hz`. Derived fields (when receiver position known): `distance_km, slant_range_km,
elevation_deg`. Receiver position rows (`receiver_lat/lon/alt_m`) are written with only the
`receiver_id` tag. Bad packets write only `crc_ok=false`. Packet loss is computed per
`(node_id, receiver_id)` from sequence gaps and the baseline resets when the `rebooted` flag is set
or seq goes backwards.

### 9.3 Provisioning (why there is no manual setup)
- `grafana/provisioning/datasources/influxdb.yml`: Flux datasource `uid: influxdb-hab`; org, bucket and token
  come from environment variables that `docker-compose.yml` passes to the Grafana container.
- `grafana/provisioning/dashboards/dashboard.yml`: file provider reading
  `/var/lib/grafana/dashboards`, rescans every 30 s, `allowUiUpdates: true`.
- `grafana/dashboards/hab-phase0.json`: uid `hab-phase0`, refresh 10 s, default range last 1 h,
  a textbox variable `bucket` (default `telemetry`) used as `${bucket}` in every query.

### 9.3b How the dashboard was created
1. Panels and Flux queries were **generated with a Python script** from the spec's 7 requirements
   (the script is not kept in the repo), then **hand-patched** against a live Grafana after
   visual checks. The JSON file is now the single source of truth; edit it directly.
2. Workflow that worked: edit JSON -> wait up to 30 s (or restart the grafana container) -> reload
   the browser -> look at every panel; use the panel "Inspect > Query" in Grafana to debug Flux.
3. Edits made in the Grafana UI can be exported with *Share > Export > JSON* and pasted back
   into the file; otherwise provisioning will overwrite them on the next rescan.

### 9.4 Panels
| # | Type | Title | What it shows |
|---|---|---|---|
| 1 | geomap | Tracker track + receivers | red markers = tracker positions, blue = receiver(s); basemap `osm-standard` |
| 2 | timeseries | Altitude over time | `alt_m` per node |
| 3 | timeseries | RSSI and SNR over time | per receiver |
| 4 | xychart | RSSI vs slant range | scatter, link budget curve |
| 8 | xychart | SNR vs slant range | scatter (split from 4, see 9.5) |
| 5 | timeseries | Packet loss % per receiver | |
| 6 | timeseries | Battery voltage and internal temperature | battery unit is **mV** (`mvolt`) |
| 7 | table | Status: last packet, fix, sats, flags, reboots | last values in the last hour; `last_seen` shows relative time |
| 9 | gauge | Signal strength | latest RSSI mapped -130 dBm = 0% to -50 dBm = 100%, only packets from the last 15 s, else "No link" |
| 10 | gauge | Tracker battery | battery % with voltage |
| 11 | gauge | Noise | noise gauge |
| 12 | stat | **Data feed** (full-width banner on top) | seconds since the last packet reached InfluxDB: green `LIVE` under 30 s, red above that, `NO DATA - check forwarder / receiver / tracker` after 10 min of silence. Added so a dead USB forwarder (7.1) is obvious |

Quick read of the status table: `fix 0 / sats 0` = no GPS lock; `last_seen: a few seconds ago` =
the LoRa/WiFi/MQTT chain is alive; `airborne_mode: true` = the CASIC flight mode ACK was received.

### 9.5 Bugs found against a real Grafana and how they were fixed
These only appear on a live instance, so keep them in mind when editing queries:
1. **`pivot()` then `last()` leaves no `_value`**: do `group |> last() |> pivot` (panels 1B and 7).
2. **Tag columns get baked into field display names** (e.g. `rssi {node_id="1",...}`), which breaks
   `byName` field matchers in the scatter panels: `drop(columns: ["_start","_stop","node_id","receiver_id","_measurement"])`
   before pivoting.
3. **Battery unit**: field is millivolts, unit must be `mvolt` (was showing "4.14 kV").
4. **Hand-written panels need `"pluginVersion": "11.2.0"`**. Without it Grafana runs migrations
   written for older schemas against current options and the panel crashes on load (xychart).
5. **Geomap showed "API KEY REQUIRED"**: the default basemap needs a key; use `osm-standard`.
   Since 2026-10-05 the basemap is `xyz` pointing at the local `tilecache` proxy (7.1) so the map works offline.
6. **xychart in this Grafana build supports one Y series**: RSSI and SNR scatters are two panels.
7. **Datasource YAML env substitution**: values must be `${VAR}` and the variable must be present in
   the Grafana container's environment (see `docker-compose.yml`).
8. Grafana 11.2.0 is pinned on purpose. Upgrading may change xychart/geomap options, re-test all 7 panels.

### 9.6 Known dashboard gaps
- Packets with `fix=0` still write `lat=0, lon=0`, so the map shows a dot off West Africa and
  the slant-range panels show about 6000 km. Fix: filter `fix > 0` in the geomap/slant queries, or
  have the bridge skip `lat/lon/distance` fields when `fix == 0`.
- Geomap and scatter panels assume a single receiver (`gs1`); add a `receiver_id` filter/variable for more.
- Receiver coordinates come from the base station's `config.h` (default is a Berlin placeholder).
  Set the real coordinates or slant range/elevation are wrong.

### 9.7 Handy commands
```bash
docker compose ps
docker compose logs -f bridge             # shows "receiver=gs1 node=1 seq=.. fix=.. sats=.."
docker compose restart grafana
docker compose down                        # keeps data volumes; add -v to wipe InfluxDB/Grafana data
```
Do not edit `.env` credentials after first boot and expect InfluxDB to change: the init variables
only apply to an empty volume.

---

## 10. Master list of learnings and gotchas

Hardware/firmware
1. **OLED power**: Vext_Ctrl (GPIO36) must be driven LOW before `Wire.begin()`. Symptom when missing:
   `oled.begin()` still returns true, the screen is dark, and an I2C scan reports *all 126 addresses*
   (unpowered OLED pulls SDA low). With it LOW the scan shows exactly 0x3C.
2. **GNSS power**: VGNSS_Ctrl (GPIO34) is active LOW. Driving it HIGH leaves the module dead (no NMEA at all).
3. **USB CDC**: needs `-DARDUINO_USB_CDC_ON_BOOT=1` (6.6).
4. **Serial capture**: clear DTR/RTS before open (6.5).
5. **Flashing**: BOOT+RESET manual bootloader entry; port name changes (6.4).
6. **GNSS chip identity**: read the module's boot banner before assuming a vendor protocol (8.2).
7. **GNSS UART buffer**: 2048-byte RX buffer, otherwise checksum failures and a false "no fix / antenna" scare.
8. **Battery ADC**: divider means x4.9 and GPIO37 must be HIGH to read. Earlier guess of x2 was wrong.
9. **Battery connector** is JST-SH1.25, not PH2.0 (5).
10. **RadioLib** is used for the SX1262 (same library Meshtastic uses). The receiver uses
    `startReceive()` plus a DIO1 interrupt flag.
11. The V4 has no PlatformIO board id; the V3 id works.
12. Check every GPIO against the **official datasheet**, not forum posts. Wrong pins fail loudly or
    (like Vext_Ctrl) fail quietly, so always add a hardware sanity step (I2C scan, LED blink, serial).

13a. **V3 vs V4 serial** (13): V3 = CP2102 UART, no USB-CDC flag; V4 = native USB, flag required (5.1).
13b. **The V4 front-end is off unless you drive it** (5.2). With GPIO7/GPIO2/TX-pin untouched the radio
     still "works" but at roughly -60 dB: every earlier range impression was handicapped.
13c. **Check the V4 hardware revision** (silkscreen). V4.2 and V4.3 use different front-ends and a different
     TX pin. A second "V4" can be a different revision from the first.
13d. **Charging inflates battery readings** and the charge LED is red while USB charges (8.7).
13e. TX power set in firmware is the SX1262 drive into the front-end, not antenna power (about +7..+13 dB more).
13f. A first-packet battery outlier appeared on a cold battery boot (8.7); watch the Grafana battery panel for
     spikes at reboots.

Process
13. Hardware-free tests are worth it: `pio test -e native` proves C++/Python packet equivalence.
14. Build and verify in layers (codec -> bridge with fake receiver -> real stack -> real board -> real GPS).
15. Wrap long-running or noisy shell commands; broad `find` over `$HOME` times out, search specific folders.

---

## 11. Open work, known gaps, flight-prep notes

### 11.1 Planned firmware work (deferred, in rough priority order)
1. Real temperature in the tracker (8.5). Battery ADC is done (8.7) but still needs a multimeter check and a
   cold-boot re-test of the outlier fix.
2. Tracker power: turn the OLED off after boot (or sleep it), drop CPU clock, light sleep between TX
   instead of `delay(2000)`. Current draw is estimated, not measured.
3. Hardware watchdog (`esp_task_wdt`), NVS-persisted `seq`/boot counter, set the `rebooted` flag on the
   first packet after a power-loss reboot.
4. Proper rolling-hour **duty-cycle limiter** (currently a fixed 2 s delay); relevant to legal
   limits in the 869 MHz sub-band and to SF11/SF12 range tests.
5. Optional downlink ACK so the tracker can display "connected to base" and RSSI/SNR (8.4).
6. Receiver (WiFi mode only): LittleFS ring buffer and replay when WiFi/MQTT is down; WiFi auto-reconnect. USB mode (7.1) avoids the problem.
7. Dashboard: hide `fix=0` positions (9.6); per-receiver variable.
7b. **Receiver position is static**: `HAB_RECEIVER_LAT/LON/ALT_M` are hard-coded in `config.h`, so distance and
    slant-range panels measure from that point, not from where the laptop actually is. Matters for walking range
    tests: set the coordinates to the real spot, or feed the laptop's position (e.g. in `serial_to_mqtt.py`).
7c. USB-mode forwarder (7.1) is started by hand; add a `launchd` job (start at login, restart on exit) so a
    reboot or closed terminal does not silently stop the feed. The Data feed banner only warns, it does not heal.
8. MQTT auth/TLS before the base station leaves a trusted network.
9. Configurable SF/TX interval via build flags for the T6 range-test matrix (SF7/9/11/12). TX power is done
   (`HAB_TX_POWER_DBM`). Higher SF needs a longer send interval, see 11.2.

### 11.2 Radio/regulatory reminders
- Tracker TX power is `HAB_TX_POWER_DBM` = 17 dBm into the front-end, estimated at roughly 26-27 dBm at the
  antenna (not measured). EU 869.4-869.65 MHz allows 500 mW ERP (27 dBm) at <=10% duty cycle: that is
  the ceiling, so for range, improve the antenna and placement rather than raising power. Confirm the
  current legal limits for Germany yourself.
- **Duty cycle**: SF9, 29-byte packet, 8-symbol preamble is about 226 ms; sent every ~2.3 s is about
  9.8%, right at the 10% limit. Any higher SF needs a longer interval; the limiter is still a fixed delay (11.1).
- Never power the radio without an antenna attached.
- Spec's regulatory inquiry (German authorities, Brandenburg launches) is still outstanding and gates Phase 3.
- Spec test plan T1 to T9 (power budget, GNSS fix time, 24 h soak, freezer, range tests, antenna
  comparison, weigh-in) is not yet run.

### 11.3 Phase 1 flight-prep notes (estimates, not measurements)
- **Payload weight**: 17 g without battery, plus a small plastic enclosure; target about 50 g total.
- **Battery**: needs a JST-SH1.25 plug (5). Estimated average draw with today's unoptimised firmware is
  roughly 90 to 140 mA (ESP32 40-60, GNSS 25-50, OLED 15-20, LoRa TX 5-8), giving about 3 h on
  400 mAh (range 2.3 to 3.8 h), less in the cold. Weights: 400 mAh about 8-10 g, 600 mAh about 12 g,
  850 mAh about 15-17 g, 1000 mAh about 18-20 g. **Measure real current with a USB power meter before relying on this.**
- **Balloon**: the owner has a 1 m party balloon. Helium gives about 1.05 g lift per litre at sea level;
  a 50 g payload plus balloon weight plus about 50 g free lift needs roughly 120-250 g gross lift
  (about 115-240 L), i.e. only about 60-80 cm diameter, **not** a full 1 m fill. Weigh the empty
  balloon first (party latex can be 80-150 g) and set fill with a spring/luggage scale reading free lift
  directly. Phase 1 per spec is "fly until contact is lost", no recovery assumed.

---

## 12. Notes for a future Claude Code session

- Start by reading this file, then `firmware/common/hab_board/board_pins_v4.h` and the two `main.cpp` files.
- The user prefers PlatformIO, wants **receiver and tracker flashable separately**, and authorised
  driving flashing/serial directly from the terminal on their Mac (still ask before anything physical:
  they must press BOOT/RESET, move USB cables, and place the board outdoors).
- Never print or commit `.env` or `firmware/receiver/include/config.h` (contain real credentials; both gitignored).
- Verify, do not assume: for firmware changes compile, flash, capture serial; for dashboard changes
  look at the real panels in the browser. Type checks do not prove UI or radio behaviour.
- Protocol changes touch four places: `bridge/packet.py`, `hab_packet.{h,cpp}`, `packet_test`, `bridge/bridge.py`.
- Treat anything in 11.3 as a calculation to be confirmed. Battery is now real but unchecked against a
  multimeter (8.7); temperature is still a placeholder (8.5), so do not present either as verified.
- In a git worktree the gitignored `receiver/include/config.h` is missing; copy it from the main checkout.
- Tooling notes from the 2026-10-04 session: macOS has no `timeout`; `pio device monitor` needs a real TTY, so
  script serial with pyserial from PlatformIO's own venv
  (`~/Library/Application Support/pipx/venvs/platformio/bin/python`, system python3 has no pyserial);
  there is no `pdftotext`/poppler, so PDFs cannot be read without `brew install poppler`.
- Useful tool habits learned: use `docker compose ps` before assuming the stack is down; Grafana dashboards
  only render fully after a few seconds, and the geomap swallows page-scroll gestures (scroll with the pointer at the right edge).

---

## 13. Session log: 2026-10-04 (V3 base station, new V4 tracker, antenna path, battery)

Branch `claude/heltec-firmware-flashing-a6e51d`, PR #1 into `main`. Commits (oldest first): V3 receiver env;
V4 front-end control + `HAB_TX_POWER_DBM`; receiver RX boosted gain; real battery read + LiPo curve + boot
warm-up. The guide update itself is a separate commit.

### 13.1 Goals
1. Use a second, flatter, lighter V4 (no pin headers) as the new balloon tracker.
2. Move the base station onto a Heltec V3 (has an antenna and an enclosure).
3. Optimise for range, and make the tracker battery level real.

### 13.2 Base station on the V3
- Expectation checked first: all LoRa/OLED/Vext/LED/ADC pins match V4 (5.1), so no pin changes.
- The V3 showed up as `/dev/cu.usbserial-0001` (CP2102). **Problem found**: the existing env sets
  `-DARDUINO_USB_CDC_ON_BOOT=1`, which would have left the V3's serial silent. Added a separate
  `[env:heltec_wifi_lora_32_V3]` to `receiver/platformio.ini` that `extends` the V4 env but omits that flag.
- Flashed with `pio run -e heltec_wifi_lora_32_V3 -t upload --upload-port /dev/cu.usbserial-0001`
  (needed `config.h` copied into the worktree).
- First boot: WiFi OK, but MQTT failed (rc=-2, "Connection reset by peer") simply because Docker Desktop was
  not running. Started Docker and `docker compose up -d` from the main checkout; the broker IP
  (192.168.178.102) was unchanged. Then: MQTT connected, radio listening at 869.525 MHz SF9. The OLED
  was confirmed working by the user.
- V3 sticker: 863~928 MHz, so it covers 869.525 MHz.

### 13.3 New tracker on the second V4
- Showed up as `/dev/cu.usbmodem101` (303A:1001). Flashed with `pio run -t upload --upload-port ...`.
- Immediately transmitted and the V3 received it; bridge showed 0% loss. GPS first showed `fix=0` with
  `gps_sentences=0`. Indoors alone does not explain zero parsed sentences (a module without sky still emits
  NMEA), but after the board went outside it got a fix (`fix=3`, 5 then 7-8 satellites) within a few
  minutes and the sentence counter climbed, so the module and wiring were fine.
- Boot banner showed `$GPTXT,...ANTENNA OPEN`; see 8.6.

### 13.4 Antenna path and range settings
- Found the tracker never drove the V4 front-end (5.2). Added `setupFem()` and the per-transmit TX-pin toggle.
- Revision identification: the new board is V4.3/R8 (KCT8103L, TX pin GPIO5). Pin roles from Meshtastic's
  driver; Heltec's PDF had no extractable pin table.
- Added `HAB_TX_POWER_DBM` (default 17, replacing the hard-coded 2 dBm).
- Measured at the receiver with the boards close together: RSSI about -74 dBm (2 dBm TX, front-end undriven)
  to about -14 dBm (17 dBm TX, front-end driven), SNR about 10.5-11.5 dB both times. -14 dBm is near receiver
  saturation, so SNR pinned and the numbers do not give a real range figure yet.
- Receiver: enabled RX boosted gain (7). Not measurable at close range.
- Estimated output about 26-27 dBm, EU limit 27 dBm ERP at <=10% duty; current duty about 9.8% (11.2).

### 13.5 Battery
- Replaced the 4100 mV placeholder with a calibrated ADC read and a LiPo curve (8.7).
- Test sequence with the real pack, watching `batt_mv` through the radio link while USB was unplugged:
  USB-only reading about 4141 mV; unplug USB (tracker stops, no battery); connect battery (cold boot, seq 0
  reads 4234 then settles at about 3837 mV, OLED 48%); reconnect USB (jump to about 3964, then slow climb to
  about 4043 mV, OLED 77%, red charge LED on).
- Boot warm-up reads added for the seq 0 outlier; verified only with a USB reset, not a cold battery boot.

### 13.6 Still open after this session
- Multimeter comparison for battery accuracy; cold battery boot re-test of the outlier fix.
- Distance test with boards far apart (real range, real effect of boosted gain, conducted-power sanity).
- V4.2/GC1109 path and the V4.3 pin assignments are not independently verified beyond the RSSI result.
- Temperature is still a placeholder; duty-cycle limiter, watchdog, power saving not built.
- GNSS "ANTENNA OPEN" message on the second V4 left uninvestigated.

---

## 14. Session log: 2026-10-04/05 (offline field mode, data-feed banner, walk test)

Problem: everything (radio, Grafana) died away from home WiFi. Cause: the receiver firmware joined one SSID and
blocked in `connectWiFi()` forever, published to a LAN-only broker IP, and had no reconnect.

### 14.1 What was built
- **USB mode** for the receiver (`HAB_USE_WIFI 0`): no WiFi/SNTP/MQTT, JSON per packet over serial; laptop-side
  `tools/serial_to_mqtt.py` forwards to `localhost:1883` and adds `rx_timestamp` (details in 7.1).
- **Offline map**: `tilecache` container (nginx caching proxy to OpenStreetMap, port 8080, `tile-cache` volume)
  and the geomap basemap switched to `xyz`. First version failed with 502 because nginx needs
  `resolver 127.0.0.11` when `proxy_pass` uses a variable; fixed.
- **Data feed banner** (panel 12, 9.4).
- Merged `claude/heltec-firmware-flashing-a6e51d` and `claude/connection-loss-outside-wifi-4ea6df` into `main`;
  receiver and tracker both compile after the merge.

### 14.2 Gotchas found
1. **Board variant**: flashing the V3 (CP2102, `usbserial`) with the V4 env (`ARDUINO_USB_CDC_ON_BOOT=1`) gives a
   board that runs but prints nothing, so USB mode shows no data. Use `-e heltec_wifi_lora_32_V3` (7.1).
2. **Silent failure**: with the forwarder not running, USB mode produces no data and no error. Hence the banner.
3. A git worktree lacks the gitignored `config.h`; copy it from the main checkout before building the receiver.
4. Local `main` had drifted from `origin/main`; the main checkout was on `clean-main`. Always `git fetch` first.

### 14.3 Field test (verified by the user)
- WiFi off, tracker left in a window, laptop and receiver walked through built-up Berlin Mitte.
- Data feed stayed `LIVE` offline; all panels kept updating; the map showed cached tiles.
- Link held through many buildings: RSSI fell to about -105 dBm and SNR dipped just below 0 dB, then recovered
  on the way back (SF9, 125 kHz, tracker TX 17 dBm setting). The dashboard showed a track on the map; GPS fix quality
  was not separately re-checked this session (tracker showed `sats=0 fix=0` earlier indoors/at the sill).
- Not yet measured: maximum range, packet loss versus distance with a correct receiver position (11.1 item 7b).
