# BLEeh Arduino library

Receiver and transmitter for the [BLEeh protocol](../PROTOCOL.md): 16 RC
channels, failsafe, telemetry and pairing over standard BLE, so any ESP32 robot
or RC model can be driven by an EdgeTX radio (through the [Nano-bay module](../nanobay/)),
a PC or a phone.

| File | What |
|---|---|
| `src/BLEeh.h` | Includes everything below |
| `src/BLEehRx.{h,cpp}` | Receiver (GATT server): put this in your robot |
| `src/BLEehTx.{h,cpp}` | Transmitter (GATT client): used by the Nano-bay module, or your own TX |
| `src/BLEehProtocol.{h,cpp}` | Portable wire format, no Arduino/ESP-IDF code. Reusable in other stacks |

Requires Arduino-ESP32 3.x and NimBLE-Arduino 2.5+. Works on ESP32, -S3, -C3,
-C6 (anything with BLE; not the ESP32-S2).

> Status: compiles for ESP32-C3/S3; **not yet tested on hardware**.

## Install

* **Arduino IDE:** download `BLEeh-x.y.z.zip` from the GitHub release, then
  *Sketch → Include Library → Add .ZIP Library*. Install **NimBLE-Arduino** from
  the Library Manager. Examples appear under *File → Examples → BLEeh*.
* **PlatformIO:**
  ```ini
  lib_deps =
      BLEeh=https://github.com/Celppu/BLEeh.git#v0.1.0   ; or copy arduino/ into lib/BLEeh
      h2zero/NimBLE-Arduino@^2.5.1
  ```
  Library in a subfolder: until BLEeh is on the PlatformIO registry, copy
  `arduino/` into your project's `lib/BLEeh/`.

## Examples

| Example | What |
|---|---|
| [MinimalReceiver](examples/MinimalReceiver/MinimalReceiver.ino) | Prints channels, sends battery telemetry |
| [HBridgeCar](examples/HBridgeCar/) | Differential-drive car on a dual H-bridge with battery telemetry and arm switch |

Building the examples with PlatformIO (development):
`pio run -d tools/examples_build`.

## Receiver quick start

```cpp
#include <BLEehRx.h>

BLEehRx rx;

void setup() {
    BLEehRxConfig cfg;
    cfg.name = "My Robot";
    cfg.pairButtonPin = 9;             // BOOT on ESP32-C3 (0 on ESP32/S3); triple-click to pair
    cfg.failsafeTimeoutMs = 250;       // signal-loss time
    cfg.failsafeMode = BLEehFailsafe::Preset;
    rx.setFailsafeValue(2, 172);       // e.g. CH3 throttle low on failsafe
    rx.begin(cfg);
}

void loop() {
    rx.update();                       // call often
    if (rx.isFailsafe()) { /* stop motors */ }
    int steering = rx.channelMicros(0);   // 988..1500..2012 us
    float throttle = rx.channelNormalized(1); // -1..1
    rx.setBattery(7400, 0, 0, 80);     // shows on the radio as RxBt etc.
}
```

## Receiver API

| Call | Meaning |
|---|---|
| `begin(config)` | Init NimBLE, create the service, start advertising. The library owns `NimBLEDevice::init()`. |
| `update()` | Pairing button, failsafe, telemetry, LED. Call every loop. |
| `channel(i)` / `channelMicros(i)` / `channelNormalized(i)` | Channel `i` (0..15) as raw CRSF (172..1811), µs (988..2012) or -1..1. Returns failsafe values while in failsafe. |
| `newFrame()` | True once per received RC frame. |
| `isFailsafe()` | Not connected, no frame for `failsafeTimeoutMs`, pairing, or the transmitter lost its input. |
| `onFailsafeChanged` | Callback `void(bool failsafe)`, called from `update()`. |
| `setFailsafeMode(Hold/Preset)`, `setFailsafeValue(i, raw)`, `setFailsafeTimeout(ms)` | Signal-loss behaviour. |
| `linkQuality()`, `rssi()`, `frameAgeMs()` | Link state. |
| `startPairing()`, `clearPairing()`, `isPairing()`, `isBonded()`, `isConnected()` | Pairing control. |
| `setBattery()`, `setGps()`, `setAttitude()`, `setBaroAltitude()`, `setVario()`, `setFlightMode()` | Telemetry sent to the transmitter. Units in [PROTOCOL.md](../PROTOCOL.md#telemetry-notification). |

`BLEehRxConfig` fields:

| Field | Default | Meaning |
|---|---|---|
| `name` | `"BLEeh RX"` | BLE name, max 20 chars |
| `pairButtonPin` | -1 | Pairing button, -1 = none |
| `pairButtonActiveLow` | true | Pressed = LOW (pull-up enabled) |
| `pairClicks`, `pairClickWindowMs` | 3, 3000 | Clicks within the window that open pairing |
| `pairWhenUnbound` | true | Open pairing at boot if nothing is bonded |
| `pairingTimeoutMs` | 120000 | Pairing window length, 0 = until bonded |
| `failsafeTimeoutMs` | 250 | Signal-loss time |
| `failsafeMode` | `Preset` | `Hold` keeps last values, `Preset` uses the failsafe values (default centre) |
| `telemetryPeriodMs` | 100 | Telemetry notification period |
| `txPowerDbm` | 15 | BLE TX power (rounded to chip steps; ESP32-S3/C3 max 20) |
| `statusLedPin`, `statusLedActiveLow` | -1, false | LED: fast blink = pairing, slow = no link, on = linked |

## Transmitter API (`BLEehTx`)

`begin()` (after `NimBLEDevice::init()`), `sendChannels(ch[16], sourceFresh)`
(thread-safe, call per input frame), `startPairing(ms)`, `forgetReceiver()`,
`status()` (connection, capabilities, uplink/downlink LQ and RSSI, battery),
`takeSensors()` (sensor updates to forward). All BLE work runs in its own task:
connect, encrypt, discover, subscribe, and reconnect forever on loss. The bond
is stored in NVS namespace `bleeh_tx`.
