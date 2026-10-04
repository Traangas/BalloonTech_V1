// Shared LoRa radio parameters - spec section 4. Tracker and receiver must
// agree on all of these except SF (receiver must be retuned to match
// whichever SF the tracker is currently transmitting during range tests).
#pragma once

#define HAB_RADIO_FREQ_MHZ 869.525
#define HAB_RADIO_BANDWIDTH_KHZ 125.0
#define HAB_RADIO_CODING_RATE 5  // "4/5" - RadioLib takes just the denominator
#define HAB_RADIO_PREAMBLE_SYMBOLS 8
// Private (non-LoRaWAN) sync word. 0x34 is reserved for LoRaWAN; 0x12 is the
// conventional "private network" value.
#define HAB_RADIO_SYNC_WORD 0x12

#ifndef HAB_SF
#define HAB_SF 9  // override with -DHAB_SF=<7|9|11|12> per the T6 test matrix
#endif

// SX1262 output power in dBm, i.e. the drive INTO the board's external PA,
// not the power at the antenna. The PA adds roughly +7..+13 dB (less gain at
// higher drive), so 17 dBm in lands near 26-27 dBm out. EU 869.4-869.65 MHz
// allows 500 mW ERP (27 dBm) at <=10% duty cycle - don't raise this without
// checking both. Conducted power has not been measured on this hardware.
#ifndef HAB_TX_POWER_DBM
#define HAB_TX_POWER_DBM 17
#endif
