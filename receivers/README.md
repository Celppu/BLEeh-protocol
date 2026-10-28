# BLEeh receivers

Ready-to-flash receiver firmware: flash a cheap ESP32 board and it behaves like
a normal RC receiver, with outputs a flight controller, servos or ESCs
understand. No coding needed. (To put BLEeh *inside* your own robot code, use
the [library](../arduino/) instead.)

> Status: **planned**. Nothing here is implemented yet; this file is the design.

## Planned outputs

| Output | Use | Notes |
|---|---|---|
| **CRSF** (UART, 420k) | Betaflight / INAV / ArduPilot flight controllers | Channels to the FC, telemetry (battery, GPS, attitude, flight mode) from the FC back to the radio. Same wiring as an ELRS receiver: RX/TX/5V/GND |
| **SBUS** (UART, 100k 8E2 inverted) | Older FCs, servo decoders | Inversion is done in the ESP32 UART, no inverter chip needed |
| **PPM** (one pin) | Old FCs, trainer ports | 8 channels |
| **PWM** (one pin per channel) | Servos and ESCs directly | 50 Hz servo pulses, configurable channel → pin map |

Failsafe follows the library: hold or preset values after the signal-loss
time; SBUS/CRSF also report failsafe to the FC in their own way.

## Planned layout

```
receivers/
├── platformio.ini        # one env per board × output, e.g. xiao_esp32c3_crsf
├── src/                  # one firmware, output selected by build flag or setting
└── boards/               # pin maps per board (XIAO C3/S3/C6, ESP32 dev boards)
```

Releases attach prebuilt `.bin` files per board, flashable from a browser
(ESP Web Tools) without installing anything.

## Open questions

* Settings: build-time only, or changeable afterwards (web page over Wi-Fi,
  BLE settings characteristic, or USB serial)?
* Telemetry from the FC (CRSF) needs the full CRSF parser on the receiver side.
* Latency budget: BLE connection interval (7.5 ms) + CRSF output period, to be
  measured against ELRS.
