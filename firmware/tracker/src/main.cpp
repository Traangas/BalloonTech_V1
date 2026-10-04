// HAB Phase 0 tracker - bring-up milestone 2: real GPS + LoRa TX.
//
// Adds the GNSS module on top of the proven TX smoke test: configures a
// high-dynamics flight mode on boot, parses NMEA continuously, and
// transmits real position/fix/satellite data instead of the earlier fixed
// dummy values.
//
// The module's boot banner (MA=CASIC, IC=AT6558R-...) revealed this is a
// CASIC AT6558R chipset, not the Quectel L76K the spec assumed - a
// completely different command protocol. CASIC has no NMEA "balloon mode"
// command; the dynamic-model equivalent is the binary CSIP protocol
// (CASIC Multimode Satellite Navigation Receiver Protocol Spec V4.2.0.3,
// section 2.11.8, CFG-NAVX, class 0x06 id 0x07), field dyModel, value 5 =
// "Flight mode acceleration <1g" - the direct analog of u-blox's
// "Airborne <1g" dynamic platform model used for HAB trackers.
//
// Still missing (later increments): sleep/duty-cycle enforcement, watchdog,
// NVS seq/boot-counter persistence, battery ADC, WiFi/BT/OLED power-down.
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Arduino.h>
#include <RadioLib.h>
#include <TinyGPSPlus.h>
#include <Wire.h>

#include "board_pins_v4.h"
#include "hab_packet.h"
#include "radio_config.h"

SPIClass loraSpi(HSPI);
SX1262 radio = new Module(HAB_LORA_NSS, HAB_LORA_DIO1, HAB_LORA_RST, HAB_LORA_BUSY, loraSpi);

HardwareSerial gnssSerial(1);
TinyGPSPlus gps;

Adafruit_SSD1306 oled(128, 64, &Wire, HAB_OLED_RST);
bool oledOk = false;

uint16_t seq = 0;
bool airborneModeConfirmed = false;
bool lastTxOk = false;

// CASIC CSIP binary protocol (protocol spec section 2.2): header 0xBA 0xCE,
// u16 LE payload length, class byte, id byte, payload, u32 LE checksum.
// Checksum = (id<<24)+(class<<16)+len, then + each payload word (4 bytes
// LE), all as wrapping u32 arithmetic - exact algorithm from section 2.2.
uint32_t casicChecksum(uint8_t msgClass, uint8_t msgId, uint16_t payLen, const uint8_t* payload) {
  uint32_t ck = (static_cast<uint32_t>(msgId) << 24) | (static_cast<uint32_t>(msgClass) << 16) | payLen;
  for (uint16_t i = 0; i + 4 <= payLen; i += 4) {
    uint32_t word = static_cast<uint32_t>(payload[i]) | (static_cast<uint32_t>(payload[i + 1]) << 8) |
                     (static_cast<uint32_t>(payload[i + 2]) << 16) |
                     (static_cast<uint32_t>(payload[i + 3]) << 24);
    ck += word;
  }
  return ck;
}

void sendCasicPacket(uint8_t msgClass, uint8_t msgId, const uint8_t* payload, uint16_t payLen) {
  uint8_t hdr[6] = {0xBA, 0xCE, static_cast<uint8_t>(payLen & 0xFF), static_cast<uint8_t>(payLen >> 8),
                    msgClass, msgId};
  uint32_t cksum = casicChecksum(msgClass, msgId, payLen, payload);
  uint8_t ckBytes[4] = {static_cast<uint8_t>(cksum & 0xFF), static_cast<uint8_t>(cksum >> 8),
                         static_cast<uint8_t>(cksum >> 16), static_cast<uint8_t>(cksum >> 24)};
  gnssSerial.write(hdr, sizeof(hdr));
  if (payLen > 0) gnssSerial.write(payload, payLen);
  gnssSerial.write(ckBytes, 4);
  Serial.printf("CASIC >> class=0x%02X id=0x%02X len=%u\n", msgClass, msgId, payLen);
}

// CFG-NAVX (0x06 0x07), 44-byte payload. Only mask bit B0 set (apply
// dynamic model) so every other field is ignored by the receiver.
void setCasicDynamicModel(uint8_t dyModel) {
  uint8_t payload[44] = {0};
  payload[0] = 0x01;  // mask B0: apply dyModel
  payload[4] = dyModel;
  sendCasicPacket(0x06, 0x07, payload, sizeof(payload));
}

// Scans the raw byte stream for a CASIC ACK-ACK/ACK-NACK (class 0x05)
// answering the given class/id. Safe to interleave with NMEA text on the
// same UART - NMEA is printable ASCII and never contains the 0xBA 0xCE
// header bytes. Returns true only on ACK-ACK; false on ACK-NACK or timeout.
bool waitForCasicAck(uint8_t expectClass, uint8_t expectId, uint32_t timeoutMs) {
  uint32_t start = millis();
  auto readByte = [&](uint8_t& out) -> bool {
    while (!gnssSerial.available()) {
      if (millis() - start >= timeoutMs) return false;
    }
    out = gnssSerial.read();
    return true;
  };

  while (millis() - start < timeoutMs) {
    uint8_t b;
    if (!readByte(b)) return false;
    if (b != 0xBA) continue;
    if (!readByte(b) || b != 0xCE) continue;

    uint8_t hdr[4];
    bool ok = true;
    for (int i = 0; i < 4 && ok; i++) ok = readByte(hdr[i]);
    if (!ok) return false;
    uint16_t payLen = hdr[0] | (static_cast<uint16_t>(hdr[1]) << 8);
    uint8_t msgClass = hdr[2];
    uint8_t msgId = hdr[3];

    uint8_t payload[64];
    uint16_t toRead = payLen > sizeof(payload) ? sizeof(payload) : payLen;
    for (uint16_t i = 0; i < toRead && ok; i++) ok = readByte(payload[i]);
    for (uint16_t i = toRead; i < payLen && ok; i++) {
      uint8_t discard;
      ok = readByte(discard);
    }
    for (int i = 0; i < 4 && ok; i++) {
      uint8_t discard;
      ok = readByte(discard);
    }
    if (!ok) return false;

    if (msgClass == 0x05 && payLen >= 2) {
      Serial.printf("CASIC << ACK msgId=0x%02X ackFor=(0x%02X,0x%02X)\n", msgId, payload[0], payload[1]);
      if (payload[0] == expectClass && payload[1] == expectId) {
        return msgId == 0x01;  // 0x01 = ACK-ACK, 0x00 = ACK-NACK
      }
    }
  }
  return false;
}

// Blocks (bounded by timeoutMs), echoing every raw NMEA line the module
// sends to Serial - this is where boot-time messages like antenna status
// ($GPTXT ...ANTENNA...) show up. Returns true once at least one line seen.
bool waitForGnssAlive(uint32_t timeoutMs) {
  uint32_t start = millis();
  char line[96];
  size_t lineLen = 0;
  bool sawLine = false;

  while (millis() - start < timeoutMs) {
    while (gnssSerial.available()) {
      char c = (char)gnssSerial.read();
      if (c == '\n' || c == '\r') {
        if (lineLen > 0) {
          line[lineLen] = '\0';
          Serial.printf("GNSS << %s\n", line);
          lineLen = 0;
          sawLine = true;
        }
      } else if (lineLen < sizeof(line) - 1) {
        line[lineLen++] = c;
      }
    }
  }
  return sawLine;
}

void setupGnss() {
  pinMode(HAB_GNSS_POWER_PIN, OUTPUT);
  // Datasheet: the board's other "_Ctrl" power-enable pin (VextCtrl) is
  // active-low ("pulled low" to enable that supply rail) - VGNSS_Ctrl is
  // assumed to follow the same convention until proven otherwise.
  digitalWrite(HAB_GNSS_POWER_PIN, LOW);

  // Default 256-byte RX buffer overflows between reads once the TX loop's
  // 2s cadence leaves the UART undrained for a full cycle - must be set
  // before begin().
  gnssSerial.setRxBufferSize(2048);
  gnssSerial.begin(9600, SERIAL_8N1, HAB_GNSS_TX_PIN, HAB_GNSS_RX_PIN);

  bool alive = waitForGnssAlive(3000);
  Serial.printf("GNSS module %s\n", alive ? "alive" : "NOT responding after 3s");

  setCasicDynamicModel(5);  // Flight mode, accel <1g - see spec section 6, tracker item 1
  airborneModeConfirmed = waitForCasicAck(0x06, 0x07, 2000);
  Serial.printf("GNSS dynamic model (flight <1g) %s\n", airborneModeConfirmed ? "CONFIRMED" : "NOT confirmed");
}

// External PA/LNA front-end: powered and enabled once, TX path switched per
// packet. Left in the RX/LNA position between packets.
void setupFem() {
  pinMode(HAB_FEM_POWER_PIN, OUTPUT);
  digitalWrite(HAB_FEM_POWER_PIN, HIGH);
  pinMode(HAB_FEM_CSD_PIN, OUTPUT);
  digitalWrite(HAB_FEM_CSD_PIN, HIGH);
  pinMode(HAB_FEM_TX_PIN, OUTPUT);
  digitalWrite(HAB_FEM_TX_PIN, LOW);
  delay(5);  // let the FEM supply settle before the first transmit
}

void setupRadio() {
  setupFem();
  loraSpi.begin(HAB_LORA_SCK, HAB_LORA_MISO, HAB_LORA_MOSI, HAB_LORA_NSS);

  int state = radio.begin(HAB_RADIO_FREQ_MHZ, HAB_RADIO_BANDWIDTH_KHZ, HAB_SF,
                           HAB_RADIO_CODING_RATE, HAB_RADIO_SYNC_WORD,
                           HAB_TX_POWER_DBM,
                           HAB_RADIO_PREAMBLE_SYMBOLS);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("radio.begin() failed, code %d - halting\n", state);
    while (true) delay(1000);
  }

  radio.explicitHeader();
  radio.setCRC(true);
  radio.autoLDRO();

  Serial.printf("radio ready: %.3f MHz, SF%d, BW%.0fkHz, CR4/%d, tx=%ddBm (into FEM)\n",
                HAB_RADIO_FREQ_MHZ, HAB_SF, HAB_RADIO_BANDWIDTH_KHZ, HAB_RADIO_CODING_RATE,
                HAB_TX_POWER_DBM);
}

void setupDisplayAndLed() {
  pinMode(HAB_LED_PIN, OUTPUT);
  digitalWrite(HAB_LED_PIN, LOW);

  // OLED's power rail is gated by Vext_Ctrl - must be pulled low before
  // Wire.begin()/oled.begin() or the display is unpowered and its SDA pin
  // sags low, making every I2C address falsely ACK. Confirmed on hardware.
  pinMode(HAB_VEXT_CTRL_PIN, OUTPUT);
  digitalWrite(HAB_VEXT_CTRL_PIN, LOW);
  delay(50);

  Wire.begin(HAB_OLED_SDA, HAB_OLED_SCL);
  oledOk = oled.begin(SSD1306_SWITCHCAPVCC, HAB_OLED_I2C_ADDR);
  if (!oledOk) {
    Serial.println("OLED init failed - continuing without display");
    return;
  }
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.println("HAB TRACKER");
  oled.println("booting...");
  oled.display();
}

// Battery voltage in mV. The board's divider is only connected while
// ADC_CTRL is high (datasheet 2.2.2), and its ~80k source impedance needs
// time to charge the ADC sample cap, so: enable, settle, discard a couple of
// reads, then take a trimmed mean (drop the lowest and highest quarter) so a
// stray spike can't reach the packet. analogReadMilliVolts() applies the
// chip's factory ADC calibration, which is what makes this a real voltage
// rather than a raw count.
uint16_t readBatteryMv() {
  constexpr int kSamples = 16;
  uint32_t samples[kSamples];
  digitalWrite(HAB_BATT_ADC_CTRL_PIN, HIGH);
  delay(10);
  analogReadMilliVolts(HAB_BATT_ADC_PIN);
  analogReadMilliVolts(HAB_BATT_ADC_PIN);
  for (int i = 0; i < kSamples; i++) {
    samples[i] = analogReadMilliVolts(HAB_BATT_ADC_PIN);
    delay(2);
  }
  digitalWrite(HAB_BATT_ADC_CTRL_PIN, LOW);
  for (int i = 1; i < kSamples; i++) {  // insertion sort
    uint32_t v = samples[i];
    int j = i - 1;
    while (j >= 0 && samples[j] > v) { samples[j + 1] = samples[j]; j--; }
    samples[j + 1] = v;
  }
  uint32_t sum = 0;
  for (int i = kSamples / 4; i < kSamples - kSamples / 4; i++) sum += samples[i];
  return static_cast<uint16_t>(sum / (kSamples / 2) * HAB_BATT_ADC_SCALE + 0.5f);
}

void setupBattery() {
  pinMode(HAB_BATT_ADC_CTRL_PIN, OUTPUT);
  digitalWrite(HAB_BATT_ADC_CTRL_PIN, LOW);
  // Divided battery voltage is at most ~0.86 V; 6 dB attenuation (~1.75 V
  // full scale) keeps it in a better-resolved range than the 11 dB default.
  analogSetPinAttenuation(HAB_BATT_ADC_PIN, ADC_6db);
  // A cold power-up on battery read ~400 mV high on the first packet only
  // (seen once, cause not isolated). Run warm-up reads now, spread over
  // ~300 ms, so the ADC and rail have settled by the first real sample.
  for (int i = 0; i < 6; i++) {
    readBatteryMv();
    delay(50);
  }
}

// LiPo voltage-to-percent from a typical single-cell resting-voltage table,
// linearly interpolated. Approximate: not calibrated to this cell, and the
// reading sags under load and reads high while USB is charging the cell.
uint8_t battPercent(uint16_t mv) {
  static const struct { uint16_t mv; uint8_t pct; } kCurve[] = {
      {3300, 0},  {3610, 5},  {3690, 10}, {3730, 20}, {3770, 30}, {3800, 40},
      {3840, 50}, {3870, 60}, {3910, 65}, {3950, 70}, {3980, 75}, {4020, 80},
      {4080, 85}, {4110, 90}, {4150, 95}, {4200, 100},
  };
  constexpr size_t kN = sizeof(kCurve) / sizeof(kCurve[0]);
  if (mv <= kCurve[0].mv) return 0;
  if (mv >= kCurve[kN - 1].mv) return 100;
  for (size_t i = 1; i < kN; i++) {
    if (mv <= kCurve[i].mv) {
      float t = static_cast<float>(mv - kCurve[i - 1].mv) / (kCurve[i].mv - kCurve[i - 1].mv);
      return static_cast<uint8_t>(kCurve[i - 1].pct + t * (kCurve[i].pct - kCurve[i - 1].pct) + 0.5f);
    }
  }
  return 100;
}

// "LoRa TX: OK/FAIL" reflects the radio hardware accepting the last
// transmit, not that the base station actually received it - a one-way
// broadcast link has no way to know that without a downlink ACK, which
// isn't implemented (out of scope: would cost duty-cycle budget and add
// protocol complexity for a Phase 0 nice-to-have).
void updateDisplay(const hab::PacketFields& f) {
  if (!oledOk) return;
  oled.clearDisplay();
  oled.setCursor(0, 0);
  oled.printf("HAB TRACKER  #%u\n", f.node_id);
  oled.printf("Alt:%5.0fm Sat:%2u\n", f.alt_m, f.sats);
  oled.printf("GPS: %s\n", f.fix >= hab::kFix2D ? "LOCK" : "NO FIX");
  oled.printf("LoRa TX: %s\n", lastTxOk ? "OK" : "FAIL");
  oled.printf("Batt: %3u%% %.2fV\n", battPercent(f.batt_mv), f.batt_mv / 1000.0f);
  oled.printf("Seq: %u\n", f.seq);
  oled.display();
}

// Howard Hinnant's civil_from_days algorithm, UTC, no external tz/libc state.
uint32_t unixTimeFromGps(int year, int month, int day, int hour, int minute, int second) {
  int y = year - (month <= 2 ? 1 : 0);
  int era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = static_cast<unsigned>(y - era * 400);
  unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  int64_t days = era * 146097LL + static_cast<int64_t>(doe) - 719468;
  return static_cast<uint32_t>(days * 86400LL + hour * 3600L + minute * 60L + second);
}

void loop() {
  while (gnssSerial.available()) {
    gps.encode(gnssSerial.read());
  }

  hab::PacketFields f{};
  f.node_id = 1;
  f.seq = seq;

  if (gps.date.isValid() && gps.time.isValid()) {
    f.gps_time = unixTimeFromGps(gps.date.year(), gps.date.month(), gps.date.day(), gps.time.hour(),
                                  gps.time.minute(), gps.time.second());
  } else {
    f.gps_time = 0;
  }

  if (gps.location.isValid()) {
    f.lat = gps.location.lat();
    f.lon = gps.location.lng();
    // TinyGPSPlus has no direct GSA fix-type field; a valid altitude read
    // alongside a valid location is a reasonable proxy for 3D vs 2D fix.
    f.fix = gps.altitude.isValid() ? hab::kFix3D : hab::kFix2D;
  } else {
    f.lat = 0;
    f.lon = 0;
    f.fix = hab::kFixNone;
  }
  f.alt_m = gps.altitude.isValid() ? static_cast<float>(gps.altitude.meters()) : 0.0f;
  f.sats = gps.satellites.isValid() ? static_cast<uint8_t>(gps.satellites.value()) : 0;

  f.batt_mv = readBatteryMv();
  f.temp_c = 21.0f;
  f.flags = airborneModeConfirmed ? hab::kFlagAirborneMode : 0;

  uint8_t buf[hab::kPacketSize];
  hab::encodePacket(f, buf, sizeof(buf));

  digitalWrite(HAB_FEM_TX_PIN, HIGH);
  int state = radio.transmit(buf, hab::kPacketSize);
  digitalWrite(HAB_FEM_TX_PIN, LOW);
  lastTxOk = (state == RADIOLIB_ERR_NONE);

  digitalWrite(HAB_LED_PIN, HIGH);
  delay(80);  // brief flash on every packet sent - negligible vs the 2s cycle
  digitalWrite(HAB_LED_PIN, LOW);

  updateDisplay(f);

  Serial.printf("TX seq=%u %s fix=%u sats=%u lat=%.5f lon=%.5f alt=%.1f batt_mv=%u gps_chars=%u gps_sentences=%u gps_fail=%u\n",
                seq, lastTxOk ? "ok" : "FAILED", f.fix, f.sats, f.lat, f.lon, f.alt_m, f.batt_mv,
                gps.charsProcessed(), gps.sentencesWithFix(), gps.failedChecksum());

  seq++;
  delay(2000);
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\nHAB Phase 0 tracker - GPS + TX bring-up");

  setupBattery();
  setupDisplayAndLed();
  setupGnss();
  setupRadio();
}
