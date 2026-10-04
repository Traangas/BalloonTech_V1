"""HAB Phase 0 packet format v1 - 29-byte binary telemetry packet.

Layout per the spec (little-endian, packed, no struct padding):
  0  version   uint8
  1  node_id   uint8
  2  seq       uint16
  4  gps_time  uint32   unix seconds
  8  lat       int32    degrees x 1e7
  12 lon       int32    degrees x 1e7
  16 alt       int32    decimetres above MSL
  20 sats      uint8
  21 fix       uint8    0 none, 2 = 2D, 3 = 3D
  22 batt_mv   uint16
  24 temp_int  int16    C x 100
  26 flags     uint8    bit0 airborne mode confirmed, bit1 rebooted
  27 crc16     uint16   CRC-16/CCITT-FALSE over bytes 0..26
"""
from __future__ import annotations

import struct

_BODY_FORMAT = "<BBHIiiiBBHhB"  # bytes 0..26 (27 bytes), everything but the CRC
_FULL_FORMAT = "<BBHIiiiBBHhBH"  # full 29-byte packet

PACKET_SIZE = struct.calcsize(_FULL_FORMAT)
assert PACKET_SIZE == 29

FLAG_AIRBORNE_MODE = 1 << 0
FLAG_REBOOTED = 1 << 1

FIX_NONE = 0
FIX_2D = 2
FIX_3D = 3


class PacketError(ValueError):
    """Raised when a raw packet fails length, CRC, or version checks."""


def crc16_ccitt(data: bytes, init: int = 0xFFFF) -> int:
    """CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflect, xorout 0)."""
    crc = init
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def decode_packet(raw: bytes) -> dict:
    if len(raw) != PACKET_SIZE:
        raise PacketError(f"expected {PACKET_SIZE} bytes, got {len(raw)}")

    (version, node_id, seq, gps_time, lat_e7, lon_e7, alt_dm,
     sats, fix, batt_mv, temp_centi, flags, crc_rx) = struct.unpack(_FULL_FORMAT, raw)

    crc_calc = crc16_ccitt(raw[:27])
    if crc_calc != crc_rx:
        raise PacketError(f"CRC mismatch: got 0x{crc_rx:04x}, expected 0x{crc_calc:04x}")

    if version != 1:
        raise PacketError(f"unsupported packet version {version}")

    return {
        "version": version,
        "node_id": node_id,
        "seq": seq,
        "gps_time": gps_time,
        "lat": lat_e7 / 1e7,
        "lon": lon_e7 / 1e7,
        "alt_m": alt_dm / 10.0,
        "sats": sats,
        "fix": fix,
        "batt_mv": batt_mv,
        "temp_c": temp_centi / 100.0,
        "flags": flags,
        "airborne_mode": bool(flags & FLAG_AIRBORNE_MODE),
        "rebooted": bool(flags & FLAG_REBOOTED),
    }


def encode_packet(*, node_id: int, seq: int, gps_time: int, lat: float, lon: float,
                   alt_m: float, sats: int, fix: int, batt_mv: int, temp_c: float,
                   flags: int, version: int = 1) -> bytes:
    body = struct.pack(
        _BODY_FORMAT,
        version, node_id, seq & 0xFFFF, gps_time,
        round(lat * 1e7), round(lon * 1e7), round(alt_m * 10),
        sats, fix, batt_mv, round(temp_c * 100), flags,
    )
    crc = crc16_ccitt(body)
    return body + struct.pack("<H", crc)
