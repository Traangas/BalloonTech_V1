// Host-side regression test for the hab_packet codec (bridge/packet.py's C++
// counterpart). Run with `pio test -e native` from firmware/packet_test.
#include <cmath>
#include <cstring>

#include <unity.h>

#include "hab_packet.h"

using namespace hab;

void setUp() {}
void tearDown() {}

void test_crc16_known_vector() {
  const uint8_t vec[] = "123456789";
  TEST_ASSERT_EQUAL_HEX16(0x29B1, crc16Ccitt(vec, 9));
}

void test_encode_matches_python_reference() {
  // Same inputs as bridge/packet.py's own test; output cross-checked
  // byte-for-byte against `encode_packet()` in Python.
  PacketFields f{};
  f.node_id = 1;
  f.seq = 42;
  f.gps_time = 1234567890;
  f.lat = 52.52001;
  f.lon = 13.40495;
  f.alt_m = 150.3f;
  f.sats = 9;
  f.fix = kFix3D;
  f.batt_mv = 4123;
  f.temp_c = 21.7f;
  f.flags = kFlagAirborneMode;

  uint8_t buf[kPacketSize];
  TEST_ASSERT_EQUAL(kPacketSize, encodePacket(f, buf, sizeof(buf)));

  const uint8_t expected[kPacketSize] = {
      0x01, 0x01, 0x2a, 0x00, 0xd2, 0x02, 0x96, 0x49, 0xe4, 0xea, 0x4d, 0x1f,
      0xdc, 0x6e, 0xfd, 0x07, 0xdf, 0x05, 0x00, 0x00, 0x09, 0x03, 0x1b, 0x10,
      0x7a, 0x08, 0x01, 0xfd, 0x3b,
  };
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, buf, kPacketSize);
}

void test_round_trip() {
  PacketFields f{};
  f.node_id = 7;
  f.seq = 65000;
  f.gps_time = 1700000000;
  f.lat = -52.52001;
  f.lon = -13.40495;
  f.alt_m = 20123.4f;
  f.sats = 12;
  f.fix = kFix3D;
  f.batt_mv = 3712;
  f.temp_c = -18.25f;
  f.flags = kFlagAirborneMode | kFlagRebooted;

  uint8_t buf[kPacketSize];
  TEST_ASSERT_EQUAL(kPacketSize, encodePacket(f, buf, sizeof(buf)));

  Packet p{};
  TEST_ASSERT_TRUE(decodePacket(buf, kPacketSize, &p) == DecodeError::kNone);
  TEST_ASSERT_EQUAL(7, p.node_id);
  TEST_ASSERT_EQUAL(65000, p.seq);
  TEST_ASSERT_EQUAL_UINT32(1700000000, p.gps_time);
  TEST_ASSERT_FLOAT_WITHIN(1e-5, -52.52001, p.lat);
  TEST_ASSERT_FLOAT_WITHIN(1e-5, -13.40495, p.lon);
  TEST_ASSERT_FLOAT_WITHIN(1e-3, 20123.4f, p.alt_m);
  TEST_ASSERT_EQUAL(12, p.sats);
  TEST_ASSERT_EQUAL(kFix3D, p.fix);
  TEST_ASSERT_EQUAL(3712, p.batt_mv);
  TEST_ASSERT_FLOAT_WITHIN(1e-3, -18.25f, p.temp_c);
  TEST_ASSERT_TRUE(p.airborne_mode);
  TEST_ASSERT_TRUE(p.rebooted);
}

void test_bad_crc_rejected() {
  PacketFields f{};
  f.node_id = 1;
  f.seq = 1;
  f.gps_time = 1;
  f.lat = 0;
  f.lon = 0;
  f.alt_m = 0;
  f.sats = 0;
  f.fix = kFixNone;
  f.batt_mv = 0;
  f.temp_c = 0;
  f.flags = 0;

  uint8_t buf[kPacketSize];
  TEST_ASSERT_EQUAL(kPacketSize, encodePacket(f, buf, sizeof(buf)));
  buf[10] ^= 0xFF;

  Packet p{};
  TEST_ASSERT_TRUE(decodePacket(buf, kPacketSize, &p) == DecodeError::kBadCrc);
}

void test_bad_length_rejected() {
  uint8_t buf[kPacketSize] = {0};
  Packet p{};
  TEST_ASSERT_TRUE(decodePacket(buf, kPacketSize - 1, &p) == DecodeError::kBadLength);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_crc16_known_vector);
  RUN_TEST(test_encode_matches_python_reference);
  RUN_TEST(test_round_trip);
  RUN_TEST(test_bad_crc_rejected);
  RUN_TEST(test_bad_length_rejected);
  return UNITY_END();
}
