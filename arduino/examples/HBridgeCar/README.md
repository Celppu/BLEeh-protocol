# HBridgeCar example

Differential-drive car on a dual H-bridge with battery telemetry
([HBridgeCar.ino](HBridgeCar.ino)), using the [BLEeh library](../../README.md).

* Arduino IDE: *File → Examples → BLEeh → HBridgeCar*, board "XIAO_ESP32C3" or "XIAO_ESP32S3".
* PlatformIO: `pio run -d tools/examples_build -e HBridgeCar_c3 -t upload`
  (or `-e HBridgeCar_s3`), then `pio device monitor -b 115200`.

| Function | XIAO pin |
|---|---|
| Left motor IN1 / IN2 | D1 / D3 |
| Right motor IN1 / IN2 | D4 / D5 |
| Battery divider midpoint (100k to B+, 33k to GND) | A2 (D2) |
| Pairing | BOOT button, triple-click |
| Status LED | built-in LED on XIAO ESP32-S3 (none on C3) |

Works with H-bridges that take two PWM inputs per motor (DRV8833, TB6612 in
IN/IN mode, L298N with ENA/ENB tied high). PWM is 20 kHz; both inputs low = coast.

| Radio channel | Use |
|---|---|
| CH1 | Steering (mixed into left/right) |
| CH2 | Throttle, spring-centred stick |
| CH5 | Arm switch: motors only run when high (> 1700 µs) |

Telemetry on the radio: link quality and RSSI, battery voltage and a rough
percentage (2S Li-ion, linear 3.3–4.2 V per cell), and a flight mode text of
`DISARMED` / `ARMED` / `FAILSAFE`. Run *Discover new sensors* in EdgeTX.

Failsafe: after 250 ms without frames (or when the radio is switched off),
all channels go to centre and the motors coast.

**First test:** wheels off the ground, arm switch off. Check the battery reading
with a multimeter and adjust `DIVIDER_RATIO`/`BATTERY_CELLS` for your pack.
Keep the divider output below ~2.5 V at full charge.
