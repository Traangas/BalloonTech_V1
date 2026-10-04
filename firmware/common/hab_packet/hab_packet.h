// HAB Phase 0 packet format v1 - 29-byte binary telemetry packet.
//
// Mirrors bridge/packet.py byte-for-byte: same struct layout, same
// CRC-16/CCITT-FALSE algorithm, same field scaling. Keep the two in sync -
// this is the wire contract between tracker/receiver firmware and the bridge.
//
// Layout (little-endian, packed, no padding):
//   0  version   uint8
//   1  node_id   uint8
//   2  seq       uint16
//   4  gps_time  uint32   unix seconds
//   8  lat       int32    degrees x 1e7
//   12 lon       int32    degrees x 1e7
//   16 alt       int32    decimetres above MSL
//   20 sats      uint8
//   21 fix       uint8    0 none, 2 = 2D, 3 = 3D
//   22 batt_mv   uint16
//   24 temp_int  int16    C x 100
//   26 flags     uint8    bit0 airborne mode confirmed, bit1 rebooted
//   27 crc16     uint16   CRC-16/CCITT-FALSE over bytes 0..26
#pragma once

#include <cstddef>
#include <cstdint>

namespace hab {

constexpr size_t kPacketSize = 29;

constexpr uint8_t kFlagAirborneMode = 1 << 0;
constexpr uint8_t kFlagRebooted = 1 << 1;

constexpr uint8_t kFixNone = 0;
constexpr uint8_t kFix2D = 2;
constexpr uint8_t kFix3D = 3;

#pragma pack(push, 1)
struct RawPacket {
  uint8_t version;
  uint8_t node_id;
  uint16_t seq;
  uint32_t gps_time;
  int32_t lat_e7;
  int32_t lon_e7;
  int32_t alt_dm;
  uint8_t sats;
  uint8_t fix;
  uint16_t batt_mv;
  int16_t temp_centi;
  uint8_t flags;
  uint16_t crc16;
};
#pragma pack(pop)

static_assert(sizeof(RawPacket) == kPacketSize, "RawPacket must be exactly 29 bytes");

// Decoded, human-scaled view of a packet (mirrors packet.py's decode_packet dict).
struct Packet {
  uint8_t version;
  uint8_t node_id;
  uint16_t seq;
  uint32_t gps_time;
  double lat;
  double lon;
  float alt_m;
  uint8_t sats;
  uint8_t fix;
  uint16_t batt_mv;
  float temp_c;
  uint8_t flags;
  bool airborne_mode;
  bool rebooted;
};

// Fields needed to build a packet (mirrors packet.py's encode_packet kwargs).
struct PacketFields {
  uint8_t node_id;
  uint16_t seq;
  uint32_t gps_time;
  double lat;
  double lon;
  float alt_m;
  uint8_t sats;
  uint8_t fix;
  uint16_t batt_mv;
  float temp_c;
  uint8_t flags;
  uint8_t version = 1;
};

enum class DecodeError {
  kNone,
  kBadLength,
  kBadCrc,
  kBadVersion,
};

// CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflect, xorout 0).
uint16_t crc16Ccitt(const uint8_t* data, size_t len, uint16_t init = 0xFFFF);

// Decodes `len` bytes at `raw` into `out`. Returns kNone on success.
DecodeError decodePacket(const uint8_t* raw, size_t len, Packet* out);

// Encodes `fields` into the 29-byte buffer `out` (must be >= kPacketSize).
// Returns the number of bytes written (kPacketSize) on success, 0 on failure.
size_t encodePacket(const PacketFields& fields, uint8_t* out, size_t out_len);

}  // namespace hab
