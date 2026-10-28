# ESP32-S3 bring-up notes (2026-10-27)

First hardware session on a **XIAO ESP32-S3** wired to the RadioMaster Pocket
Nano bay (External RF = CRSF), SSD1306 128x32 OLED, and the three menu buttons.
PC side: Windows 11, `python/monitor.py` with bleak 2.1.1.

## Verified on hardware

| Item | Result |
|---|---|
| `pio run -e xiao_esp32s3` | Builds (Arduino core 3.3.11 / IDF 5.5, NimBLE-Arduino 2.5.2, U8g2 2.36.18, CRSFDude d8063b1) |
| OLED | Found at I2C **0x3C** |
| Buttons UP/DOWN/SELECT | Read LOW when pressed, HIGH when released; menu works |
| CRSF on D7 | ~250 frames/s, CH1/CH2 track sticks over full 173..1811 range; ran 2 min without drops |
| BLE pairing (Windows) | Bonds as "Pocket BLE Bridge", reconnects automatically after reboot |
| GATT 16-channel stream | 50 packets/s, 0 missing sequences, both banks every frame, no serial reader attached |

Not yet verified: HID axes/buttons in `joy.cpl`, survival of a real EdgeTX
Device Ping (`replies=` stayed 0), the C3 build after these changes, and
BLEeh mode (called LIGHT LINK / LightLink at the time). BAY PROBE was tested
later, see below.

## Issues found and fixed

### 1. CRSFDude did not compile on Arduino core 3.3 (IDF 5.5)
`gpio_matrix_in` / `gpio_matrix_out` are still in the ROM, but their header is
no longer pulled in transitively. Fixed in `platformio.ini` by force-including
`esp32s3/rom/gpio.h` (and `esp32c3/rom/gpio.h` for the C3 env), leaving the
upstream library untouched.

### 2. Radio CRSF stream stopped after a few seconds
EdgeTX halts CRSF output (until the radio is power-cycled) if the module's
reply to a Device Ping is late or garbled. `crsf.update()` ran in `loop()`,
which also blocks for a full OLED I2C transfer, serial logging, and BLE work,
so ping replies could be many milliseconds late. **Fix:** CRSF now runs in its
own FreeRTOS task (priority 5, above `loopTask`); `loop()` takes a
critical-section snapshot of the channels. After this the stream ran without
interruption.

Symptom to recognise: `CRSF frames=` stops increasing while `freshness_ms`
grows; after a XIAO reset it stays at 0 until the radio is power-cycled or
External RF is toggled off/on.

### 3. GATT bank 0 (CH1..CH8) was almost never delivered
`sendReports()` did `setValue(bank0); notify(); setValue(bank1); notify();`.
NimBLE's argument-less `notify()` sends the characteristic's value at the time
the packet is built, which was already overwritten with bank 1, so the PC
received bank 1 twice. The visualizer could rarely assemble a full frame and
looked frozen/jittery. **Fix:** `notify(pkt, sizeof(pkt))` sends each buffer
explicitly (same for the HID report).

### 4. USB serial logging stalled `loop()` for ~2 s per print
With USB plugged in but no terminal reading, HWCDC `Serial.printf` waits up to
~2 s per write before giving up. BLE reports then went out roughly every 2 s.
It looked fine whenever a serial monitor was open. **Fix:**
`Serial.setTxTimeoutMs(0)` so log output is dropped instead of blocking.

### 5. Python visualizer could not find the bridge while Windows was connected
The firmware stopped advertising once a host connected, and bleak needs to
see an advertisement before it can attach. **Fix:** advertising restarts in
`onConnect` (NimBLE allows multiple connections), so `monitor.py --address`
can attach while the OS HID driver holds the link.

Remaining Windows quirk: a bleak connect sometimes reports "disconnected"
right after skipping the HID service (access denied) and the custom service
is missing, while the board stays connected. It is intermittent: a retry a
few seconds later usually succeeds, and `monitor.py` retries automatically.
Running two bleak clients (e.g. the visualizer plus a test script) at once
made it fail every time; use one GATT client at a time. Root cause not
identified.

### 6. OLED I2C clock was silently 400 kHz
U8g2 overrides `Wire.setClock()` with its own default in `begin()`, so the
clock is now set via `oled.setBusClock(OLED_I2C_HZ)`. **1 MHz was tried and
the display stopped working**, so it is back at **400 kHz** (SSD1306 spec
max; the ESP32 I2C peripheral cannot do 2 MHz anyway). Refresh raised from
350 ms to **100 ms** — safe now that CRSF has its own task.

### 7. Hardware: DOWN button (D3) shorted to the 3.3 V rail
Pressing DOWN sagged the whole rail. Wiring fault, fixed by the user.

### 8. OLED transfer blocked `loop()` for ~12 ms per refresh
`sendBuffer()` blocks its caller until the I2C transfer completes (other
FreeRTOS tasks keep running). At the faster BLE rates that delayed reports.
**Fix:** drawing and `sendBuffer()` run in their own task (`oled`, priority
1); `loop()` only polls buttons and hands a `UiStatus` snapshot to the task
under a critical section. Button presses wake the task for an immediate
redraw; otherwise it refreshes every `OLED_REFRESH_MS`.

## Current BLE timing

- Selectable in the menu (`RATE`, SELECT cycles, saved in NVS key
  `bridge/ble_rate`), defined in `BLE_RATES` in `include/config.h`:

  | Option | Report period | Requested interval |
  |---|---|---|
  | `25Hz` (default) | 40 ms | 15–30 ms |
  | `66Hz SAFE` | 15 ms | 15 ms |
  | `125Hz MAX` | 8 ms | 7.5 ms (BLE minimum) |

  The interval is requested on connect and when the setting changes; the
  host decides. Check the `BLE conn params:` serial line for what was granted.
- Per report: 1 HID notification + 2 GATT notifications (banks 0/1, same
  sequence). Measurements below are for the 25 Hz setting: 25 full
  16-channel frames/s, 75 notifications/s total.
- Windows delivered packets in bursts 30–60 ms apart, consistent with a
  ~15 ms connection interval (inferred from arrival timing, not logged by
  the board).
- CRSF arrives at ~250 Hz, so BLE carries roughly every 10th radio frame.

## Bay EXT_TX / EXT_RX pads (2026-10-28)

Question: do the extra bay pads labelled EXT_TX/EXT_RX (from a Zorro adapter
photo) give a normal full-duplex UART, so a plain UART driver (e.g. nRF54) could
read CRSF without the inverted half-duplex single wire?

**Answer: no, not with stock EdgeTX.**

Test (XIAO ESP32-S3, External RF = CRSF, sticks moving, D7 receiving ~250 frames/s):

- EXT_TX → D0 and EXT_RX → D8, each through 1 kΩ, inputs only.
- BAY PROBE counted edges and ran a UART2 CRSF sniffer cycling 420000/115200 baud,
  normal/inverted, on both pins, checking CRC and comparing CH1 with D7.
- D0: steady LOW, 0 edges, 0 bytes. D8: 0 valid frames; edges/bytes only in the
  second the firmware re-attached the UART (floating-pin artefact).
- Confirmed with an oscilloscope: no activity on either pad.

Why (EdgeTX source, `main` branch):

- `radio/src/targets/taranis/hal.h`: the Pocket shares the external-module block
  with Zorro/TX12MK2/Boxer. `EXTMODULE_TX_GPIO` = **PC6** (USART6 TX / TIM8_CH1)
  and `EXTMODULE_RX_GPIO` = **PC7** (USART6 RX).
- `radio/src/boards/generic_stm32/module_ports.cpp`: that UART is described as
  "TX on PPM, RX on HEARTBEAT", i.e. the pads are the bay's **PPM** and
  **heartbeat** pins.
- `radio/src/pulses/crossfire.cpp`: external CRSF is always opened on
  `ETX_MOD_PORT_SPORT` (the single-wire S.Port pin, our D7). Nothing a module does
  moves it to the PPM/heartbeat UART.
- Protocols that do use PPM/heartbeat: PPM, Multi-protocol (100k 8E2 inverted TX,
  telemetry back on S.Port), FrSky PXX1/PXX2 (PXX2 full duplex), SBUS out, AFHDS3.
  Not CRSF or Ghost.
- Full-duplex CRSF on these pins exists only in an unofficial fork targeting
  STM32H7 radios (github.com/m-archibald/edgetx-add-jr-and-trainer-port-full-uarts);
  the Pocket is STM32F4.

Not yet done: setting External RF = PPM and checking the EXT_TX pad shows
PPM pulses, to confirm the pad is routed at all.

Consequence for a future nRF54L15 module:

- CRSF must be taken from the S.Port wire (D7): idle LOW (inverted), half duplex,
  420000 baud.
- nRF UARTE has no RX/TX inversion and no single-wire mode.
- **Receive only** (channels, no telemetry): an inverting buffer (74LVC1G04 or
  one transistor) from the line to UARTE RX works. No Device Ping was seen during
  bring-up (`replies=0`), but EdgeTX may still send one, and an unanswered ping
  can stop CRSF output.
- **Full CRSF** (ping replies, BLEeh telemetry to the radio): needs an
  inverted TX path that only drives the line while replying. Either an inverting
  tri-state buffer (e.g. 74LVC1G240 with OE from a GPIO), or re-routing one pin between
  UART RX and TX for each reply, as CRSFDude does on the ESP32.

## Other changes made during bring-up

- Menu: added **PAIR NEW HOST** (same as BOOT x3); menu scrolls on 128x32.
- OLED top-right BLE icon: plain `B` idle, blinking `B` pairing, inverted box
  when connected.
- Status line adds `replies=` (CRSFDude ping replies) and `notify_fail=`;
  serial also logs GATT subscribe events and connection-parameter updates.

## Workflow tips

- Run `pio ... -t upload` and serial capture in the background and read log
  files; set `PYTHONIOENCODING=utf-8` on Windows or esptool's progress bar can
  crash on the cp1252 console.
- Only one process may hold the COM port; stop serial readers before upload.
- Asserting RTS/DTR from a serial reader can leave the S3 in download mode;
  press RESET if the board goes silent after a scripted reset.
