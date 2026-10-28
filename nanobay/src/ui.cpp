#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include "ui.h"

// Most common half-height 0.91" 128x32 I2C OLED controller.
// If using another controller (e.g. SH1106), replace this constructor.
static U8G2_SSD1306_128X32_UNIVISION_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE);

struct MenuItem {
    const char *label;
    UiEvent event;
};
static constexpr MenuItem MENU[] = {
    {"GAMEPAD", UiEvent::ChooseGamepad},
    {"BLEeh", UiEvent::ChooseBleeh},
    {"PAIR RECEIVER", UiEvent::PairReceiver},
    {"PAIR PC GAMEPAD", UiEvent::Pair},
    {"RATE", UiEvent::CycleBleRate},     // labels of settings show the current value
    {"TX PWR", UiEvent::CycleTxPower},
    {"BAY PROBE (S3)", UiEvent::ChoosePinProbe},
};

static bool isSetting(UiEvent e) { return e == UiEvent::CycleBleRate || e == UiEvent::CycleTxPower; }
static constexpr uint8_t MENU_COUNT = sizeof(MENU) / sizeof(MENU[0]);
static constexpr uint8_t MENU_ROWS = 3; // rows below the title on 128x32

static bool probeAddress(uint8_t addr) {
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
}

void UiController::begin() {
    pinMode(BTN_UP_GPIO, INPUT_PULLUP);
    pinMode(BTN_DOWN_GPIO, INPUT_PULLUP);
    pinMode(BTN_OK_GPIO, INPUT_PULLUP);

    Wire.begin(OLED_SDA_GPIO, OLED_SCL_GPIO);
    Wire.setClock(OLED_I2C_HZ);
    uint8_t address = probeAddress(0x3c) ? 0x3c : (probeAddress(0x3d) ? 0x3d : 0);
    if (!address) {
        Serial.println("OLED absent at I2C 0x3C/0x3D; controls still run.");
        return;
    }
    oled.setI2CAddress(address << 1); // U8g2 expects the 8-bit address
    oled.setBusClock(OLED_I2C_HZ);    // U8g2 overrides Wire.setClock() otherwise
    oled.begin();
    oled.setFont(u8g2_font_5x7_tf);
    oled.setFontPosTop();
    displayPresent_ = true;
    Serial.printf("OLED initialized at 0x%02X: SSD1306 128x32\n", address);
    // Priority 1 = same as loopTask, below the CRSF task (5). The task sleeps
    // while the I2C driver waits, so loop() keeps running during transfers.
    xTaskCreate(taskEntry, "oled", 4096, this, 1, &task_);
}

void UiController::setStatus(const UiStatus &status) {
    portENTER_CRITICAL(&mux_);
    status_ = status;
    portEXIT_CRITICAL(&mux_);
}

void UiController::wakeTask() {
    if (task_) xTaskNotifyGive(task_); // redraw now instead of at the next tick
}

UiEvent UiController::poll(uint32_t now) {
    // Poll even if OLED absent (buttons still useful with serial debug).
    const bool upPressed = up_.update(digitalRead(BTN_UP_GPIO) != LOW, now);
    const bool downPressed = down_.update(digitalRead(BTN_DOWN_GPIO) != LOW, now);
    const bool okPressed = ok_.update(digitalRead(BTN_OK_GPIO) != LOW, now);
    if (!upPressed && !downPressed && !okPressed) return UiEvent::None;

    UiEvent event = UiEvent::None;
    portENTER_CRITICAL(&mux_);
    if (!menuOpen_) {
        if (okPressed) menuOpen_ = true;
    } else {
        if (upPressed || downPressed)
            selection_ = (uint8_t)((selection_ + (downPressed ? 1 : MENU_COUNT - 1)) % MENU_COUNT);
        if (okPressed) {
            event = MENU[selection_].event;
            // Settings stay in the menu so repeated SELECT cycles the value.
            if (!isSetting(event)) menuOpen_ = false;
        }
    }
    const uint8_t selected = selection_;
    portEXIT_CRITICAL(&mux_);

    if (event != UiEvent::None) Serial.printf("UI menu selected: %s\n", MENU[selected].label);
    wakeTask();
    return event;
}

static void printLine(uint8_t row, const char *line) {
    oled.setCursor(0, row * 8);
    oled.print(line);
}

// Top-right BLE icon: solid box = connected, blinking B = pairing, plain B = idle.
static void drawBleIcon(uint32_t now, const UiStatus &s) {
    constexpr uint8_t x = 119, w = 9, h = 8;
    oled.setDrawColor(0);
    oled.drawBox(x, 0, w, h); // clear any title text under the icon
    oled.setDrawColor(1);
    if (s.bleConnected) {
        oled.drawBox(x, 0, w, h);
        oled.setDrawColor(0);
        oled.setCursor(x + 2, 0);
        oled.print('B');
        oled.setDrawColor(1);
    } else if (!s.pairingMode || (now / BLE_ICON_BLINK_MS) % 2 == 0) {
        oled.setCursor(x + 2, 0);
        oled.print('B');
    }
}

static void draw(uint32_t now, const UiStatus &s, bool menuOpen, uint8_t selection);

void UiController::taskEntry(void *self) {
    static_cast<UiController *>(self)->taskLoop();
}

void UiController::taskLoop() {
    for (;;) {
        UiStatus s;
        bool menuOpen;
        uint8_t selection;
        portENTER_CRITICAL(&mux_);
        s = status_;
        menuOpen = menuOpen_;
        selection = selection_;
        portEXIT_CRITICAL(&mux_);
        draw(millis(), s, menuOpen, selection);
        // Sleep until the next refresh, or until a button press wakes us.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(OLED_REFRESH_MS));
    }
}

static void draw(uint32_t now, const UiStatus &s, bool menuOpen, uint8_t selection) {
    char line[32];
    oled.clearBuffer();
    if (menuOpen) {
        printLine(0, "SELECT MODE  UP/DOWN");
        // Scroll so the selected item is always visible.
        const uint8_t first = selection < MENU_ROWS ? 0 : selection - (MENU_ROWS - 1);
        for (uint8_t row = 0; row < MENU_ROWS && first + row < MENU_COUNT; ++row) {
            const uint8_t i = first + row;
            if (MENU[i].event == UiEvent::CycleBleRate)
                snprintf(line, sizeof(line), "%c RATE: %s", i == selection ? '>' : ' ', s.bleRateLabel);
            else if (MENU[i].event == UiEvent::CycleTxPower)
                snprintf(line, sizeof(line), "%c TX PWR: %s", i == selection ? '>' : ' ', s.txPowerLabel);
            else
                snprintf(line, sizeof(line), "%c %s", i == selection ? '>' : ' ', MENU[i].label);
            printLine(row + 1, line);
        }
    } else if (s.mode == BridgeMode::Gamepad) {
        printLine(0, "POCKET BLE   GAMEPAD");
        snprintf(line, sizeof(line), "RF:%s BLE:%s%s",
                 s.crsfFresh ? "OK" : "--",
                 s.bleConnected ? "LINK" : "----",
                 s.pairingMode ? " PAIR" : "");
        printLine(1, line);
        snprintf(line, sizeof(line), "CH1:%4u CH2:%4u", s.ch1, s.ch2);
        printLine(2, line);
        printLine(3, "SEL:MODES BOOTx3:PAIR");
    } else if (s.mode == BridgeMode::PinProbe) {
        printLine(0, "BAY PROBE  RX ONLY");
        snprintf(line, sizeof(line), "EXT_TX D0:%c %lu/s", s.extTxLevel ? 'H' : 'L', (unsigned long)s.extTxEdgesPerSecond);
        printLine(1, line);
        snprintf(line, sizeof(line), "EXT_RX D8:%c %lu/s", s.extRxLevel ? 'H' : 'L', (unsigned long)s.extRxEdgesPerSecond);
        printLine(2, line);
        printLine(3, "SEL:MODES  USB:DETAIL");
    } else {
        const char *state = s.bleehPairing ? "PAIRING" :
                            !s.bleehHasReceiver ? "NO RX" :
                            s.bleehConnected ? (s.bleehRxFailsafe ? "FAILSAFE" : "LINK") : "SEARCH";
        snprintf(line, sizeof(line), "BLEeh  %s", state);
        printLine(0, line);
        if (s.bleehConnected) {
            snprintf(line, sizeof(line), "RF:%s LQ:%3u %4ddBm", s.crsfFresh ? "OK" : "--",
                     s.bleehUplinkLq, s.bleehUplinkRssi);
            printLine(1, line);
            if (s.bleehBatteryValid)
                snprintf(line, sizeof(line), "RX BAT %2u.%02uV TQ:%3u",
                         s.bleehBatteryMv / 1000, (s.bleehBatteryMv % 1000) / 10, s.bleehDownlinkLq);
            else
                snprintf(line, sizeof(line), "RX BAT --.--V TQ:%3u", s.bleehDownlinkLq);
            printLine(2, line);
        } else {
            snprintf(line, sizeof(line), "RF:%s", s.crsfFresh ? "OK" : "--");
            printLine(1, line);
            printLine(2, s.bleehPairing ? "TRIPLE-CLICK RX BTN" :
                         (s.bleehHasReceiver ? "WAITING FOR RX..." : "SEL>PAIR RECEIVER"));
        }
        printLine(3, "SEL:MODES");
    }
    if (!menuOpen && s.mode == BridgeMode::Gamepad) drawBleIcon(now, s);
    oled.sendBuffer(); // blocks this task only (~12 ms at 400 kHz)
}
