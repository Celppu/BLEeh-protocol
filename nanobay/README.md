# BLEeh Nano-bay module

Part of [BLEeh](../README.md). Turn the external module bay of a **RadioMaster Pocket** (or any EdgeTX radio
with a Nano/JR bay set to CRSF) into a **Bluetooth Low Energy transmitter**,
built from a ~$5 XIAO ESP32 board:

* **GAMEPAD mode** – the radio becomes a BLE HID gamepad for a PC (simulators,
  games), plus a 16-channel BLE stream for the included Python visualizer.
* **BLEeh mode** – the radio drives your own robots and RC models over
  **[BLEeh](../PROTOCOL.md)**, a small open RC protocol on standard BLE GATT. Any project that
  already has an ESP32 on board can be the receiver: add the [BLEeh library](../arduino/)
  and you get 16 channels, failsafe and telemetry on the radio screen
  (link quality, RSSI, battery, flight mode...) – no ELRS receiver needed.

BLEeh uses only standard BLE (GATT, bonding, LE 1M PHY), so a Windows/Linux/
macOS PC, an ESP32 or (future) an nRF board can be either end.

```
EdgeTX radio ──CRSF──> Nano-bay module (XIAO ESP32-S3/C3) ──BLE──> PC gamepad
                                                          └──BLE──> BLEeh receiver (your ESP32 robot)
```

## Status

This is a hobby prototype. What has actually been tested on hardware:

| Part | State |
|---|---|
| Bridge firmware, XIAO ESP32-S3 | Built and run: CRSF from the Pocket bay (~250 frames/s), OLED, buttons, menu |
| GAMEPAD mode on Windows 11 | Pairs and bonds; 16-channel GATT stream 0 % loss at 25 Hz |
| BLEeh library + bridge BLEeh mode | **Compiles; not yet tested on hardware** |
| BLEeh receiver examples (C3, S3) | **Compiles; not yet tested on hardware** |
| Bridge firmware, XIAO ESP32-C3 | Not rebuilt since the S3 bring-up |
| Bay EXT_TX/EXT_RX pads as a full-duplex UART | **Not available**: silent in CRSF mode (see [Bay probe](#bay-probe-esp32-s3-research)) |
| nRF52/nRF54 | Future work; the protocol is kept stack-neutral for it |

See [docs/BRINGUP_NOTES.md](docs/BRINGUP_NOTES.md) for the bring-up log and bugs found.

## Folder layout

| Path | What |
|---|---|
| `src/`, `include/`, `platformio.ini` | Module firmware (uses the library from [../arduino](../arduino/)) |
| `python/` | 16-channel gamepad visualizer (`monitor.py`) and its packet decoder |
| `tests/` | Host tests: button debouncer (C++), gamepad packet format |
| `docs/` | Bring-up notes, bay-pad findings |

## Quick start

1. **Build the module**: wire a XIAO ESP32-S3 to the bay (see [Hardware](#hardware)).
2. **Flash it**: `pio run -d nanobay -e xiao_esp32s3 -t upload` (from the repository root).
3. **Radio**: model settings → Internal RF **OFF**, External RF **CRSF**.
4. **PC gamepad**: Windows Settings → Bluetooth → Add device → *Pocket BLE Bridge*.
5. **Your robot**: flash a [library example](../arduino/examples/) (or add the library to your project),
   triple-click its BOOT button, then on the module choose **PAIR RECEIVER**.

## Hardware

```
Pocket Nano-bay CRSF signal ---[100 ohm]--- XIAO D7
Pocket GND -------------------------------- XIAO GND
Pocket switched bay power --- regulator / isolated feed ---> XIAO 5V
XIAO BOOT button ------------------------ PC gamepad pairing (triple-click)

OLED SSD1306 128x32 I2C:  SDA - D4, SCL - D5, VCC - 3.3 V, GND
Buttons to GND (internal pull-ups):  UP - D1, DOWN - D3, SELECT - D10
```

| Signal | XIAO pin | ESP32-C3 | ESP32-S3 | ESP32-C6* | nRF54L15* |
|---|---|---|---|---|---|
| CRSF data | D7 | GPIO20 | GPIO44 | GPIO17 | P2.07 |
| OLED SDA | D4 | GPIO6 | GPIO5 | GPIO22 | P1.10 |
| OLED SCL | D5 | GPIO7 | GPIO6 | GPIO23 | P1.11 |
| UP | D1 | GPIO3 | GPIO2 | GPIO1 | P1.05 |
| DOWN | D3 | GPIO5 | GPIO4 | GPIO21 | P1.07 |
| SELECT | D10 | GPIO10 | GPIO9 | GPIO18 | P2.02 |
| Bay PPM pad probe ("EXT_TX", input only) | D0 | GPIO2 (strap, **do not wire**) | GPIO1 | verify | P1.04 |
| Bay heartbeat pad probe ("EXT_RX", input only) | D8 | GPIO8 (strap, **do not wire**) | GPIO7 | verify | P2.01 |

`*` Header-compatible only; no firmware for C6 / nRF54L15 yet.

**Power and wiring warnings**

* XIAO pins are 3.3 V only. Measure the bay signal level and GND reference before connecting D7.
* **Never** connect the Pocket's battery voltage directly to XIAO 5V/3V3. Use a regulator, and isolate the bay feed when USB is plugged in.
* Check the Nano-bay contact orientation on your own radio; this project does not include a bay pinout.
* Do not hold BOOT while powering up (that enters the ESP32 bootloader).
* XIAO ESP32-S3 Sense: D10 may conflict with the microSD/SPI option.

The OLED is probed at I2C 0x3C, then 0x3D; the firmware runs without it. Other
controllers of the same size (e.g. SH1106) need a different `U8G2_...`
constructor in `src/ui.cpp`. Keep the bus at 400 kHz (1 MHz broke the test module).

## Radio setup (EdgeTX)

1. Edit the model: **Internal RF = OFF**, **External RF = CRSF** (this powers the bay).
2. Baud 400k/420k (the firmware uses 420000).
3. Set up channels 1–16 in Mixes as usual.

The bay line is inverted half-duplex CRSF, handled by [CRSFDude](https://github.com/rngtng/CRSFDude)
(including the EdgeTX Device Ping handshake). Don't replace it with a plain `HardwareSerial`.

## Menu and controls

SELECT opens the menu, UP/DOWN move, SELECT chooses. Settings stay open so
repeated SELECT cycles the value; all settings are saved in NVS.

| Item | Action |
|---|---|
| `GAMEPAD` | BLE HID gamepad + 16-channel GATT stream for the PC |
| `BLEeh` | Drive the bonded BLEeh receiver; telemetry goes to the radio |
| `PAIR RECEIVER` | Bond the nearest BLEeh receiver that is in pairing mode |
| `PAIR PC GAMEPAD` | Same as BOOT x3: forget PC bonds and advertise for pairing |
| `RATE` | Gamepad report rate: 25 Hz / 66 Hz SAFE / 125 Hz MAX |
| `TX PWR` | BLE transmit power: 0 / 9 / **15 (default)** / 20 dBm |
| `BAY PROBE (S3)` | Passive edge counter + UART sniffer on D0/D8 (research only, see below) |

OLED screens: in GAMEPAD a `B` icon shows BLE state (plain = idle, blinking =
pairing, inverted box = connected). In BLEeh mode the screen shows link state,
uplink LQ and RSSI, receiver battery and telemetry LQ.

## GAMEPAD mode (PC)

* Pair: Windows Settings → Bluetooth & devices → Add device → **Pocket BLE Bridge** (Just Works, no PIN).
  Triple-click BOOT (or `PAIR PC GAMEPAD`) to re-pair; forget the old device on the PC first.
* `joy.cpl` shows 8 axes (CH1–CH8) and buttons 1–8 (CH9–CH16 above raw 1200).
* All 16 raw channels are also sent over a custom GATT characteristic. Visualizer:

```
cd nanobay/python
python -m pip install -r requirements.txt
python monitor.py --demo                 # GUI without hardware
python monitor.py --address <BLE address> # live, shows the received Hz
```

Use one BLE app at a time on Windows; two bleak clients to the same device fail.

**Gamepad GATT format** – service `c09dfb45-9fb3-4bb8-bf25-bd3a77d21b01`,
characteristic `...21b02` (notify), 20 bytes little-endian:

| Bytes | Type | Meaning |
|---|---|---|
| 0..1 | u16 | sequence (both banks share it) |
| 2 | u8 | bank 0 = CH1–8, 1 = CH9–16 |
| 3 | u8 | bit0 CRSF fresh, bit1 pairing |
| 4..19 | u16[8] | raw CRSF values (172..992..1811) |

## BLEeh mode (your robots)

1. Flash a receiver: `pio run -d tools/examples_build -e HBridgeCar_c3 -t upload`
   (see [HBridgeCar](../arduino/examples/HBridgeCar/)), or add the [library](../arduino/) to your own project.
2. Triple-click the receiver's BOOT button (a fresh receiver starts in pairing mode by itself).
3. On the module: **PAIR RECEIVER**. It bonds the strongest receiver in pairing mode, saves it and switches to BLEeh mode.
4. From now on the bridge reconnects to that receiver automatically after resets or signal loss.

The bridge sends every CRSF frame to the receiver (BLE interval 7.5 ms) and
forwards receiver telemetry to EdgeTX as CRSF link statistics, battery, GPS,
attitude, altitude, vario and flight mode sensors (EdgeTX: Telemetry → Discover new sensors).

No receiver yet? Test from a PC: `python python/bleeh_tx.py --scan` (from the repository root).

## Bay probe (ESP32-S3, research)

**Result: the extra bay pads are not a CRSF UART.** A Zorro adapter photo labels
two more contacts EXT_TX/EXT_RX. In EdgeTX these are the module bay's **PPM**
(MCU PC6) and **heartbeat** (PC7) pins, which form a full-duplex UART
(USART6). EdgeTX only uses it for PPM, Multi-protocol, FrSky PXX1/PXX2, SBUS and
AFHDS3. **External CRSF always runs half duplex on the single S.Port wire (D7)**,
so with CRSF selected both pads stay idle. Tested with the probe below and an
oscilloscope: no activity. Details and EdgeTX source references are in
[docs/BRINGUP_NOTES.md](docs/BRINGUP_NOTES.md#bay-ext_tx--ext_rx-pads-2026-10-28).

For ports to chips without UART inversion or single-wire mode (e.g. nRF54L15),
CRSF still has to come from the D7 wire through an inverting buffer; see the
notes for the receive-only and full half-duplex options.

`BAY PROBE` (S3 only) stays in the firmware for research: connect pads to D0/D8
through 1 kΩ, only after measuring them at ≤ 3.3 V. The pins stay inputs. The
screen shows level and edges per second. Over USB serial, a UART sniffer cycles
both pins through 420000/115200 baud, normal/inverted, and reports CRC-valid
CRSF frames and whether CH1 matches D7. On the C3 these pins are boot straps, so
probe mode is refused.

## Build

PlatformIO (VS Code extension or CLI):

From the repository root:

```
pio run -d nanobay -e xiao_esp32s3 -t upload   # module firmware
python -m pytest -q                            # all host tests (needs g++ for the C++ parts)
```

Windows tip: set `PYTHONIOENCODING=utf-8` if esptool's progress output crashes the console.

## Safety and limitations

* Pairing is BLE **Just Works**: encrypted and bonded, but not protected against
  an active man-in-the-middle *during* pairing. Pair in a quiet place; receivers
  only accept new bonds inside their pairing window.
* BLE is not a long-range RC link. Range-test at your TX power and keep a
  hardware/software failsafe on anything that can hurt someone.
* Receiver failsafe defaults to "all channels centre after 250 ms". Configure
  `failsafeTimeoutMs`, `failsafeMode` and `setFailsafeValue()` for your model
  (e.g. throttle low for planes).
* BLEeh has not been hardware-tested yet; test with wheels off the ground.

## References

* [CRSFDude](https://github.com/rngtng/CRSFDude) – module-bay CRSF for ESP32
* [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino) · [U8g2](https://github.com/olikraus/u8g2) · [bleak](https://github.com/hbldh/bleak)
* Seeed XIAO pinouts: [ESP32-C3](https://wiki.seeedstudio.com/XIAO_ESP32C3_Getting_Started/), [ESP32-S3](https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/), [nRF54L15](https://wiki.seeedstudio.com/xiao_nrf54l15_sense_getting_started/)
