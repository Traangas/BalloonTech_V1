// HAB Phase 0 receiver: continuous LoRa RX -> validate -> MQTT + serial.
//
// Publishes the exact JSON shape bridge/bridge.py already expects on
// hab/rx/<receiver_id>, so the bridge needs zero changes - this firmware is
// a drop-in replacement for bridge/simulate_receiver.py.
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Arduino.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <RadioLib.h>
#include <WiFi.h>
#include <Wire.h>
#include <time.h>

#include "board_pins_v4.h"
#include "config.h"
#include "hab_packet.h"
#include "radio_config.h"

// HAB_USE_WIFI 0 = USB mode: no WiFi/MQTT/NTP, JSON lines go out over serial and
// tools/serial_to_mqtt.py on the laptop forwards them. Default keeps WiFi/MQTT.
#ifndef HAB_USE_WIFI
#define HAB_USE_WIFI 1
#endif

SPIClass loraSpi(HSPI);
SX1262 radio = new Module(HAB_LORA_NSS, HAB_LORA_DIO1, HAB_LORA_RST, HAB_LORA_BUSY, loraSpi);

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

Adafruit_SSD1306 oled(128, 64, &Wire, HAB_OLED_RST);
bool oledOk = false;

volatile bool packetReceivedFlag = false;
uint32_t validPackets = 0;
uint32_t invalidPackets = 0;

float lastRssi = 0;
float lastSnr = 0;
uint32_t lastValidPacketMillis = 0;  // 0 = none received yet
time_t lastValidPacketEpoch = 0;

// A packet arriving at least this recently counts as "linked" - generous
// margin over the current 2s TX cadence; loosen if the tracker's interval
// changes (e.g. for the SF11/12 range-test matrix).
constexpr uint32_t kLinkTimeoutMs = 15000;

void IRAM_ATTR onRadioAction() { packetReceivedFlag = true; }

#if HAB_USE_WIFI
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(HAB_WIFI_SSID, HAB_WIFI_PASSWORD);
  Serial.printf("connecting to WiFi '%s'", HAB_WIFI_SSID);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.printf("\nWiFi connected, IP=%s\n", WiFi.localIP().toString().c_str());

  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  Serial.print("waiting for SNTP time sync");
  time_t now = time(nullptr);
  while (now < 1700000000) {
    delay(250);
    Serial.print(".");
    now = time(nullptr);
  }
  Serial.println();
}

void connectMqtt() {
  mqtt.setServer(HAB_MQTT_HOST, HAB_MQTT_PORT);
  while (!mqtt.connected()) {
    Serial.printf("connecting to MQTT %s:%d...\n", HAB_MQTT_HOST, HAB_MQTT_PORT);
    String clientId = String("hab-receiver-") + HAB_RECEIVER_ID;
    if (mqtt.connect(clientId.c_str())) {
      Serial.println("MQTT connected");
    } else {
      Serial.printf("MQTT connect failed, rc=%d, retrying in 3s\n", mqtt.state());
      delay(3000);
    }
  }
}

#endif  // HAB_USE_WIFI

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
  oled.println("HAB BASE STATION");
  oled.println("booting...");
  oled.display();
}

void updateDisplay() {
  if (!oledOk) return;

  bool linked =
      lastValidPacketMillis != 0 && (millis() - lastValidPacketMillis) < kLinkTimeoutMs;

  char lastStr[16] = "never";
  if (lastValidPacketEpoch != 0) {
    struct tm tmv;
    gmtime_r(&lastValidPacketEpoch, &tmv);
    snprintf(lastStr, sizeof(lastStr), "%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
  }

  oled.clearDisplay();
  oled.setCursor(0, 0);
  oled.printf("HAB BASE %s\n", HAB_RECEIVER_ID);
#if HAB_USE_WIFI
  oled.printf("WiFi:%s MQTT:%s\n", WiFi.status() == WL_CONNECTED ? "OK" : "NO",
              mqtt.connected() ? "OK" : "NO");
  oled.printf("IP:%s\n", WiFi.localIP().toString().c_str());
#else
  oled.println("USB serial mode");
  oled.println();
#endif
  oled.printf("LoRa: %s\n", linked ? "LINKED" : "NO SIGNAL");
  oled.printf("RSSI:%.0f SNR:%.1f\n", lastRssi, lastSnr);
  oled.printf("Last: %s\n", lastStr);
  oled.printf("OK:%lu BAD:%lu\n", (unsigned long)validPackets, (unsigned long)invalidPackets);
  oled.display();
}

void setupRadio() {
  loraSpi.begin(HAB_LORA_SCK, HAB_LORA_MISO, HAB_LORA_MOSI, HAB_LORA_NSS);

  int state = radio.begin(HAB_RADIO_FREQ_MHZ, HAB_RADIO_BANDWIDTH_KHZ, HAB_SF,
                           HAB_RADIO_CODING_RATE, HAB_RADIO_SYNC_WORD,
                           17 /* txPower, unused in RX-only but required by begin() */,
                           HAB_RADIO_PREAMBLE_SYMBOLS);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("radio.begin() failed, code %d - halting\n", state);
    while (true) delay(1000);
  }

  radio.explicitHeader();
  radio.setCRC(true);
  radio.autoLDRO();

  radio.setDio1Action(onRadioAction);
  state = radio.startReceive();
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("startReceive() failed, code %d - halting\n", state);
    while (true) delay(1000);
  }
  Serial.printf("listening: %.3f MHz, SF%d, BW%.0fkHz, CR4/%d\n", HAB_RADIO_FREQ_MHZ, HAB_SF,
                HAB_RADIO_BANDWIDTH_KHZ, HAB_RADIO_CODING_RATE);
}

void publishTelemetry(const uint8_t* raw, size_t len, float rssi, float snr, float freqErrorHz) {
  hab::Packet packet;
  hab::DecodeError err = hab::decodePacket(raw, len, &packet);
  if (err != hab::DecodeError::kNone) {
    invalidPackets++;
    Serial.printf("dropped packet: decode error %d (len=%u), invalid=%lu\n",
                   static_cast<int>(err), static_cast<unsigned>(len), (unsigned long)invalidPackets);
    return;
  }
  validPackets++;
  lastRssi = rssi;
  lastSnr = snr;
  lastValidPacketMillis = millis();
  lastValidPacketEpoch = time(nullptr);  // ~0 without NTP (USB mode); display shows "never"

  char hexBuf[hab::kPacketSize * 2 + 1];
  for (size_t i = 0; i < len; ++i) {
    sprintf(hexBuf + i * 2, "%02x", raw[i]);
  }
  hexBuf[len * 2] = '\0';

  JsonDocument doc;
  doc["receiver_id"] = HAB_RECEIVER_ID;
  doc["receiver_lat"] = HAB_RECEIVER_LAT;
  doc["receiver_lon"] = HAB_RECEIVER_LON;
  doc["receiver_alt_m"] = HAB_RECEIVER_ALT_M;
#if HAB_USE_WIFI
  doc["rx_timestamp"] = (double)time(nullptr);
#endif  // USB mode: no clock here, the laptop forwarder stamps rx_timestamp
  doc["rssi"] = rssi;
  doc["snr"] = snr;
  doc["freq_error_hz"] = freqErrorHz;
  doc["raw_hex"] = hexBuf;

  char payload[384];
  size_t payloadLen = serializeJson(doc, payload, sizeof(payload));

#if HAB_USE_WIFI
  String topic = String("hab/rx/") + HAB_RECEIVER_ID;
  mqtt.publish(topic.c_str(), (const uint8_t*)payload, payloadLen, false);
#endif
  Serial.write(payload, payloadLen);
  Serial.println();
  Serial.printf("valid=%lu invalid=%lu node=%u seq=%u fix=%u sats=%u batt_mv=%u\n",
                (unsigned long)validPackets, (unsigned long)invalidPackets, packet.node_id,
                packet.seq, packet.fix, packet.sats, packet.batt_mv);
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\nHAB Phase 0 receiver starting");

  setupDisplayAndLed();
#if HAB_USE_WIFI
  connectWiFi();
  connectMqtt();
#endif
  setupRadio();
  updateDisplay();
}

void loop() {
#if HAB_USE_WIFI
  if (!mqtt.connected()) {
    connectMqtt();
  }
  mqtt.loop();
#endif

  if (packetReceivedFlag) {
    packetReceivedFlag = false;

    digitalWrite(HAB_LED_PIN, HIGH);  // flash on every packet received, valid or not

    uint8_t buf[hab::kPacketSize];
    int len = radio.getPacketLength();
    int state = radio.readData(buf, len);

    if (state == RADIOLIB_ERR_NONE) {
      float rssi = radio.getRSSI();
      float snr = radio.getSNR();
      float freqError = radio.getFrequencyError();
      publishTelemetry(buf, (size_t)len, rssi, snr, freqError);
    } else {
      invalidPackets++;
      Serial.printf("readData() failed, code %d, invalid=%lu\n", state,
                    (unsigned long)invalidPackets);
    }

    radio.startReceive();
    updateDisplay();
    delay(80);
    digitalWrite(HAB_LED_PIN, LOW);
  }

  static uint32_t lastDisplayRefresh = 0;
  if (millis() - lastDisplayRefresh >= 1000) {
    lastDisplayRefresh = millis();
    updateDisplay();
  }
}
