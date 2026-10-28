"""Python implementation of the BLEeh v1 BLE protocol.

Mirrors arduino/src/BLEehProtocol.{h,cpp}; spec in PROTOCOL.md, checked
against test-vectors/v1.json by tests/test_protocol.py.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field
from typing import Optional

PROTOCOL_VERSION = 1
NUM_CHANNELS = 16
PACKED_CHANNEL_BYTES = 22

SERVICE_UUID = "b1ee0001-55eb-4435-bb6e-270656fee0da"
RC_UUID = "b1ee0002-55eb-4435-bb6e-270656fee0da"
TELEMETRY_UUID = "b1ee0003-55eb-4435-bb6e-270656fee0da"
CAPS_UUID = "b1ee0004-55eb-4435-bb6e-270656fee0da"

ADV_COMPANY_ID = 0xFFFF
ADV_FLAG_PAIRING = 0x01

CHANNEL_MIN, CHANNEL_MID, CHANNEL_MAX = 172, 992, 1811
RC_FLAG_SOURCE_FRESH = 0x01
RC_FRAME_LEN = 1 + 2 + PACKED_CHANNEL_BYTES

TELEM_LINK, TELEM_BATTERY, TELEM_GPS, TELEM_ATTITUDE = 0x01, 0x02, 0x03, 0x04
TELEM_BARO_ALTITUDE, TELEM_VARIO, TELEM_FLIGHT_MODE = 0x05, 0x06, 0x07
LINK_FLAG_FAILSAFE = 0x01

FEATURE_FAILSAFE_FLAG = 1 << 0
FEATURE_LINK_TELEMETRY = 1 << 1
FEATURE_COMMANDS = 1 << 2
FEATURE_CHANNEL_SOUNDING = 1 << 3


def pack_channels(channels) -> bytes:
    if len(channels) != NUM_CHANNELS:
        raise ValueError("need 16 channels")
    acc = 0
    for i, ch in enumerate(channels):
        acc |= (int(ch) & 0x7FF) << (11 * i)
    return acc.to_bytes(PACKED_CHANNEL_BYTES, "little")


def unpack_channels(data: bytes) -> tuple:
    acc = int.from_bytes(data[:PACKED_CHANNEL_BYTES], "little")
    return tuple((acc >> (11 * i)) & 0x7FF for i in range(NUM_CHANNELS))


def encode_rc(channels, sequence: int, flags: int = RC_FLAG_SOURCE_FRESH) -> bytes:
    return struct.pack("<BH", flags, sequence & 0xFFFF) + pack_channels(channels)


def decode_rc(data: bytes):
    if len(data) < RC_FRAME_LEN:
        raise ValueError("RC frame too short")
    flags, seq = struct.unpack_from("<BH", data)
    return flags, seq, unpack_channels(data[3:])


@dataclass
class Telemetry:
    sequence: int = 0
    link: Optional[dict] = None
    battery: Optional[dict] = None
    gps: Optional[dict] = None
    attitude: Optional[dict] = None
    baro_altitude_cm: Optional[int] = None
    vario_cms: Optional[int] = None
    flight_mode: Optional[str] = None
    unknown: dict = field(default_factory=dict)


def decode_telemetry(data: bytes) -> Telemetry:
    if not data:
        raise ValueError("empty telemetry")
    t = Telemetry(sequence=data[0])
    i = 1
    while i < len(data):
        if i + 2 > len(data):
            raise ValueError("truncated record header")
        typ, n = data[i], data[i + 1]
        p = data[i + 2:i + 2 + n]
        if len(p) != n:
            raise ValueError("truncated record")
        if typ == TELEM_LINK and n >= 5:
            ack, lq, rssi, flags = struct.unpack_from("<HBbB", p)
            t.link = dict(ack_sequence=ack, uplink_lq=lq, uplink_rssi=rssi,
                          failsafe=bool(flags & LINK_FLAG_FAILSAFE))
        elif typ == TELEM_BATTERY and n >= 8:
            mv, cur = struct.unpack_from("<HH", p)
            t.battery = dict(millivolts=mv, current_da=cur,
                             capacity_mah=int.from_bytes(p[4:7], "little"),
                             remaining_pct=p[7])
        elif typ == TELEM_GPS and n >= 15:
            lat, lon, spd, hdg, alt, sats = struct.unpack_from("<iiHHHB", p)
            t.gps = dict(latitude=lat, longitude=lon, groundspeed=spd,
                         heading=hdg, altitude=alt, satellites=sats)
        elif typ == TELEM_ATTITUDE and n >= 6:
            pitch, roll, yaw = struct.unpack_from("<hhh", p)
            t.attitude = dict(pitch=pitch, roll=roll, yaw=yaw)
        elif typ == TELEM_BARO_ALTITUDE and n >= 4:
            t.baro_altitude_cm = struct.unpack_from("<i", p)[0]
        elif typ == TELEM_VARIO and n >= 2:
            t.vario_cms = struct.unpack_from("<h", p)[0]
        elif typ == TELEM_FLIGHT_MODE:
            t.flight_mode = p[:15].decode("utf-8", "replace")
        else:
            t.unknown[typ] = bytes(p)
        i += 2 + n
    return t


@dataclass
class Capabilities:
    protocol_version: int
    channel_count: int
    features: int
    telemetry_types: int
    name: str


def decode_capabilities(data: bytes) -> Capabilities:
    if len(data) < 8:
        raise ValueError("capabilities too short")
    ver, chans, features, types = struct.unpack_from("<BBIH", data)
    return Capabilities(ver, chans, features, types,
                        data[8:28].decode("utf-8", "replace"))


def decode_adv_manufacturer(company_id: int, payload: bytes):
    """Bleak strips the company id; returns (version, flags) or None."""
    if company_id != ADV_COMPANY_ID or len(payload) < 4 or payload[:2] != b"BE":
        return None
    return payload[2], payload[3]
