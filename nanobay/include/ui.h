#pragma once
#include <stdint.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "button_debounce.h"
#include "config.h"

enum class BridgeMode : uint8_t { Gamepad = 0, Bleeh = 1, PinProbe = 2 };
enum class UiEvent : uint8_t {
    None, ChooseGamepad, ChooseBleeh, ChoosePinProbe, Pair, PairReceiver, CycleBleRate, CycleTxPower
};

struct UiStatus {
    BridgeMode mode = BridgeMode::Gamepad;
    bool crsfFresh = false;
    bool bleConnected = false;
    bool pairingMode = false;
    uint16_t ch1 = 0;
    uint16_t ch2 = 0;
    uint32_t extTxEdgesPerSecond = 0;
    uint32_t extRxEdgesPerSecond = 0;
    bool extTxLevel = false;
    bool extRxLevel = false;
    const char *bleRateLabel = "";
    const char *txPowerLabel = "";
    // BLEeh mode (BLEeh receiver)
    bool bleehConnected = false;
    bool bleehPairing = false;
    bool bleehHasReceiver = false;
    bool bleehRxFailsafe = false;
    uint8_t bleehUplinkLq = 0;
    int8_t bleehUplinkRssi = 0;
    uint8_t bleehDownlinkLq = 0;
    bool bleehBatteryValid = false;
    uint16_t bleehBatteryMv = 0;
};

// Buttons are polled from loop(); drawing and the blocking I2C transfer run in
// a low-priority task so loop() (BLE reports) never waits on the display.
class UiController {
public:
    void begin();
    UiEvent poll(uint32_t now);
    void setStatus(const UiStatus& status);
    bool isPresent() const { return displayPresent_; }

private:
    static void taskEntry(void *self);
    void taskLoop();
    void wakeTask();

    DebouncedButton up_{DEBOUNCE_MS};
    DebouncedButton down_{DEBOUNCE_MS};
    DebouncedButton ok_{DEBOUNCE_MS};
    bool displayPresent_ = false;
    TaskHandle_t task_ = nullptr;

    // Shared between loop() and the OLED task; guarded by mux_.
    portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
    UiStatus status_;
    bool menuOpen_ = false;
    uint8_t selection_ = 0;
};
