#pragma once
#include <stdint.h>

// All XIAO pin choices refer to PHYSICAL Dn header positions. The GPIO numbers
// differ among SoCs. No strapping GPIOs are used for the 3 external buttons.
// CRSF wiring is unchanged: Pocket Nano-bay signal -> 100 ohm -> D7.
#if defined(CONFIG_IDF_TARGET_ESP32C3)
static constexpr int CRSF_GPIO     = 20;  // XIAO D7
static constexpr int PROBE_EXT_TX_GPIO = 2;  // XIAO D0: C3 GPIO2 is a strapping pin - DO NOT WIRE probes on C3
static constexpr int PROBE_EXT_RX_GPIO = 8;  // XIAO D8: C3 GPIO8 is a strapping pin - DO NOT WIRE probes on C3
static constexpr bool BAY_PROBE_SUPPORTED = false; // Keep original C3 gamepad usable; probes S3-only firmware
static constexpr int OLED_SDA_GPIO = 6;   // XIAO D4
static constexpr int OLED_SCL_GPIO = 7;   // XIAO D5
static constexpr int BTN_UP_GPIO   = 3;   // XIAO D1
static constexpr int BTN_DOWN_GPIO = 5;   // XIAO D3
static constexpr int BTN_OK_GPIO   = 10;  // XIAO D10
static constexpr int PAIR_BUTTON_GPIO = 9; // XIAO C3 BOOT/ISP only
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
static constexpr int CRSF_GPIO     = 44; // D7
static constexpr int PROBE_EXT_TX_GPIO = 1;  // XIAO D0 (ESP32-S3 safe GPIO input)
static constexpr int PROBE_EXT_RX_GPIO = 7;  // XIAO D8 (ESP32-S3 safe GPIO input)
static constexpr bool BAY_PROBE_SUPPORTED = true;
static constexpr int OLED_SDA_GPIO = 5;  // D4
static constexpr int OLED_SCL_GPIO = 6;  // D5
static constexpr int BTN_UP_GPIO   = 2;  // D1
static constexpr int BTN_DOWN_GPIO = 4;  // D3
static constexpr int BTN_OK_GPIO   = 9;  // D10
static constexpr int PAIR_BUTTON_GPIO = 0; // S3 BOOT only (not D9)
#else
#error "Pin mapping not defined for this board. See README pin table."
#endif

static constexpr uint32_t CRSF_BAUD = 420000;
static constexpr uint32_t DEBOUNCE_MS = 35;
static constexpr uint32_t TRIPLE_PRESS_WINDOW_MS = 3000;

// 0.91 inch 128x32 SSD1306 OLED. Most are 0x3C; fallback to 0x3D.
// If the screen is an SH1106 or another controller, change the display class
// in src/ui.cpp; do not assume physical size identifies the controller.
// SSD1306 spec maximum (~12 ms per full frame). 1 MHz stopped the display on
// this module during bring-up; the ESP32 I2C peripheral cannot do 2 MHz.
static constexpr uint32_t OLED_I2C_HZ = 400000;
static constexpr uint32_t OLED_REFRESH_MS = 100;  // CRSF runs in its own task, so 10 Hz is safe
static constexpr uint32_t BLE_ICON_BLINK_MS = 500;

// Selectable BLE report rate (menu "RATE", saved in NVS). Intervals are in
// BLE units of 1.25 ms; the host makes the final choice of connection interval.
struct BleRateOption {
    const char *label;
    uint32_t reportPeriodMs;
    uint16_t minIntervalUnits;
    uint16_t maxIntervalUnits;
};
static constexpr BleRateOption BLE_RATES[] = {
    {"25Hz", 40, 12, 24},       // 15-30 ms interval, lowest power
    {"66Hz SAFE", 15, 12, 12},  // 15 ms: what Windows granted by default
    {"125Hz MAX", 8, 6, 6},     // 7.5 ms: BLE minimum interval
};
static constexpr uint8_t BLE_RATE_COUNT = sizeof(BLE_RATES) / sizeof(BLE_RATES[0]);
static constexpr uint16_t BLE_SUPERVISION_TIMEOUT = 200; // x10 ms = 2 s

// BLE TX power (menu "TX PWR", saved in NVS), applies to gamepad and BLEeh.
// crsfPowerIndex is what EdgeTX shows as TX power (0=0mW 1=10mW 2=25mW 3=100mW).
struct TxPowerOption {
    const char *label;
    int8_t dbm;
    uint8_t crsfPowerIndex;
};
static constexpr TxPowerOption TX_POWERS[] = {
    {"0dBm", 0, 0},
    {"9dBm", 9, 1},
    {"15dBm", 15, 2},
    {"20dBm", 20, 3},  // ESP32-S3/C3 maximum
};
static constexpr uint8_t TX_POWER_COUNT = sizeof(TX_POWERS) / sizeof(TX_POWERS[0]);
static constexpr uint8_t TX_POWER_DEFAULT = 2;  // 15 dBm

// BLEeh (BLEeh mode): RC frames go out on every CRSF frame. While the
// radio stream is stale, frames flagged "not fresh" are sent at this period so
// the receiver enters failsafe immediately instead of waiting for its timeout.
static constexpr uint32_t BLEEH_STALE_RESEND_MS = 100;
static constexpr uint32_t BLEEH_LINK_STATS_MS = 200;  // CRSF link statistics to EdgeTX
static constexpr uint32_t CRSF_STALE_MS = 250;
static constexpr uint32_t STATUS_LOG_PERIOD_MS = 1500;
static constexpr uint32_t PROBE_SAMPLE_MS = 1000; // approximate edges per second (not UART decoding)

static constexpr uint16_t CRSF_RAW_MIN = 172;
static constexpr uint16_t CRSF_RAW_CENTER = 992;
static constexpr uint16_t CRSF_RAW_MAX = 1811;
static constexpr uint16_t BUTTON_THRESHOLD = 1200;

static constexpr const char *BLE_DEVICE_NAME = "Pocket BLE Bridge";
static constexpr const char *RC_SERVICE_UUID = "c09dfb45-9fb3-4bb8-bf25-bd3a77d21b01";
static constexpr const char *RC_CHAR_UUID = "c09dfb45-9fb3-4bb8-bf25-bd3a77d21b02";
