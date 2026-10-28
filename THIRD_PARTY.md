# Third-party software

BLEeh itself is GPL-3.0-or-later ([LICENSE](LICENSE)). It uses the following
projects; all their licenses (Apache-2.0, MIT, BSD-2-Clause, LGPL-2.1-or-later,
PSF) allow use in a GPL-3.0 project. None of their code is copied into this
repository; PlatformIO and pip download them at build/install time. Versions
are the ones used for the tested builds (2026-10-28).

## Firmware (ESP32)

| Project | Version | License | Used by | Link |
|---|---|---|---|---|
| NimBLE-Arduino | 2.5.2 | Apache-2.0 | Library (`arduino/`), Nano-bay firmware | https://github.com/h2zero/NimBLE-Arduino |
| CRSFDude | commit d8063b1 | MIT | Nano-bay firmware (CRSF on the module bay) | https://github.com/rngtng/CRSFDude |
| U8g2 | 2.36.18 | BSD-2-Clause (code); fonts have their own licenses, the firmware uses `u8g2_font_5x7_tf` (X11 misc-fixed, public domain) | Nano-bay firmware (OLED) | https://github.com/olikraus/u8g2 |
| Arduino core for ESP32 | 3.3.11 | LGPL-2.1-or-later | Everything that runs on the ESP32 | https://github.com/espressif/arduino-esp32 |
| ESP-IDF | 5.5 (bundled with the Arduino core) | Apache-2.0 | Everything that runs on the ESP32 | https://github.com/espressif/esp-idf |

The Arduino core is LGPL. Shipping prebuilt firmware binaries is fine as long
as the source of this project stays available, so users can rebuild the
firmware with a modified core.

## PC tools (Python)

| Project | Version | License | Used by | Link |
|---|---|---|---|---|
| bleak | 2.1.1 | MIT | `python/bleeh_tx.py`, `nanobay/python/monitor.py` | https://github.com/hbldh/bleak |
| pytest | 9.1.1 | MIT | Tests only | https://github.com/pytest-dev/pytest |
| Tkinter | Python standard library | PSF | `nanobay/python/monitor.py` | https://docs.python.org/3/library/tkinter.html |

## Referenced, not used as code

* **EdgeTX** (GPL-2.0): its source was read to find out which module-bay pins
  carry CRSF (see `nanobay/docs/BRINGUP_NOTES.md`). No EdgeTX code is included.
