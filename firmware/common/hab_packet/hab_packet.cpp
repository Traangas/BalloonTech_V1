#include "hab_packet.h"

#include <cmath>
#include <cstring>

namespace hab {

uint16_t crc16Ccitt(const uint8_t* data, size_t len, uint16_t init) {
  uint16_t crc = init;
  for (size_t i = 0; i < len; ++i) {
    crc ^= static_cast<uint16_t>(data[i]) << 8;
    for (int bit = 0; bit < 8; ++bit) {
      if (crc & 0x8000) {
        crc = static_cast<uint16_t>((crc << 1) ^ 0x1021);
      } else {
        crc = static_cast<uint16_t>(crc << 1);
      }
    }
  }
  return crc;
}

DecodeError decodePacket(const uint8_t* raw, size_t len, Packet* out) {
  if (len != kPacketSize) {
    return DecodeError::kBadLength;
  }

  RawPacket rp;
  std::memcpy(&rp, raw, sizeof(rp));

  const uint16_t crcCalc = crc16Ccitt(raw, kPacketSize - sizeof(uint16_t));
  if (crcCalc != rp.crc16) {
    return DecodeError::kBadCrc;
  }

  if (rp.version != 1) {
    return DecodeError::kBadVersion;
  }

  out->version = rp.version;
  out->node_id = rp.node_id;
  out->seq = rp.seq;
  out->gps_time = rp.gps_time;
  out->lat = rp.lat_e7 / 1e7;
  out->lon = rp.lon_e7 / 1e7;
  out->alt_m = rp.alt_dm / 10.0f;
  out->sats = rp.sats;
  out->fix = rp.fix;
  out->batt_mv = rp.batt_mv;
  out->temp_c = rp.temp_centi / 100.0f;
  out->flags = rp.flags;
  out->airborne_mode = (rp.flags & kFlagAirborneMode) != 0;
  out->rebooted = (rp.flags & kFlagRebooted) != 0;

  return DecodeError::kNone;
}

size_t encodePacket(const PacketFields& f, uint8_t* out, size_t out_len) {
  if (out_len < kPacketSize) {
    return 0;
  }

  RawPacket rp;
  rp.version = f.version;
  rp.node_id = f.node_id;
  rp.seq = f.seq;
  rp.gps_time = f.gps_time;
  rp.lat_e7 = static_cast<int32_t>(std::lround(f.lat * 1e7));
  rp.lon_e7 = static_cast<int32_t>(std::lround(f.lon * 1e7));
  rp.alt_dm = static_cast<int32_t>(std::lround(f.alt_m * 10.0));
  rp.sats = f.sats;
  rp.fix = f.fix;
  rp.batt_mv = f.batt_mv;
  rp.temp_centi = static_cast<int16_t>(std::lround(f.temp_c * 100.0));
  rp.flags = f.flags;

  std::memcpy(out, &rp, kPacketSize - sizeof(uint16_t));
  rp.crc16 = crc16Ccitt(out, kPacketSize - sizeof(uint16_t));
  std::memcpy(out + (kPacketSize - sizeof(uint16_t)), &rp.crc16, sizeof(uint16_t));

  return kPacketSize;
}

}  // namespace hab
