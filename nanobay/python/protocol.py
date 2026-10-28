"""Raw BLE channel GATT format for Pocket BLE Bridge.

A single notification (20 bytes) is <HBB8H, little endian:
  sequence: uint16 (wraps)
  bank:     uint8 (0=CH1..8, 1=CH9..16)
  flags:    uint8 (bit0 fresh, bit1 pairing)
  values:   eight uint16 CRSF raw values

Two bank packets with equal sequence reconstruct all 16 channels.
"""
from dataclasses import dataclass
import struct

DEVICE_NAME = "Pocket BLE Bridge"
SERVICE_UUID = "c09dfb45-9fb3-4bb8-bf25-bd3a77d21b01"
CHAR_UUID = "c09dfb45-9fb3-4bb8-bf25-bd3a77d21b02"
PACKET = struct.Struct("<HBB8H")
CRSF_MIN, CRSF_MID, CRSF_MAX = 172, 992, 1811


@dataclass(frozen=True)
class ChannelFrame:
    sequence: int
    channels: tuple[int, ...]
    fresh: bool
    pairing: bool


class ChannelAssembler:
    def __init__(self):
        self._pending = {}
        self.last_seq = None

    def feed(self, payload: bytes):
        if len(payload) != PACKET.size:
            raise ValueError(f"Expected {PACKET.size} bytes, got {len(payload)}")
        seq, bank, flags, *channels = PACKET.unpack(payload)
        if bank not in (0, 1):
            raise ValueError(f"Invalid bank {bank}")
        entry = self._pending.setdefault(seq, {})
        entry[bank] = (tuple(channels), flags)
        # Cap in-flight assemblies; BLE delivery can be interrupted.
        if len(self._pending) > 8:
            for old_seq in list(self._pending)[:-8]:
                del self._pending[old_seq]
        if 0 in entry and 1 in entry:
            first, flag0 = entry[0]
            second, flag1 = entry[1]
            del self._pending[seq]
            self.last_seq = seq
            return ChannelFrame(seq, first + second,
                                bool((flag0 & flag1) & 1),
                                bool((flag0 | flag1) & 2))
        return None


def normalized(value: int) -> float:
    return max(-1., min(1., (value - CRSF_MID) /
                            (CRSF_MAX - CRSF_MID if value >= CRSF_MID
                             else CRSF_MID - CRSF_MIN)))
