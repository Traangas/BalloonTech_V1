// Heltec WiFi LoRa 32 V4 pin map.
//
// Sourced from Heltec's official "WiFi LoRa 32 V4" datasheet, Rev 1.4 / First
// Official Release (Sep 2025) - pin layout diagram (section 2.1) and Header
// J2/J3 tables (section 2.2), cross-checked against the schematic (section 4).
// LoRa/OLED pins here also match third-party V4 bring-up guides used during
// the receiver's bring-up, and both were confirmed working on real hardware.
#pragma once

// SX1262 (LoRa)
#define HAB_LORA_NSS 8
#define HAB_LORA_SCK 9
#define HAB_LORA_MOSI 10
#define HAB_LORA_MISO 11
#define HAB_LORA_DIO1 14
#define HAB_LORA_RST 12
#define HAB_LORA_BUSY 13

// OLED (SSD1315, I2C). Its power rail is gated by Vext_Ctrl (GPIO36) -
// datasheet section 3.3: "When using VE for external power supply, the
// VextCtrl(GPIO36) pin needs to be pulled low." Confirmed on real hardware:
// without driving this low first, the OLED is unpowered and pulls SDA low,
// so every I2C address falsely ACKs; driving it low first, the bus shows
// exactly the OLED at 0x3C as expected.
#define HAB_OLED_SDA 17
#define HAB_OLED_SCL 18
#define HAB_OLED_RST 21
#define HAB_OLED_I2C_ADDR 0x3C
#define HAB_VEXT_CTRL_PIN 36

// User-controllable status LED. Datasheet Header J2 row 10: "GPIO35,
// SPIIO6, FSPID, SUBSPID, LED" - distinct from the fixed-function
// charge/power LEDs shown in the components diagram, which aren't wired
// to a GPIO.
#define HAB_LED_PIN 35

// Battery voltage sense (tracker only). Datasheet section 2.2.2 footnote:
// "ADC1_CH0 is used to read the lithium battery voltage, the ADC_CTRL(37)
// pin needs to be pulled high. VBAT = 100/(100+390) * VADC_IN1" - i.e. the
// ADC reads a divided-down voltage that must be scaled back UP by
// (100+390)/100 = 4.9 to get real battery voltage, and GPIO37 must be
// driven high before each read or the divider isn't connected.
#define HAB_BATT_ADC_PIN 1
#define HAB_BATT_ADC_CTRL_PIN 37
#define HAB_BATT_ADC_SCALE 4.9f

// GNSS UART + power (tracker only), via the board's SH1.25-8Pin GNSS
// connector (datasheet Header J3 rows 7-11, and the dedicated "GNSS" pin
// group in the section 2.1 layout diagram).
#define HAB_GNSS_TX_PIN 39      // ESP RX <- module TX (GNSS_TX)
#define HAB_GNSS_RX_PIN 38      // ESP TX -> module RX (GNSS_RX)
#define HAB_GNSS_POWER_PIN 34   // VGNSS_Ctrl - module power enable
#define HAB_GNSS_RST_PIN 42     // GNSS_RST
#define HAB_GNSS_PPS_PIN 41     // GNSS_PPS
#define HAB_GNSS_WAKEUP_PIN 40  // GNSS_Wakeup
