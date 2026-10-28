# BLEeh

**An open RC link on plain Bluetooth Low Energy.** Drive robots, cars and
models from your EdgeTX radio, a PC or (soon) a phone — with 16 channels,
failsafe and telemetry, and no dedicated RC receiver. If your project already
has an ESP32 on board, it already has a BLEeh receiver.

```
EdgeTX radio ──CRSF──> BLEeh Nano-bay module ──BLE──┐
PC (Python) ─────────────────────────────────BLE──┼──> your robot (ESP32 + BLEeh library)
Android app (planned) ───────────────────────BLE──┘     or a BLEeh receiver board (planned)
```

BLEeh uses only standard BLE (GATT, bonding, LE 1M PHY), so ESP32, nRF,
Windows/Linux/macOS and Android can all be either end, and they can be mixed.

> **Status: hobby prototype.** The Nano-bay module has been tested on hardware
> (RadioMaster Pocket + XIAO ESP32-S3) as a PC gamepad. The BLEeh link itself
> compiles and passes host tests but has **not yet been tested on hardware**.

## What's in this repository

| Folder | What | Status |
|---|---|---|
| [PROTOCOL.md](PROTOCOL.md) | The BLEeh v1 specification | Draft |
| [test-vectors/](test-vectors/) | Reference frames every implementation must match | Used by the tests |
| [arduino/](arduino/) | **Library** for Arduino IDE / PlatformIO: receiver + transmitter, examples | Compiles, untested on hardware |
| [nanobay/](nanobay/) | Firmware for a Nano-bay module (RadioMaster Pocket, any EdgeTX radio with CRSF): BLEeh transmitter + PC gamepad | Gamepad mode tested |
| [receivers/](receivers/) | Ready-to-flash receiver firmware with CRSF / SBUS / PPM / PWM outputs | Planned |
| [python/](python/) | Python implementation + PC transmitter / scanner (`bleeh_tx.py`, bleak) | Passes vectors; not yet run against a receiver |
| [tests/](tests/) | Protocol tests: C++ and Python against the test vectors | Passing |
| [tools/examples_build/](tools/examples_build/) | PlatformIO project that builds the library examples | Builds C3 + S3 |
| `android/` | Phone app driving BLEeh receivers | Planned |
| `zephyr/` | nRF52/nRF54 port | Planned |

## Quick start

* **Put BLEeh in your robot:** install the [library](arduino/) and start from
  [MinimalReceiver](arduino/examples/MinimalReceiver/MinimalReceiver.ino) or
  [HBridgeCar](arduino/examples/HBridgeCar/).
* **Build the radio module:** see [nanobay/](nanobay/).
* **No radio yet?** Drive a receiver from a PC:
  ```
  pip install -r python/requirements.txt
  python python/bleeh_tx.py --scan
  python python/bleeh_tx.py --pair
  ```

## Development

```
python -m pytest -q                            # all host tests (g++ needed for the C++ parts)
pio run -d nanobay                             # Nano-bay firmware (ESP32-S3, ESP32-C3)
pio run -d tools/examples_build       # library examples
```

Everything builds from this one repository: `nanobay/` and the examples link
the library with `symlink://` paths, so they always use the library code from
the same commit.

## Safety

BLE is not a long-range link, and Just Works pairing is not protected against
an attacker present *during* pairing. Range-test at your TX power, configure the
failsafe for your model, and keep wheels off the ground / props off for first tests.

## License

[GPL-3.0-or-later](LICENSE). You may use, modify and sell BLEeh, but firmware
or software you distribute that includes BLEeh code must also be released
under the GPL with its source code. Building your own *independent*
implementation of the protocol from [PROTOCOL.md](PROTOCOL.md) is not
restricted.

Third-party libraries and their licenses: [THIRD_PARTY.md](THIRD_PARTY.md).
