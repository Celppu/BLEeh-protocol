# BLEeh protocol, version 1

BLEeh is an RC link on **standard BLE GATT**: 16 channels, failsafe, telemetry
and pairing. Anything with a normal BLE stack can be either end: ESP32, nRF,
Windows/Linux/macOS, Android.

> Status: v1 draft. Implemented in C++ ([arduino/](arduino/)) and Python
> ([python/bleeh.py](python/bleeh.py)); both pass [test-vectors/v1.json](test-vectors/v1.json).
> Not yet tested on hardware.

## Roles and link

* **Receiver** = GATT server / peripheral (the robot). **Transmitter** = GATT client / central
  (Nano-bay module, PC, phone).
* LE 1M PHY. The transmitter chooses the connection interval; the ESP32
  transmitter uses 7.5 ms with a 500 ms supervision timeout.
* ATT MTU must be ≥ 28 (all modern stacks negotiate more; the ESP32 code asks for 64).
* One bonded transmitter per receiver.
* All multi-byte fields are little-endian.

## GATT

Base UUID `xxxxxxxx-55eb-4435-bb6e-270656fee0da` (random; the prefix `b1ee` reads "BLEE").

| UUID | Name | Properties | Content |
|---|---|---|---|
| `b1ee0001-55eb-4435-bb6e-270656fee0da` | BLEeh service | | |
| `b1ee0002-55eb-4435-bb6e-270656fee0da` | RC | Write Without Response, **encryption required** | RC frame |
| `b1ee0003-55eb-4435-bb6e-270656fee0da` | Telemetry | Notify (+ Read), encryption required | Telemetry notification |
| `b1ee0004-55eb-4435-bb6e-270656fee0da` | Capabilities | Read | Capabilities record |

## RC frame (25 bytes)

| Byte | Type | Meaning |
|---|---|---|
| 0 | u8 | flags: bit0 `SOURCE_FRESH` = transmitter has live input data; others 0 |
| 1..2 | u16 | sequence, +1 per frame (wraps) |
| 3..24 | 22 bytes | 16 channels × 11 bits, LSB first, CRSF bit order (CH1 in the lowest bits) |

Channel values are CRSF units: 172 = −100 %, 992 = centre, 1811 = +100 %
(µs = 1500 + (v − 992) × 5/8). Receivers must accept longer writes and ignore
the extra bytes (future fields). The transmitter sends one frame per input
frame; when its input stops (e.g. the radio is switched off) it sends frames
with `SOURCE_FRESH` = 0 every 100 ms so the receiver fails safe immediately.

## Telemetry notification

```
[u8 telemetry sequence] { [u8 type][u8 length][payload] } ...
```

The sequence increments per notification (downlink LQ). Unknown types must be
skipped using `length`, so new record types never break old transmitters.
A notification holds as many records as fit in ATT MTU − 3; the rest go next time.

| Type | Name | Payload |
|---|---|---|
| 0x01 | Link | u16 last RC sequence, u8 uplink LQ %, i8 uplink RSSI dBm, u8 flags (bit0 failsafe active) |
| 0x02 | Battery | u16 mV, u16 current 0.1 A, u24 capacity mAh, u8 remaining % |
| 0x03 | GPS | i32 lat ×1e7, i32 lon ×1e7, u16 groundspeed, u16 heading ×100, u16 altitude m, u8 satellites |
| 0x04 | Attitude | i16 pitch, roll, yaw (rad ×10000) |
| 0x05 | Baro altitude | i32 cm |
| 0x06 | Vario | i16 cm/s |
| 0x07 | Flight mode | UTF-8 text, max 15 bytes, no terminator |
| 0x80–0xFF | Application | Free for your own data; transmitters ignore types they don't know |

The ESP32 receiver sends Link every `telemetryPeriodMs` and each sensor when it
changes plus once per second. The Nano-bay transmitter forwards them to EdgeTX as
CRSF link statistics (RQly/1RSS/TQly/TRSS), battery, GPS, attitude, baro/vario
and flight mode.

## Capabilities (read after connecting)

| Byte | Type | Meaning |
|---|---|---|
| 0 | u8 | protocol version (1) |
| 1 | u8 | channel count (16) |
| 2..5 | u32 | feature bits |
| 6..7 | u16 | telemetry types the receiver may send (bit n = type n) |
| 8.. | UTF-8 | receiver name, max 20 bytes |

| Feature bit | Meaning |
|---|---|
| 0 | honours `SOURCE_FRESH` |
| 1 | sends Link telemetry |
| 2 | *reserved*: command characteristic |
| 3 | *reserved*: BLE Channel Sounding ranging (nRF54) |
| 4..31 | reserved, 0 |

A transmitter must ignore unknown feature bits. New capabilities (commands,
Channel Sounding) are announced here and use new characteristics, so a v1
transmitter and a newer receiver (or the reverse) keep working together.

## Advertising and pairing

* Advertising data: flags, the 128-bit service UUID, and manufacturer-specific
  data `FF FF 'B' 'E' <version> <flags>`: company ID 0xFFFF, which the Bluetooth
  SIG reserves for testing and is not assigned to anyone; flags bit0 = **pairing
  window open**. The name is in the scan response.
* Pairing: standard BLE bonding, LE Secure Connections, Just Works.
  1. The receiver opens a pairing window (button, `startPairing()`, or first boot)
     and advertises with the pairing flag.
  2. The transmitter scans, picks the strongest receiver with the flag, connects
     and bonds; both sides store the bond.
  3. The receiver deletes any other bonds and clears the pairing flag.
* Outside the pairing window the receiver disconnects any central that creates a
  new bond, and drops connections that stay unencrypted for 3 s.
* Reconnection: the transmitter connects directly to the stored address in a
  loop (no scan); the receiver advertises every 20–30 ms whenever it is not connected.

Security note: Just Works pairing is encrypted but not protected against an
active man-in-the-middle *during* pairing. Pair in a quiet place.

## Failsafe

The receiver is in failsafe when it is not connected, in its pairing window, has
had no valid RC frame for its signal-loss time, or the last frame had
`SOURCE_FRESH` = 0. Behaviour is configurable (hold or preset values) and is
reported to the transmitter in the Link record.

## Test vectors

[test-vectors/v1.json](test-vectors/v1.json) has one reference frame of each
kind with its decoded values. Every implementation must encode the inputs to
exactly those bytes and decode them back. `tests/test_protocol.py` checks the
C++ and Python implementations; new implementations (Kotlin, nRF) should copy
the file into their own tests.

## Versioning and extensions

* Never change a v1 byte layout. Additions go through new telemetry types,
  feature bits and new characteristics, which old peers ignore.
* A breaking change would be protocol version 2, announced in the capabilities
  record and the advertising data.

## Porting (nRF, Android, others)

Implement the GATT table above with your stack. `arduino/src/BLEehProtocol.{h,cpp}`
is plain C++11 with no platform dependencies and can be compiled as-is (e.g.
in Zephyr/nRF Connect SDK). Mixing transmitters and receivers from different
stacks depends only on this spec.
